#pragma once

#include "database.h"
#include "distance.h"

#include <memory>
#include <random>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace flux {

// ============================================================
// HNSW Index — Hierarchical Navigable Small World
// (mirrors Go hnsw.go)
// ============================================================

class HNSWIndex : public Index {
public:
    // ---- Parameters ----
    static constexpr int kDefaultM = 16;
    static constexpr int kDefaultMmax0 = 32;    // 2 * M
    static constexpr int kDefaultefConstruction = 200;
    static constexpr int kDefaultefSearch = 100;

    struct Options {
        int m = kDefaultM;
        int mmax0 = kDefaultMmax0;
        int ef_construction = kDefaultefConstruction;
        int ef_search = kDefaultefSearch;
    };

    HNSWIndex(DistanceMetric metric, int dimension, Options opts);
    ~HNSWIndex() override = default;

    // ---- Index interface ----
    void insert(const std::string& doc_id, const std::vector<double>& vector) override;
    void remove(const std::string& doc_id) override;

    std::vector<SearchResult> search_internal(
        const std::vector<double>& query, int k) override;

    std::vector<SearchResult> search_internal_with_filter(
        const std::vector<double>& query, int k,
        const std::unordered_set<std::string>& candidates) override;

    size_t size() const override;

    // ---- Serialization ----
    void save(std::ostream& os) const;
    void load(std::istream& is);

private:
    // ---- Distance ----
    double dist(const double* a, const double* b) const {
        return -compute_score(metric_, a, b, dimension_);
    }

    // ---- Level distribution ----
    int generate_level();

    // ---- Search layer (Algorithm 2 from HNSW paper) ----
    std::unordered_map<std::string, double> search_layer(
        const double* query, const std::vector<std::string>& entry_points,
        int ef, int layer) const;

    // ---- Greedy search (for top-layer traversal) ----
    std::string greedy_search_layer(const double* query,
                                     const std::string& entry_point,
                                     int layer) const;

    // ---- Neighbor selection ----
    std::vector<std::string> select_neighbors_simple(
        const std::unordered_map<std::string, double>& candidates, int k) const;

    // ---- Connection management ----
    void add_connections(const std::string& node_id,
                         const std::vector<std::string>& neighbors, int layer);
    void shrink_connections(const std::string& node_id, int layer, int max_conn);

    // ---- Internal ops ----
    void internal_delete(const std::string& doc_id);

    // ---- Node structure ----
    struct Node {
        std::string id;
        std::vector<double> vector;
        int level = 0;
        // connections[layer] = vector of neighbor IDs
        std::vector<std::vector<std::string>> connections;
    };

    mutable std::shared_mutex mu_;
    DistanceMetric metric_;
    int dimension_;

    std::unordered_map<std::string, Node> nodes_;
    std::unordered_set<std::string> deleted_;

    std::string entry_point_;
    int max_level_ = -1;

    // Parameters
    int m_;
    int mmax0_;
    int ef_construction_;
    int ef_search_;
    double ml_; // 1/ln(M)

    // RNG
    mutable std::mt19937 rng_;
};

} // namespace flux
