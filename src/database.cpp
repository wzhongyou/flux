#include "database.h"
#include "distance.h"
#include "wal.h"
#include "hnsw.h"
#include "ivf.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace flux {

// ============================================================
// Helper: resolve filter with meta index
// ============================================================

static void resolve_docs(std::vector<SearchResult>& results,
                         const std::unordered_map<std::string, Document>& docs) {
    for (auto& r : results) {
        auto it = docs.find(r.id);
        if (it != docs.end()) {
            r.doc_ptr = &it->second;
        }
    }
}

// ============================================================
// Collection::Search
// ============================================================

std::vector<SearchResult> Collection::search(
    const std::vector<double>& query, int k, FilterFunc filter) const {

    if (query.empty() || static_cast<int>(query.size()) != dimension) {
        return {};
    }

    if (index) {
        return index_search(query, k, std::move(filter));
    }
    return brute_force_search(query, k, std::move(filter));
}

// ============================================================
// Collection::brute_force_search
// ============================================================

std::vector<SearchResult> Collection::brute_force_search(
    const std::vector<double>& query, int k, FilterFunc filter) const {

    std::vector<SearchResult> results;
    results.reserve(docs.size());

    for (const auto& [doc_id, doc] : docs) {
        if (filter && !filter(doc.metadata)) continue;
        double score = compute_score(metric, query, doc.vector);
        results.push_back({doc_id, score});
        results.back().doc_ptr = &doc;
    }

    // Sort by score descending
    std::sort(results.begin(), results.end(),
              [](const SearchResult& a, const SearchResult& b) { return a.score > b.score; });

    if (static_cast<int>(results.size()) > k) {
        results.resize(k);
    }
    return results;
}

// ============================================================
// Collection::index_search
// ============================================================

std::vector<SearchResult> Collection::index_search(
    const std::vector<double>& query, int k, FilterFunc filter) const {

    if (!filter) {
        // Pure index search — no filter
        auto results = index->search_internal(query, k);
        resolve_docs(results, docs);
        return results;
    }

    // Pre-filtering path
    if (meta_index) {
        auto candidates = meta_index->all();
        if (candidates.empty()) return {};

        auto results = index->search_internal_with_filter(query, k, candidates);
        resolve_docs(results, docs);

        // Apply filter to results
        std::vector<SearchResult> filtered;
        for (auto& r : results) {
            if (r.doc_ptr && filter(r.doc_ptr->metadata)) {
                filtered.push_back(std::move(r));
            }
        }
        return filtered;
    }

    // Post-filter approach with oversampling
    constexpr int oversample = 3;
    int search_k = k * oversample;
    if (search_k > static_cast<int>(docs.size())) {
        search_k = static_cast<int>(docs.size());
    }

    auto results = index->search_internal(query, search_k);
    resolve_docs(results, docs);

    std::vector<SearchResult> filtered;
    for (auto& r : results) {
        if (r.doc_ptr && filter(r.doc_ptr->metadata)) {
            filtered.push_back(std::move(r));
        }
    }

    std::sort(filtered.begin(), filtered.end(),
              [](const SearchResult& a, const SearchResult& b) { return a.score > b.score; });

    // Fall back to brute force if post-filter didn't yield enough
    if (static_cast<int>(filtered.size()) < k) {
        auto bf = brute_force_search(query, k, std::move(filter));
        if (bf.size() > filtered.size()) return bf;
    }

    if (static_cast<int>(filtered.size()) > k) {
        filtered.resize(k);
    }
    return filtered;
}

// ============================================================
// Collection::hybrid_search
// ============================================================

std::vector<HybridSearchResult> Collection::hybrid_search(
    const std::vector<double>& query,
    const std::string& text_query,
    int k, double alpha,
    FilterFunc filter) const {

    if (!bm25 || text_query.empty()) {
        // Fall back to vector-only
        auto vec_results = search(query, k, std::move(filter));
        std::vector<HybridSearchResult> results;
        results.reserve(vec_results.size());
        for (auto& r : vec_results) {
            HybridSearchResult hr;
            hr.id = r.id;
            hr.vector_score = r.score;
            hr.text_score = 0.0;
            hr.combined_score = r.score;
            hr.doc_ptr = r.doc_ptr;
            results.push_back(std::move(hr));
        }
        return results;
    }

    auto text_scores = bm25->score(text_query);

    // Build candidate set
    std::unordered_set<std::string> candidate_set;
    for (const auto& [id, _] : text_scores) candidate_set.insert(id);

    // Vector search — all docs for hybrid ranking
    std::vector<SearchResult> vec_results;
    if (index) {
        vec_results = index->search_internal(query, static_cast<int>(docs.size()));
    } else {
        vec_results = brute_force_search(query, static_cast<int>(docs.size()), nullptr);
    }
    for (const auto& r : vec_results) {
        candidate_set.insert(r.id);
    }

    if (candidate_set.empty()) return {};

    // Normalize vector scores to [0, 1]
    double max_vec = std::numeric_limits<double>::lowest();
    double min_vec = std::numeric_limits<double>::max();
    std::unordered_map<std::string, double> vec_score_map;
    for (const auto& r : vec_results) {
        if (candidate_set.count(r.id)) {
            double s = std::max(0.0, r.score);
            vec_score_map[r.id] = s;
            if (s > max_vec) max_vec = s;
            if (s < min_vec) min_vec = s;
        }
    }
    double vec_range = max_vec - min_vec;
    if (vec_range == 0.0) vec_range = 1.0;

    // Normalize text scores to [0, 1]
    double max_text = std::numeric_limits<double>::lowest();
    double min_text = std::numeric_limits<double>::max();
    for (const auto& [id, score] : text_scores) {
        if (candidate_set.count(id)) {
            if (score > max_text) max_text = score;
            if (score < min_text) min_text = score;
        }
    }
    double text_range = max_text - min_text;
    if (text_range == 0.0) text_range = 1.0;

    // Build ranked lists for RRF
    std::vector<std::string> vec_ranked;
    for (const auto& r : vec_results) {
        if (candidate_set.count(r.id)) vec_ranked.push_back(r.id);
    }

    std::vector<std::pair<std::string, double>> text_ranked;
    for (const auto& [id, score] : text_scores) {
        if (candidate_set.count(id)) text_ranked.emplace_back(id, score);
    }
    std::sort(text_ranked.begin(), text_ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::unordered_map<std::string, int> vec_rank;
    for (size_t i = 0; i < vec_ranked.size(); i++) vec_rank[vec_ranked[i]] = static_cast<int>(i) + 1;

    std::unordered_map<std::string, int> text_rank;
    for (size_t i = 0; i < text_ranked.size(); i++) text_rank[text_ranked[i].first] = static_cast<int>(i) + 1;

    // Compute combined scores
    std::vector<HybridSearchResult> results;
    for (const auto& id : candidate_set) {
        auto doc_it = docs.find(id);
        if (doc_it == docs.end()) continue;

        if (filter && !filter(doc_it->second.metadata)) continue;

        double vec_norm = 0.0;
        if (vec_score_map.count(id)) {
            vec_norm = (vec_score_map[id] - min_vec) / vec_range;
        }
        double text_norm = 0.0;
        auto ts_it = text_scores.find(id);
        if (ts_it != text_scores.end()) {
            text_norm = (ts_it->second - min_text) / text_range;
        }

        double combined;
        if (alpha < 0.0) {
            // RRF
            constexpr double rrf_k = 60.0;
            combined = 0.0;
            if (vec_rank.count(id)) combined += 1.0 / (rrf_k + vec_rank[id]);
            if (text_rank.count(id)) combined += 1.0 / (rrf_k + text_rank[id]);
        } else {
            combined = alpha * vec_norm + (1.0 - alpha) * text_norm;
        }

        HybridSearchResult hr;
        hr.id = id;
        hr.vector_score = vec_norm;
        hr.text_score = text_norm;
        hr.combined_score = combined;
        hr.doc_ptr = &doc_it->second;
        results.push_back(std::move(hr));
    }

    std::sort(results.begin(), results.end(),
              [](const HybridSearchResult& a, const HybridSearchResult& b) {
                  return a.combined_score > b.combined_score;
              });

    if (static_cast<int>(results.size()) > k) results.resize(k);
    return results;
}

// ============================================================
// VectorDatabase
// ============================================================

VectorDatabase::VectorDatabase(const std::string& wal_path) {
    if (!wal_path.empty()) {
        wal_ = std::make_unique<WAL>(wal_path);
        replay_wal();
    }
}

VectorDatabase::~VectorDatabase() {
    if (wal_) wal_->close();
}

// ---- Helpers ----

Collection* VectorDatabase::get_collection(const std::string& name) {
    auto it = collections_.find(name);
    if (it == collections_.end()) return nullptr;
    return it->second.get();
}

// ---- Collection management ----

void VectorDatabase::create_collection(const std::string& name, DistanceMetric metric) {
    if (name.empty()) throw std::invalid_argument("collection name required");

    std::unique_lock lock(mu_);
    if (collections_.count(name)) {
        throw std::invalid_argument("collection \"" + name + "\" already exists");
    }

    auto col = std::make_unique<Collection>();
    col->name = name;
    col->metric = metric;
    collections_[name] = std::move(col);

    // WAL: create_collection
    if (wal_) {
        WALEvent ev;
        ev.action = "create_collection";
        ev.collection = name;
        ev.metric = metric_to_string(metric);
        wal_->append(std::move(ev));
    }
}

void VectorDatabase::create_collection_with_schema(
    const std::string& name, DistanceMetric metric, std::unique_ptr<Schema> schema) {

    if (name.empty()) throw std::invalid_argument("collection name required");

    std::unique_lock lock(mu_);
    if (collections_.count(name)) {
        throw std::invalid_argument("collection \"" + name + "\" already exists");
    }

    auto col = std::make_unique<Collection>();
    col->name = name;
    col->metric = metric;
    col->schema = std::move(schema);

    if (col->schema && col->schema->has_indexable_fields()) {
        col->meta_index = std::make_unique<MetadataInvertedIndex>();
    }
    if (col->schema) {
        std::string tf = col->schema->text_field();
        if (!tf.empty()) {
            col->text_field = tf;
            col->bm25 = std::make_unique<BM25Index>();
        }
    }

    std::string saved_name = name;
    std::string saved_metric = metric_to_string(metric);

    collections_[name] = std::move(col);

    // WAL
    if (wal_) {
        WALEvent ev;
        ev.action = "create_collection";
        ev.collection = saved_name;
        ev.metric = saved_metric;
        wal_->append(std::move(ev));
    }
}

void VectorDatabase::build_index(const std::string& collection_name, const std::string& index_type) {
    std::unique_lock lock(mu_);
    auto* col = get_collection(collection_name);
    if (!col) throw std::invalid_argument("collection \"" + collection_name + "\" not found");

    if (index_type == "hnsw") {
        int dim = col->dimension > 0 ? col->dimension : 128;
        auto idx = std::make_unique<HNSWIndex>(col->metric, dim, HNSWIndex::Options{});
        for (const auto& [id, doc] : col->docs) {
            idx->insert(id, doc.vector);
        }
        col->index = std::move(idx);
        col->index_type = "hnsw";
    } else if (index_type == "ivf") {
        int dim = col->dimension > 0 ? col->dimension : 128;
        auto idx = std::make_unique<IVFIndex>(col->metric, dim, IVFIndex::Options{});
        for (const auto& [id, doc] : col->docs) {
            idx->insert(id, doc.vector);
        }
        // Train the IVF index (K-means clustering)
        static_cast<IVFIndex*>(idx.get())->build();
        col->index = std::move(idx);
        col->index_type = "ivf";
    } else {
        throw std::invalid_argument("unsupported index type \"" + index_type + "\" (supported: hnsw, ivf)");
    }

    spdlog::info("build_index({}): {} index built with {} documents",
                 collection_name, index_type, col->docs.size());
}

void VectorDatabase::drop_index(const std::string& collection_name) {
    std::unique_lock lock(mu_);
    auto* col = get_collection(collection_name);
    if (!col) throw std::invalid_argument("collection \"" + collection_name + "\" not found");

    col->index.reset();
    col->index_type.clear();
}

std::string VectorDatabase::index_info(const std::string& collection_name) const {
    std::shared_lock lock(mu_);
    auto it = collections_.find(collection_name);
    if (it == collections_.end()) return "";
    return it->second->index_type;
}

void VectorDatabase::delete_collection(const std::string& name) {
    std::unique_lock lock(mu_);
    if (!collections_.count(name)) {
        throw std::invalid_argument("collection \"" + name + "\" not found");
    }
    collections_.erase(name);
}

void VectorDatabase::truncate_collection(const std::string& name) {
    std::unique_lock lock(mu_);
    auto* col = get_collection(name);
    if (!col) throw std::invalid_argument("collection \"" + name + "\" not found");

    col->docs.clear();
    if (col->meta_index) col->meta_index->clear();
    if (col->bm25) col->bm25->clear();
    col->index.reset();
    col->index_type.clear();
}

std::vector<std::string> VectorDatabase::list_collections() const {
    std::shared_lock lock(mu_);
    std::vector<std::string> names;
    names.reserve(collections_.size());
    for (const auto& [name, _] : collections_) names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
}

// ---- Document operations ----

void VectorDatabase::upsert(const std::string& collection_name, Document doc) {
    if (doc.id.empty()) throw std::invalid_argument("document id required");

    std::unique_lock lock(mu_);
    auto* col = get_collection(collection_name);
    if (!col) throw std::invalid_argument("collection \"" + collection_name + "\" not found");

    // Validate dimension
    if (col->dimension == 0) {
        col->dimension = static_cast<int>(doc.vector.size());
    } else if (static_cast<int>(doc.vector.size()) != col->dimension) {
        throw std::invalid_argument("dimension mismatch: expected " +
                                    std::to_string(col->dimension) + ", got " +
                                    std::to_string(doc.vector.size()));
    }

    // Validate schema
    if (col->schema) {
        col->schema->validate(doc.metadata);
    }

    // Store document (copy for WAL)
    auto doc_copy = doc;
    std::string id = doc.id;
    col->docs[id] = std::move(doc);

    // Update metadata index
    if (col->meta_index) {
        col->meta_index->insert(id, doc_copy.metadata);
    }

    // Update BM25
    if (col->bm25 && !col->text_field.empty()) {
        if (doc_copy.metadata.contains(col->text_field) &&
            doc_copy.metadata[col->text_field].is_string()) {
            col->bm25->index_document(id, doc_copy.metadata[col->text_field].get<std::string>());
        }
    }

    // Update ANN index
    if (col->index) {
        col->index->insert(id, doc_copy.vector);
    }

    // WAL
    if (wal_) {
        WALEvent ev;
        ev.action = "upsert";
        ev.collection = collection_name;
        ev.document = doc_copy;
        wal_->append(std::move(ev));
    }
}

int VectorDatabase::batch_upsert(const std::string& collection_name, std::vector<Document> docs) {
    if (docs.empty()) throw std::invalid_argument("at least one document required");

    std::unique_lock lock(mu_);
    auto* col = get_collection(collection_name);
    if (!col) throw std::invalid_argument("collection \"" + collection_name + "\" not found");

    // Validate all first (fail-fast)
    for (const auto& doc : docs) {
        if (doc.id.empty()) throw std::invalid_argument("document id required");
        if (col->dimension == 0) {
            col->dimension = static_cast<int>(doc.vector.size());
        } else if (static_cast<int>(doc.vector.size()) != col->dimension) {
            throw std::invalid_argument("dimension mismatch");
        }
        if (col->schema) col->schema->validate(doc.metadata);
    }

    for (const auto& doc : docs) {
        col->docs[doc.id] = doc;
        if (col->meta_index) col->meta_index->insert(doc.id, doc.metadata);
        if (col->index) col->index->insert(doc.id, doc.vector);
    }

    // WAL
    if (wal_) {
        WALEvent ev;
        ev.action = "batch_upsert";
        ev.collection = collection_name;
        ev.documents = nlohmann::json(docs);
        wal_->append(std::move(ev));
    }

    return static_cast<int>(docs.size());
}

void VectorDatabase::remove(const std::string& collection_name, const std::string& doc_id) {
    if (doc_id.empty()) throw std::invalid_argument("document id required");

    std::unique_lock lock(mu_);
    auto* col = get_collection(collection_name);
    if (!col) throw std::invalid_argument("collection \"" + collection_name + "\" not found");

    col->docs.erase(doc_id);
    if (col->meta_index) col->meta_index->remove(doc_id);
    if (col->bm25) col->bm25->remove_document(doc_id);
    if (col->index) col->index->remove(doc_id);

    if (wal_) {
        WALEvent ev;
        ev.action = "delete";
        ev.collection = collection_name;
        ev.id = doc_id;
        wal_->append(std::move(ev));
    }
}

int VectorDatabase::batch_remove(const std::string& collection_name,
                                  const std::vector<std::string>& ids) {
    if (ids.empty()) throw std::invalid_argument("at least one id required");

    std::unique_lock lock(mu_);
    auto* col = get_collection(collection_name);
    if (!col) throw std::invalid_argument("collection \"" + collection_name + "\" not found");

    for (const auto& id : ids) {
        col->docs.erase(id);
        if (col->meta_index) col->meta_index->remove(id);
        if (col->bm25) col->bm25->remove_document(id);
        if (col->index) col->index->remove(id);
    }

    if (wal_) {
        WALEvent ev;
        ev.action = "batch_delete";
        ev.collection = collection_name;
        ev.ids = nlohmann::json(ids);
        wal_->append(std::move(ev));
    }

    return static_cast<int>(ids.size());
}

// ---- Search ----

std::vector<SearchResult> VectorDatabase::search(
    const std::string& collection_name,
    const std::vector<double>& query, int k,
    FilterFunc filter) const {

    if (k <= 0) throw std::invalid_argument("k must be positive");

    std::shared_lock lock(mu_);
    auto it = collections_.find(collection_name);
    if (it == collections_.end()) {
        throw std::invalid_argument("collection \"" + collection_name + "\" not found");
    }

    return it->second->search(query, k, std::move(filter));
}

std::vector<HybridSearchResult> VectorDatabase::hybrid_search(
    const std::string& collection_name,
    const std::vector<double>& query,
    const std::string& text_query,
    int k, double alpha,
    FilterFunc filter) const {

    if (k <= 0) throw std::invalid_argument("k must be positive");

    std::shared_lock lock(mu_);
    auto it = collections_.find(collection_name);
    if (it == collections_.end()) {
        return {};
    }

    return it->second->hybrid_search(query, text_query, k, alpha, std::move(filter));
}

// ---- Stats ----

VectorDatabase::CollectionStats VectorDatabase::collection_stats(const std::string& name) const {
    std::shared_lock lock(mu_);
    auto it = collections_.find(name);
    if (it == collections_.end()) {
        throw std::invalid_argument("collection \"" + name + "\" not found");
    }
    const auto& col = it->second;
    return {
        static_cast<int>(col->docs.size()),
        col->dimension,
        col->metric,
        col->index_type,
    };
}

// ---- WAL Replay ----

void VectorDatabase::replay_wal() {
    if (!wal_) return;

    wal_->replay([this](const WALEvent& event) -> bool {
        const auto& action = event.action;

        if (action == "create_collection") {
            if (!collections_.count(event.collection)) {
                auto col = std::make_unique<Collection>();
                col->name = event.collection;
                col->metric = metric_from_string(
                    event.metric.empty() ? "cosine" : event.metric);
                collections_[event.collection] = std::move(col);
            }
        }
        else if (action == "upsert") {
            auto* col = get_collection(event.collection);
            if (!col) {
                spdlog::error("wal replay: collection {} not found", event.collection);
                return true; // skip
            }
            Document doc = event.document;
            if (col->dimension == 0) {
                col->dimension = static_cast<int>(doc.vector.size());
            }
            col->docs[doc.id] = std::move(doc);
        }
        else if (action == "delete") {
            auto* col = get_collection(event.collection);
            if (col) {
                col->docs.erase(event.id);
                if (col->meta_index) col->meta_index->remove(event.id);
                if (col->index) col->index->remove(event.id);
            }
        }
        else if (action == "batch_upsert") {
            auto* col = get_collection(event.collection);
            if (!col) return true;
            if (event.documents.is_array()) {
                for (const auto& jdoc : event.documents) {
                    Document doc = jdoc;
                    if (col->dimension == 0) col->dimension = static_cast<int>(doc.vector.size());
                    col->docs[doc.id] = doc;
                }
            }
        }
        else if (action == "batch_delete") {
            auto* col = get_collection(event.collection);
            if (col && event.ids.is_array()) {
                for (const auto& jid : event.ids) {
                    std::string id = jid.get<std::string>();
                    col->docs.erase(id);
                    if (col->meta_index) col->meta_index->remove(id);
                    if (col->index) col->index->remove(id);
                }
            }
        }
        else if (action == "delete_collection") {
            collections_.erase(event.collection);
        }
        else if (action == "truncate_collection") {
            auto* col = get_collection(event.collection);
            if (col) {
                col->docs.clear();
                if (col->meta_index) col->meta_index->clear();
                col->index.reset();
                col->index_type.clear();
            }
        }
        else {
            spdlog::error("wal replay: unknown action {}", action);
        }

        return true; // continue replay
    });
}

} // namespace flux
