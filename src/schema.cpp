#include "schema.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace flux {

// ============================================================
// FieldType conversions
// ============================================================

FieldType field_type_from_string(const std::string& s) {
    if (s == "string")  return FieldType::String;
    if (s == "float")   return FieldType::Float;
    if (s == "int")     return FieldType::Int;
    if (s == "bool")    return FieldType::Bool;
    if (s == "text")    return FieldType::Text;
    if (s == "geo")     return FieldType::Geo;
    throw std::invalid_argument("unknown field type: " + s);
}

const char* field_type_to_string(FieldType ft) {
    switch (ft) {
    case FieldType::String: return "string";
    case FieldType::Float:  return "float";
    case FieldType::Int:    return "int";
    case FieldType::Bool:   return "bool";
    case FieldType::Text:   return "text";
    case FieldType::Geo:    return "geo";
    }
    return "string";
}

// ============================================================
// Validation
// ============================================================

static double to_double(const nlohmann::json& val) {
    if (val.is_number_float())  return val.get<double>();
    if (val.is_number_integer()) return static_cast<double>(val.get<int64_t>());
    throw std::invalid_argument("not a number");
}

void validate_field_type(const std::string& field_name, FieldType ft, const nlohmann::json& val) {
    switch (ft) {
    case FieldType::String:
        if (!val.is_string()) throw std::invalid_argument(
            "field \"" + field_name + "\": expected string, got " + std::string(val.type_name()));
        break;

    case FieldType::Float:
        if (!val.is_number()) throw std::invalid_argument(
            "field \"" + field_name + "\": expected number, got " + std::string(val.type_name()));
        break;

    case FieldType::Int:
        if (val.is_number_float()) {
            double f = val.get<double>();
            if (f != std::floor(f)) throw std::invalid_argument(
                "field \"" + field_name + "\": expected integer, got float " + std::to_string(f));
        } else if (!val.is_number_integer()) {
            throw std::invalid_argument(
                "field \"" + field_name + "\": expected integer, got " + std::string(val.type_name()));
        }
        break;

    case FieldType::Bool:
        if (!val.is_boolean()) throw std::invalid_argument(
            "field \"" + field_name + "\": expected bool, got " + std::string(val.type_name()));
        break;

    case FieldType::Text:
        if (!val.is_string()) throw std::invalid_argument(
            "field \"" + field_name + "\": expected text (string), got " + std::string(val.type_name()));
        break;

    case FieldType::Geo:
        if (!val.is_object() || !val.contains("lat") || !val.contains("lng")) {
            throw std::invalid_argument(
                "field \"" + field_name + "\": geo must be {lat, lng} object");
        }
        break;
    }
}

void Schema::validate(const nlohmann::json& metadata) const {
    if (fields.empty()) return;

    for (const auto& field : fields) {
        if (!metadata.contains(field.name)) continue; // optional field is OK

        validate_field_type(field.name, field.type, metadata[field.name]);
    }
}

// ============================================================
// Field accessors
// ============================================================

std::vector<std::string> Schema::field_names() const {
    std::vector<std::string> names;
    names.reserve(fields.size());
    for (const auto& f : fields) names.push_back(f.name);
    return names;
}

std::vector<SchemaField> Schema::indexable_fields() const {
    std::vector<SchemaField> result;
    for (const auto& f : fields) {
        if (f.indexable) result.push_back(f);
    }
    return result;
}

bool Schema::has_indexable_fields() const {
    return std::any_of(fields.begin(), fields.end(),
                       [](const SchemaField& f) { return f.indexable; });
}

std::string Schema::text_field() const {
    for (const auto& f : fields) {
        if (f.type == FieldType::Text) return f.name;
    }
    return "";
}

const SchemaField* Schema::find_field(const std::string& name) const {
    for (const auto& f : fields) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

// ============================================================
// Parse from JSON
// ============================================================

Schema schema_from_json(const nlohmann::json& raw) {
    if (!raw.contains("fields") || !raw["fields"].is_array()) {
        throw std::invalid_argument("schema requires 'fields' array");
    }

    Schema s;
    std::set<std::string> seen;

    for (const auto& item : raw["fields"]) {
        if (!item.is_object()) {
            throw std::invalid_argument("schema fields[]: expected object");
        }
        if (!item.contains("name") || !item["name"].is_string()) {
            throw std::invalid_argument("schema fields[]: 'name' required");
        }

        std::string name = item["name"];
        if (seen.count(name)) {
            throw std::invalid_argument("schema fields[]: duplicate field \"" + name + "\"");
        }
        seen.insert(name);

        SchemaField sf;
        sf.name = name;

        std::string type_str = item.value("type", "string");
        sf.type = field_type_from_string(type_str);
        sf.indexable = item.value("indexable", false);

        s.fields.push_back(std::move(sf));
    }
    return s;
}

} // namespace flux
