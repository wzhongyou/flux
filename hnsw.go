package flux

import (
	"encoding/json"
	"fmt"
	"io"
	"math"
	"math/rand/v2"
	"sort"
	"sync"
)

// ============================================================
// Constants and Defaults
// ============================================================

const (
	// DefaultM is the default number of bidirectional connections per node
	// for layers > 0. Higher M means better recall but more memory and
	// slower insertion.
	DefaultM = 16

	// DefaultMmax0 is the default max connections per node for layer 0.
	// Set to 2 * M as recommended by the HNSW paper.
	DefaultMmax0 = 32

	// DefaultEFConstruction is the default ef parameter used during index
	// construction. Higher values yield better graph quality at the cost
	// of slower insertion.
	DefaultEFConstruction = 200

	// DefaultEFSearch is the default ef parameter used during search.
	// Higher values improve recall at the cost of latency.
	DefaultEFSearch = 100
)

// ============================================================
// HNSW Types
// ============================================================

// hnswNode represents one vertex in the multi-layer HNSW graph.
type hnswNode struct {
	ID          string      `json:"id"`
	Vector      []float64   `json:"vector"`
	Level       int         `json:"level"`
	Connections [][]string  `json:"connections"` // connections[layer] = []neighborDocIDs
}

// HNSWIndex implements a Hierarchical Navigable Small World graph for
// approximate nearest neighbor search. It implements the Index interface.
type HNSWIndex struct {
	mu         sync.RWMutex
	metric     DistanceMetric
	dimension  int
	nodes      map[string]*hnswNode // docID -> node
	deleted    map[string]bool      // lazily deleted docIDs
	entryPoint string               // docID of the current entry point
	maxLevel   int                  // highest layer in the graph

	// HNSW parameters (set via constructor options)
	m              int     // M: max connections per node for layers > 0
	mmax0          int     // Mmax0: max connections per node for layer 0
	efConstruction int     // ef parameter during construction
	efSearch       int     // ef parameter during query
	mL             float64 // 1/ln(M), controls level distribution

	// Per-instance RNG avoids global lock contention
	rng *rand.Rand
}

// distCandidate is an internal helper for tracking distances during search.
type distCandidate struct {
	DocID string
	Dist  float64
}

// HNSWOption configures an HNSWIndex parameter.
type HNSWOption func(*HNSWIndex)

// WithM sets the M parameter (max connections per node for layers > 0).
// It automatically updates Mmax0 to 2*M and mL to 1/ln(M).
func WithM(m int) HNSWOption {
	return func(idx *HNSWIndex) {
		if m > 0 {
			idx.m = m
			idx.mmax0 = 2 * m
			idx.mL = 1.0 / math.Log(float64(m))
		}
	}
}

// WithEFConstruction sets the ef parameter used during index construction.
func WithEFConstruction(ef int) HNSWOption {
	return func(idx *HNSWIndex) {
		if ef > 0 {
			idx.efConstruction = ef
		}
	}
}

// WithEFSearch sets the ef parameter used during search.
func WithEFSearch(ef int) HNSWOption {
	return func(idx *HNSWIndex) {
		if ef > 0 {
			idx.efSearch = ef
		}
	}
}

// NewHNSWIndex creates a new HNSW index with the given metric, dimension,
// and optional parameter overrides.
func NewHNSWIndex(metric DistanceMetric, dimension int, opts ...HNSWOption) *HNSWIndex {
	idx := &HNSWIndex{
		metric:         metric,
		dimension:      dimension,
		nodes:          make(map[string]*hnswNode),
		deleted:        make(map[string]bool),
		m:              DefaultM,
		mmax0:          DefaultMmax0,
		efConstruction: DefaultEFConstruction,
		efSearch:       DefaultEFSearch,
		mL:             1.0 / math.Log(float64(DefaultM)),
		entryPoint:     "",
		maxLevel:       -1,
		rng:            rand.New(rand.NewPCG(rand.Uint64(), rand.Uint64())),
	}
	for _, opt := range opts {
		opt(idx)
	}
	return idx
}

// ============================================================
// Level Generation
// ============================================================

// generateLevel returns a random level for a new node, using the standard
// HNSW distribution: level = floor(-ln(uniform(0,1)) * mL).
func (idx *HNSWIndex) generateLevel() int {
	r := idx.rng.Float64()
	if r == 0 {
		r = math.SmallestNonzeroFloat64
	}
	level := int(math.Floor(-math.Log(r) * idx.mL))
	if level < 0 {
		level = 0
	}
	// Cap to prevent degenerate cases
	if level > 64 {
		level = 64
	}
	return level
}

// ============================================================
// Distance Computation
// ============================================================

// distance returns a "lower is better" distance between two vectors,
// derived by negating computeScore (which returns "higher is better").
func (idx *HNSWIndex) distance(a, b []float64) float64 {
	return -computeScore(idx.metric, a, b)
}

// ============================================================
// Core Search Layer (Algorithm 2 from the HNSW paper)
// ============================================================

// searchLayer finds the ef nearest neighbors to query at the given layer.
// Returns a map of docID -> distance for the closest ef candidates.
func (idx *HNSWIndex) searchLayer(query []float64, entryPoints []string, ef, layer int) (map[string]float64, error) {
	visited := make(map[string]bool)
	candidates := make(map[string]float64) // frontier to explore
	results := make(map[string]float64)    // top ef results

	// Initialize with entry points
	for _, ep := range entryPoints {
		if visited[ep] || idx.deleted[ep] {
			continue
		}
		visited[ep] = true
		node, ok := idx.nodes[ep]
		if !ok {
			continue
		}
		d := idx.distance(query, node.Vector)
		candidates[ep] = d
		results[ep] = d
	}

	for len(candidates) > 0 {
		// Find closest candidate (minimum distance)
		cID, cDist := minEntry(candidates)
		delete(candidates, cID)

		// Find farthest result (maximum distance)
		fID, fDist := maxEntry(results)

		// Early termination: if closest candidate is farther than
		// the farthest result, all remaining candidates are worse.
		if cDist > fDist {
			break
		}

		node, ok := idx.nodes[cID]
		if !ok {
			continue
		}

		// Explore neighbors of the current candidate at this layer
		if layer >= len(node.Connections) {
			continue
		}
		for _, neighborID := range node.Connections[layer] {
			if visited[neighborID] || idx.deleted[neighborID] {
				continue
			}
			visited[neighborID] = true

			neighbor, ok := idx.nodes[neighborID]
			if !ok {
				continue
			}
			d := idx.distance(query, neighbor.Vector)

			if _, inResults := results[neighborID]; !inResults {
				fID, fDist = maxEntry(results)
				if len(results) < ef || d < fDist {
					candidates[neighborID] = d
					results[neighborID] = d
					// Prune if over capacity
					if len(results) > ef {
						delete(results, fID)
					}
				}
			}
		}
	}

	return results, nil
}

// greedySearchLayer finds the single closest node at a given layer
// by following the steepest gradient descent. Used for traversing
// upper layers where ef=1 is sufficient.
func (idx *HNSWIndex) greedySearchLayer(query []float64, entryPoint string, layer int) string {
	currID := entryPoint
	for {
		changed := false
		currNode := idx.nodes[currID]
		if currNode == nil {
			break
		}
		currDist := idx.distance(query, currNode.Vector)

		if layer >= len(currNode.Connections) {
			break
		}
		for _, neighborID := range currNode.Connections[layer] {
			if idx.deleted[neighborID] {
				continue
			}
			neighbor, ok := idx.nodes[neighborID]
			if !ok {
				continue
			}
			d := idx.distance(query, neighbor.Vector)
			if d < currDist {
				currDist = d
				currID = neighborID
				changed = true
			}
		}

		if !changed {
			break
		}
	}
	return currID
}

// ============================================================
// Neighbor Selection
// ============================================================

// selectNeighborsSimple picks the top-k closest entries by distance.
// Implements the simple SELECT-NEIGHBORS from the paper (no heuristic).
func (idx *HNSWIndex) selectNeighborsSimple(candidates map[string]float64, k int) []string {
	if len(candidates) <= k {
		ids := make([]string, 0, len(candidates))
		for id := range candidates {
			ids = append(ids, id)
		}
		return ids
	}

	pairs := make([]distCandidate, 0, len(candidates))
	for id, d := range candidates {
		pairs = append(pairs, distCandidate{DocID: id, Dist: d})
	}
	sort.Slice(pairs, func(i, j int) bool {
		return pairs[i].Dist < pairs[j].Dist
	})

	ids := make([]string, k)
	for i := 0; i < k; i++ {
		ids[i] = pairs[i].DocID
	}
	return ids
}

// ============================================================
// Connection Management
// ============================================================

// addConnections adds bidirectional links between nodeID and neighbors at the given layer.
// It handles connection capacity limits at both ends.
func (idx *HNSWIndex) addConnections(nodeID string, neighbors []string, layer int) {
	node := idx.nodes[nodeID]
	if node == nil {
		return
	}

	maxConn := idx.mmax0
	if layer > 0 {
		maxConn = idx.m
	}

	// Ensure the node has a connections slice for this layer
	for len(node.Connections) <= layer {
		node.Connections = append(node.Connections, nil)
	}

	// Add forward connections
	for _, neighborID := range neighbors {
		if neighborID == nodeID {
			continue
		}
		node.Connections[layer] = append(node.Connections[layer], neighborID)
	}

	// Shrink node's connections if over capacity
	if len(node.Connections[layer]) > maxConn {
		idx.shrinkConnections(nodeID, layer, maxConn)
	}

	// Add reverse connections (neighbor -> nodeID)
	for _, neighborID := range neighbors {
		if neighborID == nodeID {
			continue
		}
		neighbor, ok := idx.nodes[neighborID]
		if !ok {
			continue
		}

		for len(neighbor.Connections) <= layer {
			neighbor.Connections = append(neighbor.Connections, nil)
		}

		neighbor.Connections[layer] = append(neighbor.Connections[layer], nodeID)

		if len(neighbor.Connections[layer]) > maxConn {
			idx.shrinkConnections(neighborID, layer, maxConn)
		}
	}
}

// shrinkConnections trims nodeID's connections at the given layer to at most maxConn,
// keeping the closest connections by distance to the node's own vector.
func (idx *HNSWIndex) shrinkConnections(nodeID string, layer int, maxConn int) {
	node := idx.nodes[nodeID]
	if node == nil {
		return
	}
	conns := node.Connections[layer]
	if len(conns) <= maxConn {
		return
	}

	pairs := make([]distCandidate, 0, len(conns))
	for _, neighborID := range conns {
		neighbor, ok := idx.nodes[neighborID]
		if !ok || idx.deleted[neighborID] {
			continue
		}
		d := idx.distance(node.Vector, neighbor.Vector)
		pairs = append(pairs, distCandidate{DocID: neighborID, Dist: d})
	}
	sort.Slice(pairs, func(i, j int) bool {
		return pairs[i].Dist < pairs[j].Dist
	})

	result := make([]string, 0, maxConn)
	for i := 0; i < len(pairs) && i < maxConn; i++ {
		result = append(result, pairs[i].DocID)
	}
	node.Connections[layer] = result
}

// ============================================================
// Index Interface Implementation: Insert
// ============================================================

// Insert adds a vector to the HNSW graph. If the docID already exists,
// it is deleted and re-inserted (upsert semantics).
func (idx *HNSWIndex) Insert(docID string, vector []float64) error {
	if len(vector) != idx.dimension {
		return fmt.Errorf("dimension mismatch: expected %d, got %d", idx.dimension, len(vector))
	}
	if docID == "" {
		return fmt.Errorf("docID is required")
	}

	idx.mu.Lock()
	defer idx.mu.Unlock()

	// Handle update: delete existing node
	if _, exists := idx.nodes[docID]; exists {
		idx.internalDelete(docID)
	}

	level := idx.generateLevel()
	node := &hnswNode{
		ID:          docID,
		Vector:      vector,
		Level:       level,
		Connections: make([][]string, level+1),
	}
	for l := 0; l <= level; l++ {
		maxConn := idx.mmax0
		if l > 0 {
			maxConn = idx.m
		}
		node.Connections[l] = make([]string, 0, maxConn)
	}
	idx.nodes[docID] = node
	delete(idx.deleted, docID)

	// First node: becomes the entry point
	if idx.entryPoint == "" || idx.maxLevel < 0 {
		idx.entryPoint = docID
		idx.maxLevel = level
		return nil
	}

	// Step 1: Traverse upper layers greedily to find the entry point
	// for the level where this node will be inserted.
	currEntry := idx.entryPoint
	for lc := idx.maxLevel; lc > level; lc-- {
		currEntry = idx.greedySearchLayer(vector, currEntry, lc)
	}

	// Step 2: Search and connect at each layer from min(level, maxLevel) down to 0
	entrySet := []string{currEntry}
	for lc := min(level, idx.maxLevel); lc >= 0; lc-- {
		candidates, err := idx.searchLayer(vector, entrySet, idx.efConstruction, lc)
		if err != nil {
			return err
		}
		neighbors := idx.selectNeighborsSimple(candidates, idx.m)
		idx.addConnections(docID, neighbors, lc)

		// Use the closest candidate as the entry point for the next lower layer
		if len(candidates) > 0 {
			closest, _ := minEntry(candidates)
			entrySet = []string{closest}
		}
	}

	// Step 3: Update global entry point if this node is at a higher level
	if level > idx.maxLevel {
		idx.maxLevel = level
		idx.entryPoint = docID
	}

	return nil
}

// ============================================================
// Index Interface Implementation: Delete
// ============================================================

// Delete lazily marks a docID as removed from the index.
// The node remains in the graph but is skipped during all operations.
func (idx *HNSWIndex) Delete(docID string) {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	idx.internalDelete(docID)
}

func (idx *HNSWIndex) internalDelete(docID string) {
	if _, exists := idx.nodes[docID]; !exists {
		return
	}
	idx.deleted[docID] = true
}

// ============================================================
// Index Interface Implementation: SearchInternal
// ============================================================

// SearchInternal returns the top-k nearest neighbors as SearchResult entries
// with ID and Score populated. Document and Metadata are NOT populated
// — the caller (Collection.Search) resolves these.
func (idx *HNSWIndex) SearchInternal(query []float64, k int) ([]SearchResult, error) {
	if len(query) != idx.dimension {
		return nil, fmt.Errorf("query dimension %d does not match index dimension %d",
			len(query), idx.dimension)
	}

	idx.mu.RLock()
	defer idx.mu.RUnlock()

	if len(idx.nodes) == 0 || idx.entryPoint == "" {
		return nil, nil
	}

	// Phase 1: Top-layer greedy traversal to find a good entry point for layer 0
	currEntry := idx.entryPoint
	for lc := idx.maxLevel; lc > 0; lc-- {
		currEntry = idx.greedySearchLayer(query, currEntry, lc)
	}

	// Phase 2: Bottom-layer search with efSearch
	candidates, err := idx.searchLayer(query, []string{currEntry}, idx.efSearch, 0)
	if err != nil {
		return nil, err
	}

	// Phase 3: Select top-k and convert back to similarity scores
	pairs := make([]distCandidate, 0, len(candidates))
	for id, d := range candidates {
		if idx.deleted[id] {
			continue
		}
		pairs = append(pairs, distCandidate{DocID: id, Dist: d})
	}
	sort.Slice(pairs, func(i, j int) bool {
		return pairs[i].Dist < pairs[j].Dist
	})

	results := make([]SearchResult, 0, min(k, len(pairs)))
	for i := 0; i < len(pairs) && len(results) < k; i++ {
		// distance() returns -computeScore, so negate to get original similarity score
		score := -pairs[i].Dist
		results = append(results, SearchResult{
			ID:    pairs[i].DocID,
			Score: score,
		})
	}

	return results, nil
}

// SearchInternalWithFilter implements FilteredIndex.
// It restricts search to only consider documents in the candidates set.
func (idx *HNSWIndex) SearchInternalWithFilter(query []float64, k int, candidates map[string]bool) ([]SearchResult, error) {
	if len(query) != idx.dimension {
		return nil, fmt.Errorf("query dimension %d does not match index dimension %d",
			len(query), idx.dimension)
	}

	idx.mu.RLock()
	defer idx.mu.RUnlock()

	if len(idx.nodes) == 0 || idx.entryPoint == "" || len(candidates) == 0 {
		return nil, nil
	}

	// Phase 1: Top-layer greedy traversal
	currEntry := idx.entryPoint
	for lc := idx.maxLevel; lc > 0; lc-- {
		currEntry = idx.greedySearchLayer(query, currEntry, lc)
	}

	// Phase 2: Bottom-layer search with efSearch, filtered by candidates
	// We use searchLayer to get candidates then apply the candidate set filter
	candidatesRaw, err := idx.searchLayer(query, []string{currEntry}, idx.efSearch, 0)
	if err != nil {
		return nil, err
	}

	// Phase 3: Filter by candidate set, then select top-k
	pairs := make([]distCandidate, 0, len(candidatesRaw))
	for id, d := range candidatesRaw {
		if idx.deleted[id] {
			continue
		}
		if !candidates[id] {
			continue // not in the pre-filtered candidate set
		}
		pairs = append(pairs, distCandidate{DocID: id, Dist: d})
	}
	sort.Slice(pairs, func(i, j int) bool {
		return pairs[i].Dist < pairs[j].Dist
	})

	results := make([]SearchResult, 0, min(k, len(pairs)))
	for i := 0; i < len(pairs) && len(results) < k; i++ {
		score := -pairs[i].Dist
		results = append(results, SearchResult{
			ID:    pairs[i].DocID,
			Score: score,
		})
	}

	return results, nil
}

// Len returns the number of active (non-deleted) indexed vectors.
func (idx *HNSWIndex) Len() int {
	idx.mu.RLock()
	defer idx.mu.RUnlock()
	count := len(idx.nodes) - len(idx.deleted)
	if count < 0 {
		return 0
	}
	return count
}

// ============================================================
// Index Interface Implementation: Save / Load
// ============================================================

// hnswSnapshot is the serializable representation of the HNSW index state.
type hnswSnapshot struct {
	Metric     DistanceMetric        `json:"metric"`
	Dimension  int                   `json:"dimension"`
	M          int                   `json:"m"`
	Mmax0      int                   `json:"mmax0"`
	EFConst    int                   `json:"ef_construction"`
	EFSearch   int                   `json:"ef_search"`
	EntryPoint string                `json:"entry_point"`
	MaxLevel   int                   `json:"max_level"`
	Nodes      map[string]*hnswNode  `json:"nodes"`
	DeletedIDs []string              `json:"deleted_ids,omitempty"`
}

// Save persists the full HNSW index state to a writer in JSON format.
func (idx *HNSWIndex) Save(w io.Writer) error {
	idx.mu.RLock()
	defer idx.mu.RUnlock()

	deletedIDs := make([]string, 0, len(idx.deleted))
	for id := range idx.deleted {
		deletedIDs = append(deletedIDs, id)
	}

	snap := hnswSnapshot{
		Metric:     idx.metric,
		Dimension:  idx.dimension,
		M:          idx.m,
		Mmax0:      idx.mmax0,
		EFConst:    idx.efConstruction,
		EFSearch:   idx.efSearch,
		EntryPoint: idx.entryPoint,
		MaxLevel:   idx.maxLevel,
		Nodes:      idx.nodes,
		DeletedIDs: deletedIDs,
	}

	enc := json.NewEncoder(w)
	return enc.Encode(snap)
}

// Load restores the full HNSW index state from a JSON reader.
func (idx *HNSWIndex) Load(r io.Reader) error {
	var snap hnswSnapshot
	dec := json.NewDecoder(r)
	if err := dec.Decode(&snap); err != nil {
		return fmt.Errorf("hnsw load: %w", err)
	}

	idx.mu.Lock()
	defer idx.mu.Unlock()

	idx.metric = snap.Metric
	idx.dimension = snap.Dimension
	idx.m = snap.M
	idx.mmax0 = snap.Mmax0
	idx.efConstruction = snap.EFConst
	idx.efSearch = snap.EFSearch
	idx.entryPoint = snap.EntryPoint
	idx.maxLevel = snap.MaxLevel
	idx.nodes = snap.Nodes
	idx.deleted = make(map[string]bool, len(snap.DeletedIDs))
	for _, id := range snap.DeletedIDs {
		idx.deleted[id] = true
	}
	idx.mL = 1.0 / math.Log(float64(idx.m))
	idx.rng = rand.New(rand.NewPCG(rand.Uint64(), rand.Uint64()))

	return nil
}

// ============================================================
// Helpers
// ============================================================

// minEntry finds the entry with the minimum distance in a map.
func minEntry(m map[string]float64) (string, float64) {
	var bestID string
	bestDist := math.MaxFloat64
	for id, d := range m {
		if d < bestDist {
			bestDist = d
			bestID = id
		}
	}
	return bestID, bestDist
}

// maxEntry finds the entry with the maximum distance in a map.
func maxEntry(m map[string]float64) (string, float64) {
	var bestID string
	bestDist := -math.MaxFloat64
	for id, d := range m {
		if d > bestDist {
			bestDist = d
			bestID = id
		}
	}
	return bestID, bestDist
}
