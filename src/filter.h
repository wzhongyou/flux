#pragma once

#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>

namespace flux {

// ============================================================
// Filter engine — filter functions + metadata inverted index
// (mirrors Go filter.go + vector.go FilterFunc)
// ============================================================

using FilterFunc = std::function<bool(const nlohmann::json& metadata)>;

// ---- Filter combinators ----

FilterFunc FieldEqual(const std::string& field, const nlohmann::json& value);
FilterFunc FieldNotEqual(const std::string& field, const nlohmann::json& value);
FilterFunc FieldRange(const std::string& field, double min, double max);
FilterFunc FieldIn(const std::string& field, std::vector<nlohmann::json> values);
FilterFunc FieldExists(const std::string& field);
FilterFunc TextMatch(const std::string& field, const std::string& pattern);
FilterFunc GeoRadius(const std::string& field, double center_lat, double center_lng, double radius_m);

FilterFunc And(std::vector<FilterFunc> filters);
FilterFunc Or(std::vector<FilterFunc> filters);
FilterFunc Not(FilterFunc filter);

// ---- Utility ----

// Convert a JSON value to a comparable key string for the inverted index.
std::string value_to_key(const nlohmann::json& value);

// Check equality between two JSON values (type-aware).
bool json_values_equal(const nlohmann::json& a, const nlohmann::json& b);

// Extract double from JSON value (throws if not numeric).
double json_to_double(const nlohmann::json& v);

// Haversine distance in kilometers.
double haversine_km(double lat1, double lng1, double lat2, double lng2);

// ============================================================
// MetadataInvertedIndex — fast pre-filtering for schema fields
// ============================================================
class MetadataInvertedIndex {
public:
    MetadataInvertedIndex() = default;

    // Insert a document's metadata into the index.
    void insert(const std::string& doc_id, const nlohmann::json& metadata);

    // Remove a document from the index.
    void remove(const std::string& doc_id);

    // Equality match: field == value.
    std::unordered_set<std::string> match(const std::string& field, const nlohmann::json& value) const;

    // Set match: field in {values}.
    std::unordered_set<std::string> match_in(const std::string& field,
                                              const std::vector<nlohmann::json>& values) const;

    // Range match: field value in [min, max].
    std::unordered_set<std::string> match_range(const std::string& field,
                                                  double min, double max) const;

    // All indexed document IDs.
    std::unordered_set<std::string> all() const;

    // Clear all entries.
    void clear();

    // Size of the index (total entries).
    size_t size() const;

private:
    mutable std::shared_mutex mu_;

    // field → value_key → set of doc_ids
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::unordered_set<std::string>>> fields_;

    // field → doc_id → numeric value (for range queries)
    std::unordered_map<std::string,
        std::unordered_map<std::string, double>> ranges_;
};

} // namespace flux
