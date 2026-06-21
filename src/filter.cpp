#include "filter.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace flux {

// ============================================================
// Utility functions
// ============================================================

std::string value_to_key(const nlohmann::json& value) {
    if (value.is_string())   return "s:" + value.get<std::string>();
    if (value.is_number_float()) return "f:" + std::to_string(value.get<double>());
    if (value.is_number_integer()) return "i:" + std::to_string(value.get<int64_t>());
    if (value.is_boolean())  return value.get<bool>() ? "b:true" : "b:false";
    return "?:" + value.dump();
}

bool json_values_equal(const nlohmann::json& a, const nlohmann::json& b) {
    if (a.type() != b.type()) {
        // Special case: int vs float
        if (a.is_number() && b.is_number()) {
            return json_to_double(a) == json_to_double(b);
        }
        return false;
    }
    return a == b;
}

double json_to_double(const nlohmann::json& v) {
    if (v.is_number_float())  return v.get<double>();
    if (v.is_number_integer()) return static_cast<double>(v.get<int64_t>());
    throw std::invalid_argument("value is not numeric");
}

double haversine_km(double lat1, double lng1, double lat2, double lng2) {
    constexpr double R = 6371.0;
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlng = (lng2 - lng1) * M_PI / 180.0;
    double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
               std::cos(lat1 * M_PI / 180.0) * std::cos(lat2 * M_PI / 180.0) *
                   std::sin(dlng / 2) * std::sin(dlng / 2);
    double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
    return R * c;
}

// ============================================================
// Filter combinators
// ============================================================

FilterFunc FieldEqual(const std::string& field, const nlohmann::json& value) {
    return [field, value](const nlohmann::json& metadata) -> bool {
        if (metadata.is_null() || !metadata.is_object()) return false;
        if (!metadata.contains(field)) return false;
        return json_values_equal(metadata[field], value);
    };
}

FilterFunc FieldNotEqual(const std::string& field, const nlohmann::json& value) {
    return [field, value](const nlohmann::json& metadata) -> bool {
        if (metadata.is_null()) return true;
        if (!metadata.contains(field)) return true;
        return !json_values_equal(metadata[field], value);
    };
}

FilterFunc FieldRange(const std::string& field, double min, double max) {
    return [field, min, max](const nlohmann::json& metadata) -> bool {
        if (metadata.is_null() || !metadata.is_object()) return false;
        if (!metadata.contains(field)) return false;
        try {
            double v = json_to_double(metadata[field]);
            return v >= min && v <= max;
        } catch (...) {
            return false;
        }
    };
}

FilterFunc FieldIn(const std::string& field, std::vector<nlohmann::json> values) {
    return [field, values = std::move(values)](const nlohmann::json& metadata) -> bool {
        if (metadata.is_null() || !metadata.is_object()) return false;
        if (!metadata.contains(field)) return false;
        for (const auto& v : values) {
            if (json_values_equal(metadata[field], v)) return true;
        }
        return false;
    };
}

FilterFunc FieldExists(const std::string& field) {
    return [field](const nlohmann::json& metadata) -> bool {
        if (metadata.is_null() || !metadata.is_object()) return false;
        return metadata.contains(field);
    };
}

FilterFunc TextMatch(const std::string& field, const std::string& pattern) {
    return [field, pattern](const nlohmann::json& metadata) -> bool {
        if (metadata.is_null() || !metadata.is_object()) return false;
        if (!metadata.contains(field) || !metadata[field].is_string()) return false;
        std::string s = metadata[field].get<std::string>();
        // Case-insensitive contains
        auto it = std::search(
            s.begin(), s.end(),
            pattern.begin(), pattern.end(),
            [](char c1, char c2) { return std::tolower(c1) == std::tolower(c2); });
        return it != s.end();
    };
}

FilterFunc GeoRadius(const std::string& field, double center_lat, double center_lng, double radius_m) {
    return [field, center_lat, center_lng, radius_m](const nlohmann::json& metadata) -> bool {
        if (metadata.is_null() || !metadata.is_object()) return false;
        if (!metadata.contains(field) || !metadata[field].is_object()) return false;
        const auto& geo = metadata[field];
        if (!geo.contains("lat") || !geo.contains("lng")) return false;
        try {
            double lat = json_to_double(geo["lat"]);
            double lng = json_to_double(geo["lng"]);
            return haversine_km(center_lat, center_lng, lat, lng) <= (radius_m / 1000.0);
        } catch (...) {
            return false;
        }
    };
}

FilterFunc And(std::vector<FilterFunc> filters) {
    return [filters = std::move(filters)](const nlohmann::json& metadata) -> bool {
        for (const auto& f : filters) {
            if (!f(metadata)) return false;
        }
        return true;
    };
}

FilterFunc Or(std::vector<FilterFunc> filters) {
    return [filters = std::move(filters)](const nlohmann::json& metadata) -> bool {
        for (const auto& f : filters) {
            if (f(metadata)) return true;
        }
        return false;
    };
}

FilterFunc Not(FilterFunc filter) {
    return [filter = std::move(filter)](const nlohmann::json& metadata) -> bool {
        return !filter(metadata);
    };
}

// ============================================================
// MetadataInvertedIndex
// ============================================================

void MetadataInvertedIndex::insert(const std::string& doc_id, const nlohmann::json& metadata) {
    if (!metadata.is_object()) return;

    std::unique_lock lock(mu_);

    for (auto it = metadata.begin(); it != metadata.end(); ++it) {
        const std::string& field = it.key();
        const auto& value = it.value();

        std::string key = value_to_key(value);

        // String/int/bool index
        fields_[field][key].insert(doc_id);

        // Numeric index for range queries
        if (value.is_number()) {
            ranges_[field][doc_id] = json_to_double(value);
        }
    }
}

void MetadataInvertedIndex::remove(const std::string& doc_id) {
    std::unique_lock lock(mu_);

    for (auto& [field, values] : fields_) {
        for (auto& [key, ids] : values) {
            ids.erase(doc_id);
        }
        // Clean empty keys
        auto it = values.begin();
        while (it != values.end()) {
            if (it->second.empty()) {
                it = values.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& [field, doc_values] : ranges_) {
        doc_values.erase(doc_id);
    }
}

std::unordered_set<std::string> MetadataInvertedIndex::match(
    const std::string& field, const nlohmann::json& value) const {

    std::shared_lock lock(mu_);
    auto fit = fields_.find(field);
    if (fit == fields_.end()) return {};

    std::string key = value_to_key(value);
    auto kit = fit->second.find(key);
    if (kit == fit->second.end()) return {};

    return kit->second; // copy
}

std::unordered_set<std::string> MetadataInvertedIndex::match_in(
    const std::string& field, const std::vector<nlohmann::json>& values) const {

    std::shared_lock lock(mu_);
    auto fit = fields_.find(field);
    if (fit == fields_.end()) return {};

    std::unordered_set<std::string> result;
    for (const auto& v : values) {
        std::string key = value_to_key(v);
        auto kit = fit->second.find(key);
        if (kit != fit->second.end()) {
            result.insert(kit->second.begin(), kit->second.end());
        }
    }
    return result;
}

std::unordered_set<std::string> MetadataInvertedIndex::match_range(
    const std::string& field, double min, double max) const {

    std::shared_lock lock(mu_);
    auto rit = ranges_.find(field);
    if (rit == ranges_.end() || rit->second.empty()) return {};

    std::unordered_set<std::string> result;
    for (const auto& [doc_id, val] : rit->second) {
        if (val >= min && val <= max) {
            result.insert(doc_id);
        }
    }
    return result;
}

std::unordered_set<std::string> MetadataInvertedIndex::all() const {
    std::shared_lock lock(mu_);
    std::unordered_set<std::string> result;
    for (const auto& [field, values] : fields_) {
        for (const auto& [key, ids] : values) {
            result.insert(ids.begin(), ids.end());
        }
    }
    return result;
}

void MetadataInvertedIndex::clear() {
    std::unique_lock lock(mu_);
    fields_.clear();
    ranges_.clear();
}

size_t MetadataInvertedIndex::size() const {
    std::shared_lock lock(mu_);
    size_t count = 0;
    for (const auto& [field, values] : fields_) {
        for (const auto& [key, ids] : values) {
            count += ids.size();
        }
    }
    return count;
}

} // namespace flux
