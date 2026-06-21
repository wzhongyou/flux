#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <stdexcept>

namespace flux {

// ============================================================
// Schema — typed metadata schema (mirrors Go schema.go)
// ============================================================

enum class FieldType {
    String,
    Float,
    Int,
    Bool,
    Text,   // indexed for BM25 full-text search
    Geo,    // {"lat": double, "lng": double}
};

FieldType field_type_from_string(const std::string& s);
const char* field_type_to_string(FieldType ft);

struct SchemaField {
    std::string name;
    FieldType type;
    bool indexable = false; // build inverted index for pre-filtering
};

struct Schema {
    std::vector<SchemaField> fields;

    // Validate metadata against this schema.
    // Throws std::invalid_argument on type mismatch.
    void validate(const nlohmann::json& metadata) const;

    // Field accessors
    std::vector<std::string> field_names() const;
    std::vector<SchemaField> indexable_fields() const;
    bool has_indexable_fields() const;

    // Returns the first text-type field name, or empty string.
    std::string text_field() const;

    // Find a field by name (returns nullptr if not found).
    const SchemaField* find_field(const std::string& name) const;
};

// Parse Schema from a raw JSON map (for HTTP API).
Schema schema_from_json(const nlohmann::json& raw);

// Helper: validate a single value against a field type.
void validate_field_type(const std::string& field_name, FieldType ft, const nlohmann::json& val);

} // namespace flux
