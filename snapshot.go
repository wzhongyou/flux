package flux

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
)

// ============================================================
// Snapshot Types
// ============================================================

// snapshotCollection represents a serializable collection state.
type snapshotCollection struct {
	Name      string `json:"name"`
	Metric    DistanceMetric `json:"metric"`
	Dimension int    `json:"dimension"`
	IndexType string `json:"index_type,omitempty"`
}

// snapshotDoc is a serializable document stored in a snapshot.
type snapshotDoc struct {
	ID       string                 `json:"id"`
	Vector   []float64              `json:"vector"`
	Metadata map[string]interface{} `json:"metadata,omitempty"`
}

// dbSnapshot is the complete serializable state of a VectorDatabase.
type dbSnapshot struct {
	Version     int                          `json:"version"`
	Collections []snapshotCollection         `json:"collections"`
	Documents   map[string][]snapshotDoc    `json:"documents"` // collection name -> docs
	IndexType   map[string]string            `json:"index_types"` // collection name -> index type
}

// ============================================================
// Snapshot Operations
// ============================================================

// Snapshot saves the complete database state to a file.
// This includes all collections, documents, and index metadata.
// Returns the path to the snapshot file.
//
// After a successful snapshot, the WAL can be safely truncated
// (see TruncateWAL).
func (db *VectorDatabase) Snapshot(path string) error {
	db.mu.RLock()
	defer db.mu.RUnlock()

	snap := dbSnapshot{
		Version:     1,
		Collections: make([]snapshotCollection, 0, len(db.collections)),
		Documents:   make(map[string][]snapshotDoc),
		IndexType:   make(map[string]string),
	}

	for name, col := range db.collections {
		// Clone to avoid holding Document pointers
		col := col

		snap.Collections = append(snap.Collections, snapshotCollection{
			Name:      col.Name,
			Metric:    col.Metric,
			Dimension: col.Dimension,
		})

		docs := make([]snapshotDoc, 0, len(col.Docs))
		for _, doc := range col.Docs {
			cloned, _ := doc.Clone()
			docs = append(docs, snapshotDoc{
				ID:       cloned.ID,
				Vector:   cloned.Vector,
				Metadata: cloned.Metadata,
			})
		}
		snap.Documents[name] = docs

		if col.IndexType != "" {
			snap.IndexType[name] = col.IndexType
		}
	}

	// Ensure directory exists
	dir := filepath.Dir(path)
	if dir != "" && dir != "." {
		if err := os.MkdirAll(dir, 0755); err != nil {
			return fmt.Errorf("snapshot mkdir: %w", err)
		}
	}

	f, err := os.Create(path)
	if err != nil {
		return fmt.Errorf("snapshot create: %w", err)
	}
	defer f.Close()

	enc := json.NewEncoder(f)
	enc.SetIndent("", "  ")
	if err := enc.Encode(snap); err != nil {
		return fmt.Errorf("snapshot encode: %w", err)
	}

	return nil
}

// LoadFromSnapshot restores the database state from a snapshot file.
// It replaces all current in-memory state.
// After loading, any remaining WAL events should be replayed on top.
func (db *VectorDatabase) LoadFromSnapshot(path string) error {
	f, err := os.Open(path)
	if err != nil {
		return fmt.Errorf("snapshot open: %w", err)
	}
	defer f.Close()

	var snap dbSnapshot
	dec := json.NewDecoder(f)
	if err := dec.Decode(&snap); err != nil {
		return fmt.Errorf("snapshot decode: %w", err)
	}

	db.mu.Lock()
	defer db.mu.Unlock()

	// Clear existing state
	db.collections = make(map[string]*Collection)

	for _, cs := range snap.Collections {
		col := &Collection{
			Name:      cs.Name,
			Metric:    cs.Metric,
			Dimension: cs.Dimension,
			Docs:      make(map[string]*Document),
		}

		// Restore documents
		docs := snap.Documents[cs.Name]
		for _, d := range docs {
			col.Docs[d.ID] = &Document{
				ID:       d.ID,
				Vector:   d.Vector,
				Metadata: d.Metadata,
			}
		}

		// Restore index type (not the index itself — WAL replay will rebuild it)
		if it, ok := snap.IndexType[cs.Name]; ok {
			col.IndexType = it
		}

		db.collections[cs.Name] = col
	}

	return nil
}

// TruncateWAL truncates the WAL file to zero length.
// This should only be called after a successful snapshot.
func (db *VectorDatabase) TruncateWAL() error {
	if db.wal == nil {
		return nil
	}
	return db.wal.Truncate()
}
