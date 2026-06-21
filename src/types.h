#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace flux {

// ============================================================
// Distance metrics
// ============================================================
enum class DistanceMetric {
    Cosine,
    Euclidean,
    InnerProduct,
};

// Parse from string (for config/API).
DistanceMetric metric_from_string(const std::string& s);
const char* metric_to_string(DistanceMetric m);

// ============================================================
// Document — a single vector + metadata entry
// ============================================================
struct Document {
    std::string id;
    std::vector<double> vector;
    nlohmann::json metadata;

    Document() = default;
    Document(std::string id_, std::vector<double> vec_, nlohmann::json meta_ = nlohmann::json::object())
        : id(std::move(id_)), vector(std::move(vec_)), metadata(std::move(meta_)) {}

    // Deep copy
    Document clone() const;
};

// ============================================================
// SearchResult — one result from a similarity search
// ============================================================
struct SearchResult {
    std::string id;
    double score = 0.0;
    const Document* doc_ptr = nullptr; // optional document reference

    SearchResult() = default;
    SearchResult(std::string id_, double s) : id(std::move(id_)), score(s) {}
};

// ============================================================
// Hybrid search result
// ============================================================
struct HybridSearchResult {
    std::string id;
    double vector_score = 0.0;
    double text_score = 0.0;
    double combined_score = 0.0;
    const Document* doc_ptr = nullptr;
};

// ============================================================
// Distance candidate (used in ANN index search internals)
// ============================================================
struct DistCandidate {
    std::string doc_id;
    double dist = 0.0;
};

// ============================================================
// NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE helpers are in the .cpp files
// that need them. Declare the to_json/from_json here for clarity:
// ============================================================

void to_json(nlohmann::json& j, const Document& d);
void from_json(const nlohmann::json& j, Document& d);

void to_json(nlohmann::json& j, const SearchResult& r);
void to_json(nlohmann::json& j, const HybridSearchResult& r);

} // namespace flux
