#include "hnsw.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>

namespace flux {

// ============================================================
// Construction
// ============================================================

HNSWIndex::HNSWIndex(DistanceMetric metric, int dimension, Options opts)
    : metric_(metric)
    , dimension_(dimension)
    , m_(opts.m)
    , mmax0_(opts.mmax0)
    , ef_construction_(opts.ef_construction)
    , ef_search_(opts.ef_search)
    , ml_(1.0 / std::log(static_cast<double>(opts.m)))
    , rng_(42) // Fixed seed for deterministic behavior
{
    if (dimension <= 0) throw std::invalid_argument("dimension must be positive");
    if (m_ <= 0) throw std::invalid_argument("M must be positive");
}

// ============================================================
// Level generation
// ============================================================

int HNSWIndex::generate_level() {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    double r = dist(rng_);
    if (r == 0.0) r = std::numeric_limits<double>::min();
    int level = static_cast<int>(std::floor(-std::log(r) * ml_));
    if (level < 0) level = 0;
    if (level > 64) level = 64;
    return level;
}

// ============================================================
// Distance helpers
// ============================================================

static double min_distance_entry(const std::unordered_map<std::string, double>& m,
                                  std::string& out_id) {
    out_id.clear();
    double best = std::numeric_limits<double>::max();
    for (const auto& [id, d] : m) {
        if (d < best) { best = d; out_id = id; }
    }
    return best;
}

static double max_distance_entry(const std::unordered_map<std::string, double>& m,
                                  std::string& out_id) {
    out_id.clear();
    double best = std::numeric_limits<double>::lowest();
    for (const auto& [id, d] : m) {
        if (d > best) { best = d; out_id = id; }
    }
    return best;
}

// ============================================================
// Search Layer (Algorithm 2 from HNSW paper)
// ============================================================

std::unordered_map<std::string, double> HNSWIndex::search_layer(
    const double* query,
    const std::vector<std::string>& entry_points,
    int ef, int layer) const {

    std::unordered_set<std::string> visited;
    std::unordered_map<std::string, double> candidates; // frontier
    std::unordered_map<std::string, double> results;    // top-ef results

    // Initialize with entry points
    for (const auto& ep : entry_points) {
        if (visited.count(ep) || deleted_.count(ep)) continue;
        visited.insert(ep);

        auto it = nodes_.find(ep);
        if (it == nodes_.end()) continue;

        double d = dist(query, it->second.vector.data());
        candidates[ep] = d;
        results[ep] = d;
    }

    while (!candidates.empty()) {
        // Find closest candidate
        std::string c_id;
        double c_dist = min_distance_entry(candidates, c_id);
        candidates.erase(c_id);

        // Find farthest result
        std::string f_id;
        double f_dist = max_distance_entry(results, f_id);

        // Early termination
        if (c_dist > f_dist) break;

        auto node_it = nodes_.find(c_id);
        if (node_it == nodes_.end()) continue;

        // Explore neighbors at this layer
        if (layer >= static_cast<int>(node_it->second.connections.size())) continue;

        for (const auto& neighbor_id : node_it->second.connections[layer]) {
            if (visited.count(neighbor_id) || deleted_.count(neighbor_id)) continue;
            visited.insert(neighbor_id);

            auto n_it = nodes_.find(neighbor_id);
            if (n_it == nodes_.end()) continue;

            double d = dist(query, n_it->second.vector.data());

            if (!results.count(neighbor_id)) {
                if (static_cast<int>(results.size()) < ef || d < f_dist) {
                    candidates[neighbor_id] = d;
                    results[neighbor_id] = d;

                    // Prune
                    if (static_cast<int>(results.size()) > ef) {
                        std::string rm_id;
                        max_distance_entry(results, rm_id);
                        results.erase(rm_id);
                    }
                    if (!results.empty()) {
                        f_dist = max_distance_entry(results, f_id);
                    }
                }
            }
        }
    }

    return results;
}

// ============================================================
// Greedy Search Layer
// ============================================================

std::string HNSWIndex::greedy_search_layer(
    const double* query, const std::string& entry_point, int layer) const {

    std::string curr_id = entry_point;

    while (true) {
        bool changed = false;
        auto curr_it = nodes_.find(curr_id);
        if (curr_it == nodes_.end()) break;

        double curr_dist = dist(query, curr_it->second.vector.data());

        if (layer >= static_cast<int>(curr_it->second.connections.size())) break;

        for (const auto& neighbor_id : curr_it->second.connections[layer]) {
            if (deleted_.count(neighbor_id)) continue;

            auto n_it = nodes_.find(neighbor_id);
            if (n_it == nodes_.end()) continue;

            double d = dist(query, n_it->second.vector.data());
            if (d < curr_dist) {
                curr_dist = d;
                curr_id = neighbor_id;
                changed = true;
            }
        }

        if (!changed) break;
    }

    return curr_id;
}

// ============================================================
// Neighbor Selection
// ============================================================

std::vector<std::string> HNSWIndex::select_neighbors_simple(
    const std::unordered_map<std::string, double>& cands, int k) const {

    if (static_cast<int>(cands.size()) <= k) {
        std::vector<std::string> ids;
        ids.reserve(cands.size());
        for (const auto& [id, _] : cands) ids.push_back(id);
        return ids;
    }

    std::vector<DistCandidate> pairs;
    pairs.reserve(cands.size());
    for (const auto& [id, d] : cands) {
        pairs.push_back({id, d});
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const DistCandidate& a, const DistCandidate& b) { return a.dist < b.dist; });

    std::vector<std::string> ids(k);
    for (int i = 0; i < k; i++) ids[i] = pairs[i].doc_id;
    return ids;
}

// ============================================================
// Connection Management
// ============================================================

void HNSWIndex::add_connections(const std::string& node_id,
                                  const std::vector<std::string>& neighbors,
                                  int layer) {
    auto node_it = nodes_.find(node_id);
    if (node_it == nodes_.end()) return;
    auto& node = node_it->second;

    int max_conn = (layer > 0) ? m_ : mmax0_;

    // Ensure connections vector is large enough
    while (static_cast<int>(node.connections.size()) <= layer) {
        node.connections.emplace_back();
    }

    // Add forward connections
    for (const auto& neighbor_id : neighbors) {
        if (neighbor_id == node_id) continue;
        node.connections[layer].push_back(neighbor_id);
    }

    // Shrink if over capacity
    if (static_cast<int>(node.connections[layer].size()) > max_conn) {
        shrink_connections(node_id, layer, max_conn);
    }

    // Add reverse connections (neighbor -> node_id)
    for (const auto& neighbor_id : neighbors) {
        if (neighbor_id == node_id) continue;

        auto n_it = nodes_.find(neighbor_id);
        if (n_it == nodes_.end()) continue;

        while (static_cast<int>(n_it->second.connections.size()) <= layer) {
            n_it->second.connections.emplace_back();
        }

        n_it->second.connections[layer].push_back(node_id);

        if (static_cast<int>(n_it->second.connections[layer].size()) > max_conn) {
            shrink_connections(neighbor_id, layer, max_conn);
        }
    }
}

void HNSWIndex::shrink_connections(const std::string& node_id, int layer, int max_conn) {
    auto node_it = nodes_.find(node_id);
    if (node_it == nodes_.end()) return;

    auto& conns = node_it->second.connections[layer];
    if (static_cast<int>(conns.size()) <= max_conn) return;

    std::vector<DistCandidate> pairs;
    const auto& node_vec = node_it->second.vector;

    for (const auto& neighbor_id : conns) {
        auto n_it = nodes_.find(neighbor_id);
        if (n_it == nodes_.end() || deleted_.count(neighbor_id)) continue;
        double d = dist(node_vec.data(), n_it->second.vector.data());
        pairs.push_back({neighbor_id, d});
    }

    std::sort(pairs.begin(), pairs.end(),
              [](const DistCandidate& a, const DistCandidate& b) { return a.dist < b.dist; });

    std::vector<std::string> result;
    result.reserve(max_conn);
    for (int i = 0; i < max_conn && i < static_cast<int>(pairs.size()); i++) {
        result.push_back(pairs[i].doc_id);
    }
    conns = std::move(result);
}

// ============================================================
// Insert
// ============================================================

void HNSWIndex::insert(const std::string& doc_id, const std::vector<double>& vector) {
    if (static_cast<int>(vector.size()) != dimension_) {
        throw std::invalid_argument("dimension mismatch: expected " +
                                    std::to_string(dimension_) + ", got " +
                                    std::to_string(vector.size()));
    }
    if (doc_id.empty()) throw std::invalid_argument("doc_id is required");

    std::unique_lock lock(mu_);

    // Handle update: delete existing node
    if (nodes_.count(doc_id)) {
        internal_delete(doc_id);
    }

    int level = generate_level();
    Node node;
    node.id = doc_id;
    node.vector = vector;
    node.level = level;
    node.connections.resize(level + 1);

    nodes_[doc_id] = node;
    deleted_.erase(doc_id);

    // First node: becomes entry point
    if (entry_point_.empty() || max_level_ < 0) {
        entry_point_ = doc_id;
        max_level_ = level;
        return;
    }

    // Step 1: Traverse upper layers greedily from the entry point
    std::string curr_entry = entry_point_;
    for (int lc = max_level_; lc > level; lc--) {
        curr_entry = greedy_search_layer(vector.data(), curr_entry, lc);
    }

    // Step 2: Search and connect at each layer from min(level, max_level) down to 0
    std::vector<std::string> entry_set = {curr_entry};
    int start_layer = std::min(level, max_level_);

    for (int lc = start_layer; lc >= 0; lc--) {
        auto candidates = search_layer(vector.data(), entry_set, ef_construction_, lc);
        auto neighbors = select_neighbors_simple(candidates, m_);
        add_connections(doc_id, neighbors, lc);

        // Use the closest candidate as entry point for next layer
        if (!candidates.empty()) {
            std::string closest;
            min_distance_entry(candidates, closest);
            entry_set = {closest};
        }
    }

    // Step 3: Update global entry point
    if (level > max_level_) {
        max_level_ = level;
        entry_point_ = doc_id;
    }
}

// ============================================================
// Delete
// ============================================================

void HNSWIndex::remove(const std::string& doc_id) {
    std::unique_lock lock(mu_);
    internal_delete(doc_id);
}

void HNSWIndex::internal_delete(const std::string& doc_id) {
    if (!nodes_.count(doc_id)) return;
    deleted_.insert(doc_id);
}

// ============================================================
// SearchInternal
// ============================================================

std::vector<SearchResult> HNSWIndex::search_internal(
    const std::vector<double>& query, int k) {

    if (static_cast<int>(query.size()) != dimension_) {
        return {};
    }

    std::shared_lock lock(mu_);
    if (nodes_.empty() || entry_point_.empty()) return {};

    // Phase 1: Top-layer greedy traversal
    std::string curr_entry = entry_point_;
    for (int lc = max_level_; lc > 0; lc--) {
        curr_entry = greedy_search_layer(query.data(), curr_entry, lc);
    }

    // Phase 2: Bottom-layer search with efSearch
    auto candidates = search_layer(query.data(), {curr_entry}, ef_search_, 0);

    // Phase 3: Select top-k
    std::vector<DistCandidate> pairs;
    pairs.reserve(candidates.size());
    for (const auto& [id, d] : candidates) {
        if (deleted_.count(id)) continue;
        pairs.push_back({id, d});
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const DistCandidate& a, const DistCandidate& b) { return a.dist < b.dist; });

    std::vector<SearchResult> results;
    results.reserve(std::min(k, static_cast<int>(pairs.size())));
    for (int i = 0; i < static_cast<int>(pairs.size()) && static_cast<int>(results.size()) < k; i++) {
        double score = -pairs[i].dist; // Negate distance to get similarity score
        results.push_back({pairs[i].doc_id, score});
    }

    return results;
}

// ============================================================
// SearchInternalWithFilter
// ============================================================

std::vector<SearchResult> HNSWIndex::search_internal_with_filter(
    const std::vector<double>& query, int k,
    const std::unordered_set<std::string>& cand_set) {

    if (static_cast<int>(query.size()) != dimension_) return {};
    if (cand_set.empty()) return {};

    std::shared_lock lock(mu_);
    if (nodes_.empty() || entry_point_.empty()) return {};

    // Phase 1: Top-layer greedy traversal
    std::string curr_entry = entry_point_;
    for (int lc = max_level_; lc > 0; lc--) {
        curr_entry = greedy_search_layer(query.data(), curr_entry, lc);
    }

    // Phase 2: Bottom-layer search
    auto candidates = search_layer(query.data(), {curr_entry}, ef_search_, 0);

    // Phase 3: Filter by candidate set
    std::vector<DistCandidate> pairs;
    for (const auto& [id, d] : candidates) {
        if (deleted_.count(id)) continue;
        if (!cand_set.count(id)) continue;
        pairs.push_back({id, d});
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const DistCandidate& a, const DistCandidate& b) { return a.dist < b.dist; });

    std::vector<SearchResult> results;
    results.reserve(std::min(k, static_cast<int>(pairs.size())));
    for (int i = 0; i < static_cast<int>(pairs.size()) && static_cast<int>(results.size()) < k; i++) {
        results.push_back({pairs[i].doc_id, -pairs[i].dist});
    }

    return results;
}

// ============================================================
// Size
// ============================================================

size_t HNSWIndex::size() const {
    std::shared_lock lock(mu_);
    return nodes_.size() - deleted_.size();
}

// ============================================================
// Save / Load (JSON snapshot)
// ============================================================

void HNSWIndex::save(std::ostream& os) const {
    std::shared_lock lock(mu_);

    nlohmann::json j;
    j["metric"] = metric_to_string(metric_);
    j["dimension"] = dimension_;
    j["m"] = m_;
    j["mmax0"] = mmax0_;
    j["ef_construction"] = ef_construction_;
    j["ef_search"] = ef_search_;
    j["entry_point"] = entry_point_;
    j["max_level"] = max_level_;

    // Nodes
    auto& jnodes = j["nodes"];
    jnodes = nlohmann::json::object();
    for (const auto& [id, node] : nodes_) {
        nlohmann::json jn;
        jn["id"] = node.id;
        jn["vector"] = node.vector;
        jn["level"] = node.level;
        jn["connections"] = node.connections;
        jnodes[id] = jn;
    }

    // Deleted set
    j["deleted_ids"] = std::vector<std::string>(deleted_.begin(), deleted_.end());

    os << j.dump(2);
}

void HNSWIndex::load(std::istream& is) {
    nlohmann::json j;
    is >> j;

    std::unique_lock lock(mu_);

    metric_ = metric_from_string(j["metric"].get<std::string>());
    dimension_ = j["dimension"];
    m_ = j["m"];
    mmax0_ = j["mmax0"];
    ef_construction_ = j["ef_construction"];
    ef_search_ = j["ef_search"];
    entry_point_ = j["entry_point"];
    max_level_ = j["max_level"];

    ml_ = 1.0 / std::log(static_cast<double>(m_));

    // Restore nodes
    nodes_.clear();
    for (auto it = j["nodes"].begin(); it != j["nodes"].end(); ++it) {
        Node node;
        node.id = it.value()["id"];
        node.vector = it.value()["vector"].get<std::vector<double>>();
        node.level = it.value()["level"];
        node.connections = it.value()["connections"].get<std::vector<std::vector<std::string>>>();
        nodes_[node.id] = std::move(node);
    }

    // Restore deleted set
    deleted_.clear();
    for (const auto& id : j["deleted_ids"]) {
        deleted_.insert(id.get<std::string>());
    }
}

} // namespace flux
