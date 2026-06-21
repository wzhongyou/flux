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
// IVF Index — Inverted File Index with K-means clustering
// (mirrors Go ivf.go)
// ============================================================

class IVFIndex : public Index {
public:
    static constexpr int kDefaultNumCentroids = 100;
    static constexpr int kDefaultNumProbe = 5;
    static constexpr int kDefaultMaxIter = 25;

    struct Options {
        int num_centroids = kDefaultNumCentroids;
        int n_probe = kDefaultNumProbe;
        int max_iter = kDefaultMaxIter;
    };

    IVFIndex(DistanceMetric metric, int dimension, Options opts);
    ~IVFIndex() override = default;

    // ---- Index interface ----
    void insert(const std::string& doc_id, const std::vector<double>& vector) override;
    void remove(const std::string& doc_id) override;

    std::vector<SearchResult> search_internal(
        const std::vector<double>& query, int k) override;

    std::vector<SearchResult> search_internal_with_filter(
        const std::vector<double>& query, int k,
        const std::unordered_set<std::string>& candidates) override;

    size_t size() const override;

    // ---- Training ----
    // Must be called after inserting a sufficient number of vectors.
    void build();

    // ---- Persistence ----
    void save(std::ostream& os) const;
    void load(std::istream& is);

private:
    // Euclidean distance between two vectors (used for K-means).
    double vector_dist(const std::vector<double>& a, const std::vector<double>& b) const;

    // Find the index of the centroid nearest to the given vector.
    int find_nearest_centroid(const std::vector<double>& vector) const;

    // K-means++ initialization + iteration.
    void build_clusters();

    // ---- Node ----
    struct Node {
        std::string id;
        std::vector<double> vector;
    };

    mutable std::shared_mutex mu_;
    DistanceMetric metric_;
    int dimension_;

    // Centroids (K x dimension)
    std::vector<std::vector<double>> centroids_;

    // Inverted lists: centroid_idx -> list of doc_ids
    std::vector<std::vector<std::string>> inverted_lists_;

    // All indexed vectors
    std::unordered_map<std::string, Node> nodes_;

    // Parameters
    int num_centroids_;
    int n_probe_;
    int max_iter_;

    mutable std::mt19937 rng_;
};

} // namespace flux
