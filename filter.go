package flux

import (
	"fmt"
	"math"
	"sort"
	"strings"
	"sync"
)

// ============================================================
// Rich Filter Combinators
// ============================================================

// Or returns a FilterFunc that matches if any of the given filters match.
func Or(filters ...FilterFunc) FilterFunc {
	return func(metadata map[string]interface{}) bool {
		for _, filter := range filters {
			if filter(metadata) {
				return true
			}
		}
		return false
	}
}

// Not returns a FilterFunc that inverts the given filter.
func Not(filter FilterFunc) FilterFunc {
	return func(metadata map[string]interface{}) bool {
		return !filter(metadata)
	}
}

// FieldIn returns a FilterFunc that matches if the field's value is in the given set.
func FieldIn(field string, values ...interface{}) FilterFunc {
	return func(metadata map[string]interface{}) bool {
		if metadata == nil {
			return false
		}
		actual, ok := metadata[field]
		if !ok {
			return false
		}
		for _, v := range values {
			if equalValues(actual, v) {
				return true
			}
		}
		return false
	}
}

// FieldExists returns a FilterFunc that matches if the field exists (even if null/empty).
func FieldExists(field string) FilterFunc {
	return func(metadata map[string]interface{}) bool {
		if metadata == nil {
			return false
		}
		_, ok := metadata[field]
		return ok
	}
}

// FieldNotEqual returns a FilterFunc that matches if the field's value is NOT equal.
func FieldNotEqual(field string, value interface{}) FilterFunc {
	return func(metadata map[string]interface{}) bool {
		if metadata == nil {
			return true
		}
		actual, ok := metadata[field]
		if !ok {
			return true // field doesn't exist → not equal
		}
		return !equalValues(actual, value)
	}
}

// TextMatch returns a FilterFunc that matches if the field contains the given substring.
// Matching is case-insensitive.
func TextMatch(field, pattern string) FilterFunc {
	lowerPattern := strings.ToLower(pattern)
	return func(metadata map[string]interface{}) bool {
		if metadata == nil {
			return false
		}
		raw, ok := metadata[field]
		if !ok {
			return false
		}
		s, ok := raw.(string)
		if !ok {
			return false
		}
		return strings.Contains(strings.ToLower(s), lowerPattern)
	}
}

// GeoRadius returns a FilterFunc that matches if the geo field's coordinates
// are within the given radius (in kilometers) from the center point.
// The geo field should be a map[string]interface{} with "lat" and "lng" keys.
func GeoRadius(field string, centerLat, centerLng, radiusKm float64) FilterFunc {
	return func(metadata map[string]interface{}) bool {
		if metadata == nil {
			return false
		}
		raw, ok := metadata[field]
		if !ok {
			return false
		}
		geo, ok := raw.(map[string]interface{})
		if !ok {
			return false
		}
		lat, ok := toFloat64(geo["lat"])
		if !ok {
			return false
		}
		lng, ok := toFloat64(geo["lng"])
		if !ok {
			return false
		}
		return haversine(centerLat, centerLng, lat, lng) <= radiusKm
	}
}

// haversine calculates the great-circle distance in km between two points.
func haversine(lat1, lng1, lat2, lng2 float64) float64 {
	const R = 6371.0 // Earth radius in km
	dLat := (lat2 - lat1) * math.Pi / 180
	dLng := (lng2 - lng1) * math.Pi / 180
	a := math.Sin(dLat/2)*math.Sin(dLat/2) +
		math.Cos(lat1*math.Pi/180)*math.Cos(lat2*math.Pi/180)*
			math.Sin(dLng/2)*math.Sin(dLng/2)
	c := 2 * math.Atan2(math.Sqrt(a), math.Sqrt(1-a))
	return R * c
}

// FilterToJSON returns a human-readable description of a filter for explain output.
// Since FilterFunc is a closure, we can only describe the built-in combinators
// by pattern matching on their behavior indirectly.
// For explain output, use the JSON representation from the API layer instead.
func FilterToJSON(field string, op string, value interface{}) string {
	return fmt.Sprintf(`{"field":%q,"op":%q,"value":%v}`, field, op, value)
}

// ============================================================
// MetadataInvertedIndex — pre-filtering for metadata fields
// ============================================================

// MetadataInvertedIndex provides fast pre-filtering for metadata fields.
// It maintains inverted maps: field → value → set of docIDs.
// Only fields marked as Indexable in the Schema are indexed.
type MetadataInvertedIndex struct {
	mu     sync.RWMutex
	fields map[string]map[string]map[string]bool // field → value/key → set of docIDs
	// Range index for numeric fields: field → docID → min/max placeholder
	// We use sorted lists for range queries
	ranges map[string]map[string]float64 // field → docID → numeric value
}

// NewMetadataInvertedIndex creates a new metadata inverted index.
func NewMetadataInvertedIndex() *MetadataInvertedIndex {
	return &MetadataInvertedIndex{
		fields: make(map[string]map[string]map[string]bool),
		ranges: make(map[string]map[string]float64),
	}
}

// Insert adds a document's metadata to the index.
func (idx *MetadataInvertedIndex) Insert(docID string, metadata map[string]interface{}) error {
	idx.mu.Lock()
	defer idx.mu.Unlock()

	for field, value := range metadata {
		key := valueToKey(value)
		if idx.fields[field] == nil {
			idx.fields[field] = make(map[string]map[string]bool)
		}
		if idx.fields[field][key] == nil {
			idx.fields[field][key] = make(map[string]bool)
		}
		idx.fields[field][key][docID] = true

		// Track numeric values for range queries
		if fv, ok := toFloat64(value); ok {
			if idx.ranges[field] == nil {
				idx.ranges[field] = make(map[string]float64)
			}
			idx.ranges[field][docID] = fv
		}
	}
	return nil
}

// Delete removes a document from the index.
func (idx *MetadataInvertedIndex) Delete(docID string) {
	idx.mu.Lock()
	defer idx.mu.Unlock()

	for field, values := range idx.fields {
		for key, ids := range values {
			delete(ids, docID)
			if len(ids) == 0 {
				delete(idx.fields[field], key)
			}
		}
	}
	for field := range idx.ranges {
		delete(idx.ranges[field], docID)
	}
}

// Match returns the set of docIDs where field == value.
func (idx *MetadataInvertedIndex) Match(field string, value interface{}) map[string]bool {
	idx.mu.RLock()
	defer idx.mu.RUnlock()

	key := valueToKey(value)
	if idx.fields[field] == nil {
		return nil
	}
	ids := idx.fields[field][key]
	if ids == nil {
		return nil
	}
	// Return a copy to avoid concurrent modification
	result := make(map[string]bool, len(ids))
	for id := range ids {
		result[id] = true
	}
	return result
}

// MatchRange returns the set of docIDs where field is in [min, max].
func (idx *MetadataInvertedIndex) MatchRange(field string, min, max float64) map[string]bool {
	idx.mu.RLock()
	defer idx.mu.RUnlock()

	fieldRanges, ok := idx.ranges[field]
	if !ok || len(fieldRanges) == 0 {
		return nil
	}

	result := make(map[string]bool)
	for docID, val := range fieldRanges {
		if val >= min && val <= max {
			result[docID] = true
		}
	}
	return result
}

// MatchIn returns the set of docIDs where field is in the given value set.
func (idx *MetadataInvertedIndex) MatchIn(field string, values []interface{}) map[string]bool {
	idx.mu.RLock()
	defer idx.mu.RUnlock()

	if idx.fields[field] == nil {
		return nil
	}

	result := make(map[string]bool)
	for _, v := range values {
		key := valueToKey(v)
		if ids, ok := idx.fields[field][key]; ok {
			for id := range ids {
				result[id] = true
			}
		}
	}
	return result
}

// All returns all indexed document IDs.
func (idx *MetadataInvertedIndex) All() map[string]bool {
	idx.mu.RLock()
	defer idx.mu.RUnlock()

	result := make(map[string]bool)
	for _, values := range idx.fields {
		for _, ids := range values {
			for id := range ids {
				result[id] = true
			}
		}
	}
	return result
}

// Clear removes all entries.
func (idx *MetadataInvertedIndex) Clear() {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	idx.fields = make(map[string]map[string]map[string]bool)
	idx.ranges = make(map[string]map[string]float64)
}

// valueToKey converts a metadata value to a string key for the inverted index.
func valueToKey(value interface{}) string {
	switch v := value.(type) {
	case string:
		return "s:" + v
	case float64:
		return fmt.Sprintf("f:%v", v)
	case int:
		return fmt.Sprintf("i:%d", v)
	case bool:
		if v {
			return "b:true"
		}
		return "b:false"
	default:
		return fmt.Sprintf("?:%v", v)
	}
}

// Snapshot returns a serializable representation of the index state.
// For snapshot purposes, we return field → value → sorted docIDs.
func (idx *MetadataInvertedIndex) Snapshot() map[string]map[string][]string {
	idx.mu.RLock()
	defer idx.mu.RUnlock()

	result := make(map[string]map[string][]string)
	for field, values := range idx.fields {
		result[field] = make(map[string][]string)
		for key, ids := range values {
			sorted := make([]string, 0, len(ids))
			for id := range ids {
				sorted = append(sorted, id)
			}
			sort.Strings(sorted)
			result[field][key] = sorted
		}
	}
	return result
}

// LoadSnapshot restores the index from a snapshot.
func (idx *MetadataInvertedIndex) LoadSnapshot(data map[string]map[string][]string) {
	idx.mu.Lock()
	defer idx.mu.Unlock()

	idx.fields = make(map[string]map[string]map[string]bool)
	for field, values := range data {
		idx.fields[field] = make(map[string]map[string]bool)
		for key, ids := range values {
			idx.fields[field][key] = make(map[string]bool)
			for _, id := range ids {
				idx.fields[field][key][id] = true
			}
		}
	}
}

// Len returns the total number of indexed entries.
func (idx *MetadataInvertedIndex) Len() int {
	idx.mu.RLock()
	defer idx.mu.RUnlock()

	count := 0
	for _, values := range idx.fields {
		for _, ids := range values {
			count += len(ids)
		}
	}
	return count
}
