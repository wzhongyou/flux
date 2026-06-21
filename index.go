package flux

import "io"

// Index is the interface for ANN (Approximate Nearest Neighbor) index structures.
// Each Collection can optionally hold one Index to accelerate search.
type Index interface {
	// Insert adds a vector to the index under the given docID.
	// If the docID already exists, it is replaced (upsert semantics).
	Insert(docID string, vector []float64) error

	// Delete lazily or eagerly removes a docID from the index.
	Delete(docID string)

	// SearchInternal returns the top-k nearest neighbors as SearchResult entries
	// with ID and Score populated, but NOT Document/Metadata.
	// The caller (Collection.Search) resolves Document references and applies filters.
	SearchInternal(query []float64, k int) ([]SearchResult, error)

	// Save persists the full index state to a writer.
	Save(w io.Writer) error

	// Load restores the full index state from a reader.
	Load(r io.Reader) error

	// Len returns the number of active (non-deleted) indexed vectors.
	Len() int
}

// FilteredIndex is an optional extension of Index for indexes that support
// pre-filtered search via a candidate document ID set.
// This interface is checked at runtime via type assertion, preserving
// backward compatibility with the base Index interface.
type FilteredIndex interface {
	Index

	// SearchInternalWithFilter returns the top-k nearest neighbors whose
	// document IDs are in the candidates set. If candidates is empty,
	// an empty result is returned.
	SearchInternalWithFilter(query []float64, k int, candidates map[string]bool) ([]SearchResult, error)
}
