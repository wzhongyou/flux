package flux

import (
	"encoding/json"
	"fmt"
	"math"
	"sort"
	"sync"
)

type DistanceMetric string

const (
	Cosine       DistanceMetric = "cosine"
	Euclidean    DistanceMetric = "l2"
	InnerProduct DistanceMetric = "ip"
)

type Document struct {
	ID       string                 `json:"id"`
	Vector   []float64              `json:"vector"`
	Metadata map[string]interface{} `json:"metadata,omitempty"`
}

type SearchResult struct {
	ID       string    `json:"id"`
	Score    float64   `json:"score"`
	Document *Document `json:"document,omitempty"`
}

type FilterFunc func(metadata map[string]interface{}) bool

func FieldEqual(field string, value interface{}) FilterFunc {
	return func(metadata map[string]interface{}) bool {
		if metadata == nil {
			return false
		}
		actual, ok := metadata[field]
		if !ok {
			return false
		}
		return equalValues(actual, value)
	}
}

func FieldRange(field string, min, max float64) FilterFunc {
	return func(metadata map[string]interface{}) bool {
		if metadata == nil {
			return false
		}
		raw, ok := metadata[field]
		if !ok {
			return false
		}
		value, ok := toFloat64(raw)
		if !ok {
			return false
		}
		return value >= min && value <= max
	}
}

func And(filters ...FilterFunc) FilterFunc {
	return func(metadata map[string]interface{}) bool {
		for _, filter := range filters {
			if !filter(metadata) {
				return false
			}
		}
		return true
	}
}

func equalValues(a, b interface{}) bool {
	switch x := a.(type) {
	case string:
		y, ok := b.(string)
		return ok && x == y
	case float64:
		if y, ok := b.(float64); ok {
			return x == y
		}
		if y, ok := toFloat64(b); ok {
			return x == y
		}
	case int:
		if y, ok := toFloat64(b); ok {
			return float64(x) == y
		}
	}
	return false
}

func toFloat64(value interface{}) (float64, bool) {
	switch v := value.(type) {
	case float32:
		return float64(v), true
	case float64:
		return v, true
	case int:
		return float64(v), true
	case int32:
		return float64(v), true
	case int64:
		return float64(v), true
	case uint:
		return float64(v), true
	case uint32:
		return float64(v), true
	case uint64:
		return float64(v), true
	default:
		return 0, false
	}
}

// ============================================================
// VectorDatabase
// ============================================================

type VectorDatabase struct {
	mu          sync.RWMutex
	collections map[string]*Collection
	wal         *WAL
	Metrics     *Metrics
}

type Collection struct {
	Name      string
	Metric    DistanceMetric
	Dimension int
	Docs      map[string]*Document

	// Index is the optional ANN index for accelerated search.
	Index Index
	// IndexType records the type of index ("hnsw", "ivf", or "" for brute force).
	IndexType string

	// Schema defines the typed metadata structure for this collection (optional).
	Schema *Schema
	// MetaIndex is the inverted index for pre-filtering metadata fields.
	MetaIndex *MetadataInvertedIndex

	// TextField is the metadata field name to index for BM25 full-text search.
	TextField string
	// BM25 is the BM25 full-text index (created when TextField is set).
	BM25 *BM25Index
}

func NewVectorDatabase(walPath string) (*VectorDatabase, error) {
	db := &VectorDatabase{
		collections: make(map[string]*Collection),
		Metrics:     NewMetrics(),
	}
	if walPath == "" {
		return db, nil
	}

	wal, err := NewWAL(walPath)
	if err != nil {
		return nil, err
	}
	db.wal = wal
	if err := db.replayWAL(); err != nil {
		wal.Close()
		return nil, err
	}
	return db, nil
}

func (db *VectorDatabase) Close() error {
	if db.wal == nil {
		return nil
	}
	return db.wal.Close()
}

// ============================================================
// Collection Management
// ============================================================

func (db *VectorDatabase) CreateCollection(name string, metric DistanceMetric) error {
	if name == "" {
		return fmt.Errorf("collection name required")
	}
	if metric == "" {
		metric = Cosine
	}

	db.mu.Lock()
	defer db.mu.Unlock()
	if _, ok := db.collections[name]; ok {
		return fmt.Errorf("collection %q already exists", name)
	}
	collection := &Collection{
		Name:      name,
		Metric:    metric,
		Docs:      make(map[string]*Document),
		Index:     nil,
		IndexType: "",
	}
	db.collections[name] = collection
	if db.wal != nil {
		return db.wal.Append(&walEvent{Action: "create_collection", Collection: name, Metric: metric})
	}
	return nil
}

// CreateCollectionWithSchema creates a new collection with a typed metadata schema.
// The schema enables metadata validation on upsert and optional inverted indexing
// for pre-filtered search.
func (db *VectorDatabase) CreateCollectionWithSchema(name string, metric DistanceMetric, schema *Schema) error {
	if name == "" {
		return fmt.Errorf("collection name required")
	}
	if metric == "" {
		metric = Cosine
	}

	db.mu.Lock()
	defer db.mu.Unlock()
	if _, ok := db.collections[name]; ok {
		return fmt.Errorf("collection %q already exists", name)
	}

	collection := &Collection{
		Name:      name,
		Metric:    metric,
		Docs:      make(map[string]*Document),
		Schema:    schema,
		Index:     nil,
		IndexType: "",
	}
	if schema != nil && schema.HasIndexableFields() {
		collection.MetaIndex = NewMetadataInvertedIndex()
	}
	if schema != nil {
		if tf := schema.TextField(); tf != "" {
			collection.TextField = tf
			collection.BM25 = NewBM25Index()
		}
	}
	db.collections[name] = collection
	if db.wal != nil {
		return db.wal.Append(&walEvent{Action: "create_collection", Collection: name, Metric: metric})
	}
	return nil
}

// BuildIndex builds an ANN index for the given collection.
// Supported index types: "hnsw". More will be added in future.
// If the collection already has documents, they are indexed immediately.
func (db *VectorDatabase) BuildIndex(collectionName string, indexType string) error {
	db.mu.Lock()
	defer db.mu.Unlock()

	collection, ok := db.collections[collectionName]
	if !ok {
		return fmt.Errorf("collection %q not found", collectionName)
	}

	switch indexType {
	case "hnsw":
		dim := collection.Dimension
		if dim == 0 {
			dim = 128 // default dimension for empty collection
		}
		idx := NewHNSWIndex(collection.Metric, dim)
		for _, doc := range collection.Docs {
			if err := idx.Insert(doc.ID, doc.Vector); err != nil {
				return fmt.Errorf("hnsw insert %q: %w", doc.ID, err)
			}
		}
		collection.Index = idx
		collection.IndexType = "hnsw"
		return nil
	case "ivf":
		dim := collection.Dimension
		if dim == 0 {
			dim = 128
		}
		idx := NewIVFIndex(collection.Metric, dim)
		for _, doc := range collection.Docs {
			if err := idx.Insert(doc.ID, doc.Vector); err != nil {
				return fmt.Errorf("ivf insert %q: %w", doc.ID, err)
			}
		}
		idx.Build() // Run K-means clustering
		collection.Index = idx
		collection.IndexType = "ivf"
		return nil
	default:
		return fmt.Errorf("unsupported index type %q (supported: hnsw, ivf)", indexType)
	}}

// DropIndex removes the ANN index from the given collection,
// falling back to brute-force search.
func (db *VectorDatabase) DropIndex(collectionName string) error {
	db.mu.Lock()
	defer db.mu.Unlock()

	collection, ok := db.collections[collectionName]
	if !ok {
		return fmt.Errorf("collection %q not found", collectionName)
	}
	collection.Index = nil
	collection.IndexType = ""
	return nil
}

// IndexInfo returns the index type for a collection, or "" if none.
func (db *VectorDatabase) IndexInfo(collectionName string) (string, error) {
	db.mu.RLock()
	defer db.mu.RUnlock()

	collection, ok := db.collections[collectionName]
	if !ok {
		return "", fmt.Errorf("collection %q not found", collectionName)
	}
	if collection.Index == nil {
		return "", nil
	}
	return collection.IndexType, nil
}

// ============================================================
// Document Operations
// ============================================================

func (db *VectorDatabase) Upsert(collectionName string, doc *Document) error {
	if doc == nil {
		return fmt.Errorf("document required")
	}
	if doc.ID == "" {
		return fmt.Errorf("document id required")
	}
	db.mu.Lock()
	defer db.mu.Unlock()
	collection, ok := db.collections[collectionName]
	if !ok {
		return fmt.Errorf("collection %q not found", collectionName)
	}
	if collection.Dimension == 0 {
		collection.Dimension = len(doc.Vector)
	} else if len(doc.Vector) != collection.Dimension {
		return fmt.Errorf("dimension mismatch: expected %d, got %d", collection.Dimension, len(doc.Vector))
	}
	// Validate schema if present
	if collection.Schema != nil {
		if err := collection.Schema.Validate(doc.Metadata); err != nil {
			return err
		}
	}

	collection.Docs[doc.ID] = doc

	// Update metadata inverted index if present
	if collection.MetaIndex != nil {
		if err := collection.MetaIndex.Insert(doc.ID, doc.Metadata); err != nil {
			return fmt.Errorf("meta index insert: %w", err)
		}
	}

	// Index text content for BM25 if configured
	if collection.BM25 != nil && collection.TextField != "" {
		if text, ok := doc.Metadata[collection.TextField].(string); ok && text != "" {
			collection.BM25.IndexDocument(doc.ID, text)
		}
	}

	// Update ANN index if present
	if collection.Index != nil {
		if err := collection.Index.Insert(doc.ID, doc.Vector); err != nil {
			return fmt.Errorf("index insert: %w", err)
		}
	}

	db.Metrics.IncUpsert()

	if db.wal != nil {
		return db.wal.Append(&walEvent{Action: "upsert", Collection: collectionName, Document: doc})
	}
	return nil
}

func (db *VectorDatabase) Delete(collectionName, id string) error {
	if id == "" {
		return fmt.Errorf("document id required")
	}
	db.mu.Lock()
	defer db.mu.Unlock()
	collection, ok := db.collections[collectionName]
	if !ok {
		return fmt.Errorf("collection %q not found", collectionName)
	}
	delete(collection.Docs, id)

	// Update metadata inverted index if present
	if collection.MetaIndex != nil {
		collection.MetaIndex.Delete(id)
	}

	// Update ANN index if present
	if collection.Index != nil {
		collection.Index.Delete(id)
	}

	db.Metrics.IncDelete()

	if db.wal != nil {
		return db.wal.Append(&walEvent{Action: "delete", Collection: collectionName, ID: id})
	}
	return nil
}

// BatchUpsert inserts or updates multiple documents in a single atomic operation.
// Returns the count of successfully upserted documents.
func (db *VectorDatabase) BatchUpsert(collectionName string, docs []*Document) (int, error) {
	if len(docs) == 0 {
		return 0, fmt.Errorf("at least one document required")
	}

	db.mu.Lock()
	defer db.mu.Unlock()

	collection, ok := db.collections[collectionName]
	if !ok {
		return 0, fmt.Errorf("collection %q not found", collectionName)
	}

	// Validate all docs first (fail-fast)
	for _, doc := range docs {
		if doc.ID == "" {
			return 0, fmt.Errorf("document id required")
		}
		if collection.Dimension == 0 {
			collection.Dimension = len(doc.Vector)
		} else if len(doc.Vector) != collection.Dimension {
			return 0, fmt.Errorf("dimension mismatch: expected %d, got %d", collection.Dimension, len(doc.Vector))
		}
		if collection.Schema != nil {
			if err := collection.Schema.Validate(doc.Metadata); err != nil {
				return 0, err
			}
		}
	}

	// Apply all docs
	for _, doc := range docs {
		collection.Docs[doc.ID] = doc

		if collection.MetaIndex != nil {
			collection.MetaIndex.Insert(doc.ID, doc.Metadata)
		}
		if collection.Index != nil {
			collection.Index.Insert(doc.ID, doc.Vector)
		}
	}

	db.Metrics.IncUpsert()

	if db.wal != nil {
		// Write batch event
		if err := db.wal.Append(&walEvent{
			Action:     "batch_upsert",
			Collection: collectionName,
			Documents:  docs,
		}); err != nil {
			return len(docs), err
		}
	}

	return len(docs), nil
}

// BatchDelete removes multiple documents from a collection in a single atomic operation.
// Returns the count of successfully deleted documents.
func (db *VectorDatabase) BatchDelete(collectionName string, ids []string) (int, error) {
	if len(ids) == 0 {
		return 0, fmt.Errorf("at least one id required")
	}

	db.mu.Lock()
	defer db.mu.Unlock()

	collection, ok := db.collections[collectionName]
	if !ok {
		return 0, fmt.Errorf("collection %q not found", collectionName)
	}

	for _, id := range ids {
		delete(collection.Docs, id)
		if collection.MetaIndex != nil {
			collection.MetaIndex.Delete(id)
		}
		if collection.Index != nil {
			collection.Index.Delete(id)
		}
	}

	db.Metrics.IncDelete()

	if db.wal != nil {
		if err := db.wal.Append(&walEvent{
			Action:     "batch_delete",
			Collection: collectionName,
			IDs:        ids,
		}); err != nil {
			return len(ids), err
		}
	}

	return len(ids), nil
}

// DeleteCollection removes an entire collection and all its data.
func (db *VectorDatabase) DeleteCollection(name string) error {
	db.mu.Lock()
	defer db.mu.Unlock()

	if _, ok := db.collections[name]; !ok {
		return fmt.Errorf("collection %q not found", name)
	}
	delete(db.collections, name)

	if db.wal != nil {
		return db.wal.Append(&walEvent{Action: "delete_collection", Collection: name})
	}
	return nil
}

// TruncateCollection removes all documents from a collection but keeps the collection itself.
func (db *VectorDatabase) TruncateCollection(name string) error {
	db.mu.Lock()
	defer db.mu.Unlock()

	collection, ok := db.collections[name]
	if !ok {
		return fmt.Errorf("collection %q not found", name)
	}

	collection.Docs = make(map[string]*Document)
	if collection.MetaIndex != nil {
		collection.MetaIndex.Clear()
	}
	if collection.Index != nil {
		// Index must be rebuilt externally via BuildIndex
		collection.Index = nil
		collection.IndexType = ""
	}

	if db.wal != nil {
		return db.wal.Append(&walEvent{Action: "truncate_collection", Collection: name})
	}
	return nil
}

// ============================================================
// Search
// ============================================================

// Search performs a vector similarity search on the given collection.
//
// If the collection has an ANN index and no filter is specified, the index
// is used for accelerated search. If a filter is specified with an index,
// post-filtering with oversampling is used, falling back to brute force
// if the filtered results are insufficient.
func (db *VectorDatabase) Search(collectionName string, query []float64, k int, filter FilterFunc) ([]SearchResult, error) {
	if k <= 0 {
		return nil, fmt.Errorf("k must be positive")
	}
	db.mu.RLock()
	defer db.mu.RUnlock()
	collection, ok := db.collections[collectionName]
	if !ok {
		return nil, fmt.Errorf("collection %q not found", collectionName)
	}
	return collection.Search(query, k, filter), nil
}

// HybridSearch performs a combined vector + BM25 text search on a collection.
func (db *VectorDatabase) HybridSearch(collectionName string, query []float64, textQuery string, k int, alpha float64, filter FilterFunc) []HybridSearchResult {
	if k <= 0 {
		return nil
	}
	db.mu.RLock()
	defer db.mu.RUnlock()
	collection, ok := db.collections[collectionName]
	if !ok {
		return nil
	}
	return collection.HybridSearch(query, textQuery, k, alpha, filter)
}

// Search searches this collection.
// It uses the ANN index if available, otherwise falls back to brute force.
func (c *Collection) Search(query []float64, k int, filter FilterFunc) []SearchResult {
	if len(query) == 0 || len(query) != c.Dimension {
		return nil
	}

	if c.Index != nil {
		return c.indexSearch(query, k, filter)
	}

	return c.bruteForceSearch(query, k, filter)
}

// indexSearch uses the ANN index for search, with post-filter fallback.
func (c *Collection) indexSearch(query []float64, k int, filter FilterFunc) []SearchResult {
	// Resolve document references from IDs
	resolveDocs := func(results []SearchResult) {
		for i := range results {
			if doc, ok := c.Docs[results[i].ID]; ok {
				results[i].Document = doc
			}
		}
	}

	if filter == nil {
		// Pure index search — no filter needed
		results, err := c.Index.SearchInternal(query, k)
		if err != nil {
			return nil
		}
		resolveDocs(results)
		return results
	}

	// Pre-filtering path: use metadata inverted index if available
	if c.MetaIndex != nil {
		candidateIDs := resolveFilterWithMetaIndex(c.MetaIndex, filter)
		if len(candidateIDs) == 0 {
			return nil
		}
		// Check if index supports pre-filtered search
		filteredIdx, ok := c.Index.(FilteredIndex)
		if ok {
			results, err := filteredIdx.SearchInternalWithFilter(query, k, candidateIDs)
			if err != nil {
				return nil
			}
			resolveDocs(results)
			return results
		}
		// Fall through to post-filter if index doesn't support pre-filtering
	}

	// Post-filter approach: search with oversampling to compensate for
	// filter selectivity, then apply the filter.
	const oversample = 3
	searchK := k * oversample
	if searchK > len(c.Docs) {
		searchK = len(c.Docs)
	}

	results, err := c.Index.SearchInternal(query, searchK)
	if err != nil {
		return nil
	}

	filtered := make([]SearchResult, 0, len(results))
	for _, r := range results {
		doc, ok := c.Docs[r.ID]
		if !ok {
			continue
		}
		if filter(doc.Metadata) {
			r.Document = doc
			filtered = append(filtered, r)
		}
	}

	sort.Slice(filtered, func(i, j int) bool {
		return filtered[i].Score > filtered[j].Score
	})

	// If post-filtering didn't yield enough results, fall back to brute force
	// for correctness (this ensures highly selective filters still work).
	if len(filtered) < k {
		bfResults := c.bruteForceSearch(query, k, filter)
		if len(bfResults) > len(filtered) {
			return bfResults
		}
	}

	if len(filtered) > k {
		filtered = filtered[:k]
	}

	return filtered
}

// resolveFilterWithMetaIndex uses the metadata inverted index to find candidate
// document IDs that match the given filter. It attempts to extract simple
// equality/range predicates from the FilterFunc.
//
// Note: Since FilterFunc is a closure, we cannot fully inspect its structure.
// This function tries to use the MetaIndex for common patterns and falls
// back to brute-force scanning if the pattern is not recognized.
func resolveFilterWithMetaIndex(mi *MetadataInvertedIndex, filter FilterFunc) map[string]bool {
	// For complex filters that we can't decompose into index lookups,
	// we still need to use the index as guidance. The best we can do
	// is return all indexed documents and let the post-filter refine.
	// In practice, most filters are FieldEqual or FieldRange, which
	// the user should apply through the schema's indexable fields.
	return mi.All()
}

// bruteForceSearch performs an exhaustive scan of all documents,
// applying the filter and computing similarity scores.
func (c *Collection) bruteForceSearch(query []float64, k int, filter FilterFunc) []SearchResult {
	filtered := make([]SearchResult, 0, len(c.Docs))
	for _, doc := range c.Docs {
		if filter != nil && !filter(doc.Metadata) {
			continue
		}
		score := computeScore(c.Metric, query, doc.Vector)
		filtered = append(filtered, SearchResult{ID: doc.ID, Score: score, Document: doc})
	}

	sort.Slice(filtered, func(i, j int) bool {
		return filtered[i].Score > filtered[j].Score
	})

	if len(filtered) > k {
		filtered = filtered[:k]
	}
	return filtered
}

// ============================================================
// Stats & Metadata
// ============================================================

func (db *VectorDatabase) CollectionStats(collectionName string) (int, int, DistanceMetric, error) {
	db.mu.RLock()
	defer db.mu.RUnlock()
	collection, ok := db.collections[collectionName]
	if !ok {
		return 0, 0, "", fmt.Errorf("collection %q not found", collectionName)
	}
	return len(collection.Docs), collection.Dimension, collection.Metric, nil
}

func (db *VectorDatabase) ListCollections() []string {
	db.mu.RLock()
	defer db.mu.RUnlock()
	names := make([]string, 0, len(db.collections))
	for name := range db.collections {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

// RefreshMetrics updates the Metrics collector with current database state.
func (db *VectorDatabase) RefreshMetrics() {
	db.mu.RLock()
	defer db.mu.RUnlock()

	colCount := len(db.collections)
	var vecCount int
	var idxCount int
	for _, col := range db.collections {
		vecCount += len(col.Docs)
		if col.Index != nil {
			idxCount++
		}
	}
	db.Metrics.UpdateCollectionStats(colCount, vecCount, idxCount)
}

// ============================================================
// Distance Computation
// ============================================================

func computeScore(metric DistanceMetric, a, b []float64) float64 {
	dot := 0.0
	for i := range a {
		dot += a[i] * b[i]
	}
	switch metric {
	case Euclidean:
		distance := 0.0
		for i := range a {
			delta := a[i] - b[i]
			distance += delta * delta
		}
		return -math.Sqrt(distance)
	case InnerProduct:
		return dot
	default: // Cosine
		normA := vectorNorm(a)
		normB := vectorNorm(b)
		if normA == 0 || normB == 0 {
			return 0
		}
		return dot / (normA * normB)
	}
}

func vectorNorm(v []float64) float64 {
	value := 0.0
	for _, x := range v {
		value += x * x
	}
	return math.Sqrt(value)
}

// ============================================================
// Document Cloning
// ============================================================

func (d *Document) Clone() (*Document, error) {
	copyVector := make([]float64, len(d.Vector))
	copy(copyVector, d.Vector)
	copyMetadata := make(map[string]interface{}, len(d.Metadata))
	for k, v := range d.Metadata {
		copyMetadata[k] = v
	}
	return &Document{ID: d.ID, Vector: copyVector, Metadata: copyMetadata}, nil
}

// ============================================================
// WAL Replay
// ============================================================

func (db *VectorDatabase) replayWAL() error {
	if db.wal == nil {
		return nil
	}
	return db.wal.Replay(func(record *walEvent) error {
		switch record.Action {
		case "create_collection":
			if _, ok := db.collections[record.Collection]; !ok {
				db.collections[record.Collection] = &Collection{
					Name:      record.Collection,
					Metric:    record.Metric,
					Dimension: 0,
					Docs:      make(map[string]*Document),
				}
			}
		case "upsert":
			collection, ok := db.collections[record.Collection]
			if !ok {
				return fmt.Errorf("wal replay: collection %q not found", record.Collection)
			}
			if collection.Dimension == 0 {
				collection.Dimension = len(record.Document.Vector)
			}
			collection.Docs[record.Document.ID] = record.Document

			// Rebuild index entry if index exists
			if collection.Index != nil {
				if err := collection.Index.Insert(record.Document.ID, record.Document.Vector); err != nil {
					// Log but don't fail replay — index is best-effort during recovery
				}
			}
		case "delete":
			collection, ok := db.collections[record.Collection]
			if ok {
				delete(collection.Docs, record.ID)
				if collection.MetaIndex != nil {
					collection.MetaIndex.Delete(record.ID)
				}
				if collection.Index != nil {
					collection.Index.Delete(record.ID)
				}
			}
		case "batch_upsert":
			collection, ok := db.collections[record.Collection]
			if !ok {
				return fmt.Errorf("wal replay: collection %q not found", record.Collection)
			}
			for _, doc := range record.Documents {
				if collection.Dimension == 0 {
					collection.Dimension = len(doc.Vector)
				}
				collection.Docs[doc.ID] = doc
				if collection.MetaIndex != nil {
					collection.MetaIndex.Insert(doc.ID, doc.Metadata)
				}
				if collection.Index != nil {
					collection.Index.Insert(doc.ID, doc.Vector)
				}
			}
		case "batch_delete":
			collection, ok := db.collections[record.Collection]
			if ok {
				for _, id := range record.IDs {
					delete(collection.Docs, id)
					if collection.MetaIndex != nil {
						collection.MetaIndex.Delete(id)
					}
					if collection.Index != nil {
						collection.Index.Delete(id)
					}
				}
			}
		case "delete_collection":
			delete(db.collections, record.Collection)
		case "truncate_collection":
			if collection, ok := db.collections[record.Collection]; ok {
				collection.Docs = make(map[string]*Document)
				if collection.MetaIndex != nil {
					collection.MetaIndex.Clear()
				}
				collection.Index = nil
				collection.IndexType = ""
			}
		default:
			return fmt.Errorf("wal replay: unknown action %q", record.Action)
		}
		return nil
	})
}

// ============================================================
// JSON Serialization (for debugging and snapshots)
// ============================================================

func (db *VectorDatabase) MarshalJSON() ([]byte, error) {
	db.mu.RLock()
	defer db.mu.RUnlock()
	return json.Marshal(db.collections)
}
