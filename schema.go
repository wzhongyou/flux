package flux

import (
	"fmt"
	"strings"
)

// ============================================================
// Schema — typed metadata schema for collections
// ============================================================

// FieldType represents the type of a metadata field.
type FieldType string

const (
	FieldTypeString FieldType = "string"
	FieldTypeFloat  FieldType = "float"
	FieldTypeInt    FieldType = "int"
	FieldTypeBool   FieldType = "bool"
	FieldTypeText   FieldType = "text" // indexed for BM25 full-text search
	FieldTypeGeo    FieldType = "geo"  // {"lat": float64, "lng": float64}
)

// SchemaField defines a single field in a collection's schema.
type SchemaField struct {
	Name      string    `json:"name"`
	Type      FieldType `json:"type"`
	Indexable bool      `json:"indexable,omitempty"` // build invert index for pre-filtering
}

// Schema defines the typed metadata structure for a collection.
type Schema struct {
	Fields []SchemaField `json:"fields"`
}

// Validate checks whether the given metadata matches the schema types.
func (s *Schema) Validate(metadata map[string]interface{}) error {
	if s == nil || len(s.Fields) == 0 {
		return nil
	}
	for _, field := range s.Fields {
		val, ok := metadata[field.Name]
		if !ok {
			continue // missing optional field is OK
		}
		if err := validateFieldType(field.Name, field.Type, val); err != nil {
			return err
		}
	}
	return nil
}

func validateFieldType(name string, ft FieldType, val interface{}) error {
	switch ft {
	case FieldTypeString:
		if _, ok := val.(string); !ok {
			return fmt.Errorf("field %q: expected string, got %T", name, val)
		}
	case FieldTypeFloat:
		switch val.(type) {
		case float64, float32:
			return nil
		case int:
			return nil // int is OK for float fields
		default:
			return fmt.Errorf("field %q: expected number, got %T", name, val)
		}
	case FieldTypeInt:
		switch val.(type) {
		case int, int32, int64:
			return nil
		case float64:
			f := val.(float64)
			if f != float64(int64(f)) {
				return fmt.Errorf("field %q: expected integer, got float %v", name, f)
			}
			return nil
		default:
			return fmt.Errorf("field %q: expected integer, got %T", name, val)
		}
	case FieldTypeBool:
		if _, ok := val.(bool); !ok {
			return fmt.Errorf("field %q: expected bool, got %T", name, val)
		}
	case FieldTypeText:
		if _, ok := val.(string); !ok {
			return fmt.Errorf("field %q: expected text (string), got %T", name, val)
		}
	case FieldTypeGeo:
		m, ok := val.(map[string]interface{})
		if !ok {
			return fmt.Errorf("field %q: expected geo object {lat, lng}, got %T", name, val)
		}
		_, hasLat := m["lat"]
		_, hasLng := m["lng"]
		if !hasLat || !hasLng {
			return fmt.Errorf("field %q: geo must have 'lat' and 'lng' fields", name)
		}
	default:
		return fmt.Errorf("field %q: unknown type %q", name, ft)
	}
	return nil
}

// FieldNames returns the list of field names in the schema.
func (s *Schema) FieldNames() []string {
	if s == nil {
		return nil
	}
	names := make([]string, len(s.Fields))
	for i, f := range s.Fields {
		names[i] = f.Name
	}
	return names
}

// IndexableFields returns fields that have Indexable set to true.
func (s *Schema) IndexableFields() []SchemaField {
	if s == nil {
		return nil
	}
	var result []SchemaField
	for _, f := range s.Fields {
		if f.Indexable {
			result = append(result, f)
		}
	}
	return result
}

// TextField returns the first text-type field name, or empty string.
func (s *Schema) TextField() string {
	if s == nil {
		return ""
	}
	for _, f := range s.Fields {
		if f.Type == FieldTypeText {
			return f.Name
		}
	}
	return ""
}

// HasIndexableFields returns true if any field has Indexable set.
func (s *Schema) HasIndexableFields() bool {
	if s == nil {
		return false
	}
	for _, f := range s.Fields {
		if f.Indexable {
			return true
		}
	}
	return false
}

// findField finds a schema field by name.
func (s *Schema) findField(name string) *SchemaField {
	if s == nil {
		return nil
	}
	for i := range s.Fields {
		if s.Fields[i].Name == name {
			return &s.Fields[i]
		}
	}
	return nil
}

// ============================================================
// JSON-friendly collection creation request schema
// ============================================================

// SchemaFromMap parses a raw map into a Schema (for HTTP API).
func SchemaFromMap(raw map[string]interface{}) (*Schema, error) {
	fieldsRaw, ok := raw["fields"]
	if !ok {
		return nil, fmt.Errorf("schema requires 'fields' array")
	}
	fieldsArr, ok := fieldsRaw.([]interface{})
	if !ok {
		return nil, fmt.Errorf("'fields' must be an array")
	}
	s := &Schema{Fields: make([]SchemaField, 0, len(fieldsArr))}
	seen := make(map[string]bool)
	for i, item := range fieldsArr {
		fm, ok := item.(map[string]interface{})
		if !ok {
			return nil, fmt.Errorf("fields[%d]: expected object", i)
		}
		name, _ := fm["name"].(string)
		if name == "" {
			return nil, fmt.Errorf("fields[%d]: 'name' required", i)
		}
		if seen[name] {
			return nil, fmt.Errorf("fields[%d]: duplicate field %q", i, name)
		}
		seen[name] = true
		ftStr, _ := fm["type"].(string)
		ft := FieldType(strings.ToLower(ftStr))
		switch ft {
		case FieldTypeString, FieldTypeFloat, FieldTypeInt, FieldTypeBool, FieldTypeText, FieldTypeGeo:
		default:
			return nil, fmt.Errorf("fields[%d]: unsupported type %q", i, ftStr)
		}
		indexable, _ := fm["indexable"].(bool)
		s.Fields = append(s.Fields, SchemaField{Name: name, Type: ft, Indexable: indexable})
	}
	return s, nil
}
