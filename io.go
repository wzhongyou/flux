package flux

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
)

// ============================================================
// Import / Export
// ============================================================

// exportDoc is a JSON-serializable document for export.
type exportDoc struct {
	ID       string                 `json:"id"`
	Vector   []float64              `json:"vector"`
	Metadata map[string]interface{} `json:"metadata,omitempty"`
}

// ExportCollection writes all documents in a collection as JSON Lines
// (one JSON object per line) to the given writer.
func (db *VectorDatabase) ExportCollection(collectionName string, w io.Writer) (int, error) {
	db.mu.RLock()
	defer db.mu.RUnlock()

	collection, ok := db.collections[collectionName]
	if !ok {
		return 0, fmt.Errorf("collection %q not found", collectionName)
	}

	encoder := json.NewEncoder(w)
	count := 0
	for _, doc := range collection.Docs {
		ed := exportDoc{
			ID:       doc.ID,
			Vector:   doc.Vector,
			Metadata: doc.Metadata,
		}
		if err := encoder.Encode(ed); err != nil {
			return count, fmt.Errorf("export encode: %w", err)
		}
		count++
	}

	return count, nil
}

// ExportCollectionToFile exports a collection to a JSON Lines file.
func (db *VectorDatabase) ExportCollectionToFile(collectionName, path string) (int, error) {
	// Use the database's own WAL approach for file writing simplicity
	// but for now just use the generic writer
	return 0, fmt.Errorf("use ExportCollection with a file writer")
}

// ImportCollection reads JSON Lines from the reader and upserts
// documents into the given collection. If an index is active on
// the collection, it is maintained incrementally.
//
// Format: one JSON object per line with fields: id, vector, metadata (optional)
func (db *VectorDatabase) ImportCollection(collectionName string, r io.Reader) (int, error) {
	// Verify collection exists first
	db.mu.RLock()
	_, ok := db.collections[collectionName]
	db.mu.RUnlock()
	if !ok {
		return 0, fmt.Errorf("collection %q not found", collectionName)
	}

	scanner := bufio.NewScanner(r)
	scanner.Buffer(make([]byte, 1024*1024), 1024*1024) // 1MB max line

	lineNumber := 0
	count := 0
	for scanner.Scan() {
		lineNumber++
		line := scanner.Bytes()
		if len(line) == 0 || line[0] == '#' || line[0] == '/' {
			// Skip empty lines and comments
			continue
		}

		var ed exportDoc
		if err := json.Unmarshal(line, &ed); err != nil {
			return count, fmt.Errorf("import line %d: %w", lineNumber, err)
		}

		doc := &Document{
			ID:       ed.ID,
			Vector:   ed.Vector,
			Metadata: ed.Metadata,
		}

		if err := db.Upsert(collectionName, doc); err != nil {
			return count, fmt.Errorf("import line %d upsert: %w", lineNumber, err)
		}
		count++
	}

	if err := scanner.Err(); err != nil {
		return count, fmt.Errorf("import scan: %w", err)
	}

	return count, nil
}
