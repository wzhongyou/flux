#include "ivf.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace flux {

// ============================================================
// Construction
// ============================================================

IVFIndex::IVFIndex(DistanceMetric metric, int dimension, Options opts)
    : metric_(metric)
    , dimension_(dimension)
    , num_centroids_(opts.num_centroids)
    , n_probe_(opts.n_probe)
    , max_iter_(opts.max_iter)
    , rng_(42)
{
    if (dimension <= 0) throw std::invalid_argument("dimension must be positive");
    if (num_centroids_ <= 0) throw std::invalid_argument("num_centroids must be positive");
}

// ============================================================
// Distance (Euclidean, for K-means)
// ============================================================

double IVFIndex::vector_dist(const std::vector<double>& a, const std::vector<double>& b) const {
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); i++) {
        double delta = a[i] - b[i];
        sum += delta * delta;
    }
    return std::sqrt(sum);
}

int IVFIndex::find_nearest_centroid(const std::vector<double>& vector) const {
    if (centroids_.empty()) return -1;

    int best = 0;
    double best_dist = std::numeric_limits<double>::max();

    for (size_t c = 0; c < centroids_.size(); c++) {
        double d = vector_dist(vector, centroids_[c]);
        if (d < best_dist) {
            best_dist = d;
            best = static_cast<int>(c);
        }
    }
    return best;
}

// ============================================================
// K-means Clustering
// ============================================================

void IVFIndex::build_clusters() {
    std::vector<std::vector<double>> all_vectors;
    all_vectors.reserve(nodes_.size());
    for (const auto& [_, node] : nodes_) {
        all_vectors.push_back(node.vector);
    }

    int n = static_cast<int>(all_vectors.size());
    if (n == 0) return;

    int k = num_centroids_;
    if (k > n) k = n;

    // ---- K-means++ initialization ----
    centroids_.resize(k);

    // First centroid: random
    std::uniform_int_distribution<int> idx_dist(0, n - 1);
    centroids_[0] = all_vectors[idx_dist(rng_)];

    for (int c = 1; c < k; c++) {
        std::vector<double> dists(n);
        double total_dist = 0.0;

        for (int i = 0; i < n; i++) {
            double min_d = std::numeric_limits<double>::max();
            for (int j = 0; j < c; j++) {
                double d = vector_dist(all_vectors[i], centroids_[j]);
                if (d < min_d) min_d = d;
            }
            dists[i] = min_d * min_d;
            total_dist += dists[i];
        }

        std::uniform_real_distribution<double> prob_dist(0.0, total_dist);
        double t = prob_dist(rng_);
        double cum = 0.0;
        int chosen = 0;
        for (int i = 0; i < n; i++) {
            cum += dists[i];
            if (cum >= t) { chosen = i; break; }
        }
        centroids_[c] = all_vectors[chosen];
    }

    // ---- Iterate K-means ----
    std::vector<int> assignments(n, 0);
    for (int iter = 0; iter < max_iter_; iter++) {
        bool changed = false;

        // Assignment step
        for (int i = 0; i < n; i++) {
            int best_c = 0;
            double best_d = std::numeric_limits<double>::max();
            for (int c = 0; c < k; c++) {
                double d = vector_dist(all_vectors[i], centroids_[c]);
                if (d < best_d) {
                    best_d = d;
                    best_c = c;
                }
            }
            if (assignments[i] != best_c) {
                assignments[i] = best_c;
                changed = true;
            }
        }

        if (!changed) break;

        // Update step
        std::vector<int> counts(k, 0);
        std::vector<std::vector<double>> sums(k, std::vector<double>(dimension_, 0.0));

        for (int i = 0; i < n; i++) {
            int c = assignments[i];
            for (int d = 0; d < dimension_; d++) {
                sums[c][d] += all_vectors[i][d];
            }
            counts[c]++;
        }

        for (int c = 0; c < k; c++) {
            if (counts[c] > 0) {
                for (int d = 0; d < dimension_; d++) {
                    centroids_[c][d] = sums[c][d] / counts[c];
                }
            }
        }
    }
}

// ============================================================
// Build
// ============================================================

void IVFIndex::build() {
    std::unique_lock lock(mu_);
    if (nodes_.empty()) return;

    build_clusters();

    // Build inverted lists
    int k = static_cast<int>(centroids_.size());
    inverted_lists_.resize(k);
    for (size_t i = 0; i < inverted_lists_.size(); i++) {
        inverted_lists_[i].clear();
    }

    for (const auto& [doc_id, node] : nodes_) {
        int c = find_nearest_centroid(node.vector);
        if (c >= 0 && c < k) {
            inverted_lists_[c].push_back(doc_id);
        }
    }
}

// ============================================================
// Insert / Delete
// ============================================================

void IVFIndex::insert(const std::string& doc_id, const std::vector<double>& vector) {
    if (static_cast<int>(vector.size()) != dimension_) {
        throw std::invalid_argument("dimension mismatch");
    }
    if (doc_id.empty()) throw std::invalid_argument("doc_id is required");

    std::unique_lock lock(mu_);

    // Upsert: remove old entry
    if (nodes_.count(doc_id)) {
        // Remove from inverted list
        if (!centroids_.empty()) {
            int old_c = find_nearest_centroid(nodes_[doc_id].vector);
            if (old_c >= 0 && old_c < static_cast<int>(inverted_lists_.size())) {
                auto& list = inverted_lists_[old_c];
                auto it = std::find(list.begin(), list.end(), doc_id);
                if (it != list.end()) list.erase(it);
            }
        }
    }

    nodes_[doc_id] = {doc_id, vector};

    // Add to inverted list if centroids exist
    if (!centroids_.empty()) {
        int c = find_nearest_centroid(vector);
        if (c >= 0 && c < static_cast<int>(inverted_lists_.size())) {
            inverted_lists_[c].push_back(doc_id);
        }
    }
}

void IVFIndex::remove(const std::string& doc_id) {
    std::unique_lock lock(mu_);
    nodes_.erase(doc_id);
    // Note: entry is left in the inverted list; skipped during search
    // since the node is no longer in nodes_.
}

// ============================================================
// Search
// ============================================================

std::vector<SearchResult> IVFIndex::search_internal(
    const std::vector<double>& query, int k) {

    if (static_cast<int>(query.size()) != dimension_) return {};

    std::shared_lock lock(mu_);
    if (nodes_.empty() || centroids_.empty()) return {};

    // Step 1: Find closest centroids
    struct CentroidDist {
        int idx = 0;
        double dist = 0.0;
    };
    std::vector<CentroidDist> cdists;
    cdists.reserve(centroids_.size());

    for (size_t c = 0; c < centroids_.size(); c++) {
        cdists.push_back({static_cast<int>(c), vector_dist(query, centroids_[c])});
    }
    std::sort(cdists.begin(), cdists.end(),
              [](const CentroidDist& a, const CentroidDist& b) { return a.dist < b.dist; });

    // Step 2: Search inverted lists of closest nProbe centroids
    int n_probe = std::min(n_probe_, static_cast<int>(cdists.size()));

    std::unordered_set<std::string> visited;
    std::vector<DistCandidate> candidates;

    for (int p = 0; p < n_probe; p++) {
        int c = cdists[p].idx;
        if (c >= static_cast<int>(inverted_lists_.size())) continue;

        for (const auto& doc_id : inverted_lists_[c]) {
            if (visited.count(doc_id)) continue;
            visited.insert(doc_id);

            auto it = nodes_.find(doc_id);
            if (it == nodes_.end()) continue;

            double score = compute_score(metric_, query, it->second.vector);
            candidates.push_back({doc_id, -score}); // store negative score as "distance"
        }
    }

    // Step 3: Sort and select top-k
    std::sort(candidates.begin(), candidates.end(),
              [](const DistCandidate& a, const DistCandidate& b) { return a.dist < b.dist; });

    std::vector<SearchResult> results;
    results.reserve(std::min(k, static_cast<int>(candidates.size())));
    for (int i = 0; i < static_cast<int>(candidates.size()) &&
         static_cast<int>(results.size()) < k; i++) {
        results.push_back({candidates[i].doc_id, -candidates[i].dist});
    }

    return results;
}

// ============================================================
// Search with Filter
// ============================================================

std::vector<SearchResult> IVFIndex::search_internal_with_filter(
    const std::vector<double>& query, int k,
    const std::unordered_set<std::string>& candidates) {

    if (static_cast<int>(query.size()) != dimension_) return {};
    if (candidates.empty()) return {};

    std::shared_lock lock(mu_);
    if (nodes_.empty() || centroids_.empty()) return {};

    // Find closest centroids
    struct CentroidDist {
        int idx = 0;
        double dist = 0.0;
    };
    std::vector<CentroidDist> cdists;
    cdists.reserve(centroids_.size());

    for (size_t c = 0; c < centroids_.size(); c++) {
        cdists.push_back({static_cast<int>(c), vector_dist(query, centroids_[c])});
    }
    std::sort(cdists.begin(), cdists.end(),
              [](const CentroidDist& a, const CentroidDist& b) { return a.dist < b.dist; });

    int n_probe = std::min(n_probe_, static_cast<int>(cdists.size()));

    std::unordered_set<std::string> visited;
    std::vector<DistCandidate> filtered;

    for (int p = 0; p < n_probe; p++) {
        int c = cdists[p].idx;
        if (c >= static_cast<int>(inverted_lists_.size())) continue;

        for (const auto& doc_id : inverted_lists_[c]) {
            if (visited.count(doc_id)) continue;
            visited.insert(doc_id);

            if (!candidates.count(doc_id)) continue;

            auto it = nodes_.find(doc_id);
            if (it == nodes_.end()) continue;

            double score = compute_score(metric_, query, it->second.vector);
            filtered.push_back({doc_id, -score});
        }
    }

    std::sort(filtered.begin(), filtered.end(),
              [](const DistCandidate& a, const DistCandidate& b) { return a.dist < b.dist; });

    std::vector<SearchResult> results;
    results.reserve(std::min(k, static_cast<int>(filtered.size())));
    for (int i = 0; i < static_cast<int>(filtered.size()) &&
         static_cast<int>(results.size()) < k; i++) {
        results.push_back({filtered[i].doc_id, -filtered[i].dist});
    }

    return results;
}

// ============================================================
// Size
// ============================================================

size_t IVFIndex::size() const {
    std::shared_lock lock(mu_);
    return nodes_.size();
}

// ============================================================
// Save / Load
// ============================================================

void IVFIndex::save(std::ostream& os) const {
    std::shared_lock lock(mu_);

    nlohmann::json j;
    j["metric"] = metric_to_string(metric_);
    j["dimension"] = dimension_;
    j["centroids"] = centroids_;
    j["num_centroids"] = num_centroids_;
    j["n_probe"] = n_probe_;

    auto& jnodes = j["nodes"];
    jnodes = nlohmann::json::object();
    for (const auto& [id, node] : nodes_) {
        nlohmann::json jn;
        jn["id"] = node.id;
        jn["vector"] = node.vector;
        jnodes[id] = jn;
    }

    os << j.dump(2);
}

void IVFIndex::load(std::istream& is) {
    nlohmann::json j;
    is >> j;

    std::unique_lock lock(mu_);

    metric_ = metric_from_string(j["metric"].get<std::string>());
    dimension_ = j["dimension"];
    num_centroids_ = j["num_centroids"];
    n_probe_ = j["n_probe"];
    centroids_ = j["centroids"].get<std::vector<std::vector<double>>>();

    nodes_.clear();
    for (auto it = j["nodes"].begin(); it != j["nodes"].end(); ++it) {
        Node node;
        node.id = it.value()["id"];
        node.vector = it.value()["vector"].get<std::vector<double>>();
        nodes_[node.id] = std::move(node);
    }

    // Rebuild inverted lists
    if (!centroids_.empty()) {
        int k_c = static_cast<int>(centroids_.size());
        inverted_lists_.resize(k_c);
        for (int i = 0; i < k_c; i++) inverted_lists_[i].clear();

        for (const auto& [doc_id, node] : nodes_) {
            int c = find_nearest_centroid(node.vector);
            if (c >= 0 && c < k_c) {
                inverted_lists_[c].push_back(doc_id);
            }
        }
    }
}

} // namespace flux
