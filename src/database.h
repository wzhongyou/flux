#pragma once

#include <nlohmann/json.hpp>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "types.h"
#include "schema.h"
#include "filter.h"
#include "bm25.h"

namespace flux {

// ============================================================
// Index interface (mirrors Go index.go)
// ============================================================

class Index {
public:
    virtual ~Index() = default;

    virtual void insert(const std::string& doc_id, const std::vector<double>& vector) = 0;
    virtual void remove(const std::string& doc_id) = 0;

    // Returns top-k results with ID and Score populated (NOT Document).
    virtual std::vector<SearchResult> search_internal(
        const std::vector<double>& query, int k) = 0;

    // Pre-filtered search (optional interface — see FilteredIndex).
    virtual std::vector<SearchResult> search_internal_with_filter(
        const std::vector<double>& query, int k,
        const std::unordered_set<std::string>& candidates) = 0;

    virtual size_t size() const = 0;
};

// ============================================================
// Collection (mirrors Go Collection struct)
// ============================================================

struct Collection {
    std::string name;
    DistanceMetric metric = DistanceMetric::Cosine;
    int dimension = 0;

    // Document storage: doc_id → Document
    std::unordered_map<std::string, Document> docs;

    // Optional ANN index
    std::unique_ptr<Index> index;
    std::string index_type; // "hnsw", "ivf", or ""

    // Schema
    std::unique_ptr<Schema> schema;
    std::unique_ptr<MetadataInvertedIndex> meta_index;

    // BM25 full-text
    std::string text_field;
    std::unique_ptr<BM25Index> bm25;

    // ---- Search ----

    std::vector<SearchResult> search(
        const std::vector<double>& query, int k, FilterFunc filter) const;

    std::vector<HybridSearchResult> hybrid_search(
        const std::vector<double>& query,
        const std::string& text_query,
        int k, double alpha,
        FilterFunc filter) const;

private:
    std::vector<SearchResult> brute_force_search(
        const std::vector<double>& query, int k, FilterFunc filter) const;

    std::vector<SearchResult> index_search(
        const std::vector<double>& query, int k, FilterFunc filter) const;
};

// ============================================================
// VectorDatabase (mirrors Go VectorDatabase)
// ============================================================

class WAL; // forward declaration

class VectorDatabase {
public:
    explicit VectorDatabase(const std::string& wal_path = "");
    ~VectorDatabase();

    VectorDatabase(const VectorDatabase&) = delete;
    VectorDatabase& operator=(const VectorDatabase&) = delete;

    // ---- Collection management ----

    void create_collection(const std::string& name, DistanceMetric metric = DistanceMetric::Cosine);
    void create_collection_with_schema(const std::string& name, DistanceMetric metric,
                                       std::unique_ptr<Schema> schema);

    // Build an ANN index for a collection ("hnsw" or "ivf").
    void build_index(const std::string& collection_name, const std::string& index_type);

    // Drop the ANN index (fall back to brute force).
    void drop_index(const std::string& collection_name);

    // Get index type for a collection ("" if none).
    std::string index_info(const std::string& collection_name) const;

    // Delete an entire collection.
    void delete_collection(const std::string& name);

    // Truncate all documents (keep collection).
    void truncate_collection(const std::string& name);

    [[nodiscard]] std::vector<std::string> list_collections() const;

    // ---- Document operations ----

    void upsert(const std::string& collection_name, Document doc);
    int  batch_upsert(const std::string& collection_name, std::vector<Document> docs);
    void remove(const std::string& collection_name, const std::string& doc_id);
    int  batch_remove(const std::string& collection_name, const std::vector<std::string>& ids);

    // ---- Search ----

    std::vector<SearchResult> search(
        const std::string& collection_name,
        const std::vector<double>& query, int k,
        FilterFunc filter = nullptr) const;

    std::vector<HybridSearchResult> hybrid_search(
        const std::string& collection_name,
        const std::vector<double>& query,
        const std::string& text_query,
        int k, double alpha,
        FilterFunc filter = nullptr) const;

    // ---- Stats ----

    struct CollectionStats {
        int doc_count = 0;
        int dimension = 0;
        DistanceMetric metric = DistanceMetric::Cosine;
        std::string index_type;
    };

    CollectionStats collection_stats(const std::string& name) const;

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<Collection>> collections_;
    std::unique_ptr<WAL> wal_;

    void replay_wal();
    Collection* get_collection(const std::string& name);

    // WAL event helpers
    void wal_create_collection(const std::string& name, DistanceMetric metric);
    void wal_upsert(const std::string& collection_name, const Document& doc);
    void wal_delete(const std::string& collection_name, const std::string& doc_id);
};

} // namespace flux
