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
// IVF Index — Inverted File Index
//
// IVF uses K-means clustering to partition the vector space.
// During search, only the nProbe closest clusters are examined,
// reducing the search space significantly.
// ============================================================

const (
	// DefaultIVFNumCentroids is the default number of centroids (K) for IVF.
	DefaultIVFNumCentroids = 100

	// DefaultIVFNumProbe is the default number of clusters to probe during search.
	DefaultIVFNumProbe = 5

	// DefaultIVFMaxIter is the default maximum K-means iterations.
	DefaultIVFMaxIter = 25
)

// IVFIndex implements the Index interface using an Inverted File structure
// with K-means clustering for approximate nearest neighbor search.
type IVFIndex struct {
	mu         sync.RWMutex
	metric     DistanceMetric
	dimension  int
	centroids  [][]float64   // K centroids
	invertedLists [][]string // invertedLists[centroidIdx] = []docIDs
	nodes      map[string]*ivfNode

	// Parameters
	numCentroids int
	nProbe       int
	maxIter      int

	rng *rand.Rand
}

// ivfNode stores a vector in the IVF index.
type ivfNode struct {
	ID     string    `json:"id"`
	Vector []float64 `json:"vector"`
}

// ivfSnapshot is the serializable state of an IVF index.
type ivfSnapshot struct {
	Metric     DistanceMetric `json:"metric"`
	Dimension  int            `json:"dimension"`
	Centroids  [][]float64    `json:"centroids"`
	NumCentroids int          `json:"num_centroids"`
	NProbe     int            `json:"n_probe"`
	Nodes      map[string]*ivfNode `json:"nodes"`
	// Inverted lists are reconstructed from the nodes during load
}

// IVFOption configures an IVFIndex parameter.
type IVFOption func(*IVFIndex)

// WithIVFNumCentroids sets the number of centroids (K) for the IVF index.
func WithIVFNumCentroids(k int) IVFOption {
	return func(idx *IVFIndex) {
		if k > 0 {
			idx.numCentroids = k
		}
	}
}

// WithIVFNProbe sets the number of clusters to probe during search.
func WithIVFNProbe(n int) IVFOption {
	return func(idx *IVFIndex) {
		if n > 0 {
			idx.nProbe = n
		}
	}
}

// NewIVFIndex creates a new IVF index with the given parameters.
func NewIVFIndex(metric DistanceMetric, dimension int, opts ...IVFOption) *IVFIndex {
	idx := &IVFIndex{
		metric:        metric,
		dimension:     dimension,
		centroids:     nil,
		invertedLists: nil,
		nodes:         make(map[string]*ivfNode),
		numCentroids:  DefaultIVFNumCentroids,
		nProbe:        DefaultIVFNumProbe,
		maxIter:       DefaultIVFMaxIter,
		rng:           rand.New(rand.NewPCG(rand.Uint64(), rand.Uint64())),
	}
	for _, opt := range opts {
		opt(idx)
	}
	return idx
}

// ============================================================
// K-means Clustering
// ============================================================

// buildClusters performs K-means clustering on the indexed vectors.
func (idx *IVFIndex) buildClusters() {
	vectors := make([][]float64, 0, len(idx.nodes))
	for _, node := range idx.nodes {
		vectors = append(vectors, node.Vector)
	}

	n := len(vectors)
	if n == 0 {
		idx.centroids = nil
		idx.invertedLists = nil
		return
	}

	k := idx.numCentroids
	if k > n {
		k = n
	}

	// K-means++ initialization: choose first centroid randomly,
	// then subsequent centroids weighted by distance from nearest centroid.
	centroids := make([][]float64, k)

	// Choose first centroid randomly
	centroids[0] = vectors[idx.rng.IntN(n)]

	for c := 1; c < k; c++ {
		dists := make([]float64, n)
		var totalDist float64
		for i, v := range vectors {
			minD := math.MaxFloat64
			for j := 0; j < c; j++ {
				d := idx.vectorDist(v, centroids[j])
				if d < minD {
					minD = d
				}
			}
			dists[i] = minD * minD // squared distance for probability
			totalDist += dists[i]
		}

		// Choose next centroid weighted by distance
		t := idx.rng.Float64() * totalDist
		var cum float64
		chosen := 0
		for i, d := range dists {
			cum += d
			if cum >= t {
				chosen = i
				break
			}
		}
		centroids[c] = vectors[chosen]
	}

	// Iterate K-means
	assignments := make([]int, n)
	for iter := 0; iter < idx.maxIter; iter++ {
		changed := false

		// Assignment step
		for i, v := range vectors {
			bestC := 0
			bestD := math.MaxFloat64
			for c := 0; c < k; c++ {
				d := idx.vectorDist(v, centroids[c])
				if d < bestD {
					bestD = d
					bestC = c
				}
			}
			if assignments[i] != bestC {
				assignments[i] = bestC
				changed = true
			}
		}

		if !changed {
			break
		}

		// Update step
		for c := 0; c < k; c++ {
			var sum []float64
			count := 0
			for i, v := range vectors {
				if assignments[i] == c {
					if sum == nil {
						sum = make([]float64, len(v))
					}
					for d := range v {
						sum[d] += v[d]
					}
					count++
				}
			}
			if count > 0 {
				for d := range sum {
					centroids[c][d] = sum[d] / float64(count)
				}
			}
		}
	}

	idx.centroids = centroids
}

// vectorDist computes the Euclidean distance between two vectors.
// Used for K-means clustering.
func (idx *IVFIndex) vectorDist(a, b []float64) float64 {
	var sum float64
	for i := range a {
		delta := a[i] - b[i]
		sum += delta * delta
	}
	return math.Sqrt(sum)
}

// ============================================================
// Index Interface Implementation
// ============================================================

// Insert adds a vector to the IVF index and assigns it to the nearest centroid.
func (idx *IVFIndex) Insert(docID string, vector []float64) error {
	if len(vector) != idx.dimension {
		return fmt.Errorf("dimension mismatch: expected %d, got %d", idx.dimension, len(vector))
	}
	if docID == "" {
		return fmt.Errorf("docID is required")
	}

	idx.mu.Lock()
	defer idx.mu.Unlock()

	// Upsert: remove old entry if exists
	if old, exists := idx.nodes[docID]; exists {
		// Remove from inverted list
		if idx.centroids != nil {
			bestC := idx.findNearestCentroid(old.Vector)
			if bestC >= 0 && bestC < len(idx.invertedLists) {
				list := idx.invertedLists[bestC]
				for i, id := range list {
					if id == docID {
						idx.invertedLists[bestC] = append(list[:i], list[i+1:]...)
						break
					}
				}
			}
		}
	}

	node := &ivfNode{ID: docID, Vector: vector}
	idx.nodes[docID] = node

	// Add to inverted list if centroids exist
	if idx.centroids != nil {
		bestC := idx.findNearestCentroid(vector)
		if bestC >= 0 && bestC < len(idx.invertedLists) {
			idx.invertedLists[bestC] = append(idx.invertedLists[bestC], docID)
		}
	}

	return nil
}

// Delete lazily marks a vector as deleted by removing it from the node map.
func (idx *IVFIndex) Delete(docID string) {
	idx.mu.Lock()
	defer idx.mu.Unlock()

	delete(idx.nodes, docID)
	// Note: we leave the entry in the inverted list; it will be skipped
	// during search since the node is no longer in the map.
}

// SearchInternal returns the top-k nearest neighbors using IVF.
func (idx *IVFIndex) SearchInternal(query []float64, k int) ([]SearchResult, error) {
	if len(query) != idx.dimension {
		return nil, fmt.Errorf("query dimension %d does not match index dimension %d",
			len(query), idx.dimension)
	}

	idx.mu.RLock()
	defer idx.mu.RUnlock()

	if len(idx.nodes) == 0 || idx.centroids == nil {
		return nil, nil
	}

	// Step 1: Find the closest centroids
	centroidDists := make([]struct {
		idx  int
		dist float64
	}, len(idx.centroids))
	for c := range idx.centroids {
		d := idx.vectorDist(query, idx.centroids[c])
		centroidDists[c] = struct {
			idx  int
			dist float64
		}{c, d}
	}
	sort.Slice(centroidDists, func(i, j int) bool {
		return centroidDists[i].dist < centroidDists[j].dist
	})

	// Step 2: Search the inverted lists of the closest nProbe centroids
	nProbe := idx.nProbe
	if nProbe > len(centroidDists) {
		nProbe = len(centroidDists)
	}

	visited := make(map[string]bool)
	candidates := make([]distCandidate, 0)

	for p := 0; p < nProbe; p++ {
		c := centroidDists[p].idx
		if c >= len(idx.invertedLists) {
			continue
		}
		for _, docID := range idx.invertedLists[c] {
			if visited[docID] {
				continue
			}
			visited[docID] = true

			node, ok := idx.nodes[docID]
			if !ok {
				continue
			}

			score := computeScore(idx.metric, query, node.Vector)
			candidates = append(candidates, distCandidate{DocID: docID, Dist: -score})
		}
	}

	// Step 3: Sort by similarity (higher score = better)
	sort.Slice(candidates, func(i, j int) bool {
		return candidates[i].Dist < candidates[j].Dist
	})

	results := make([]SearchResult, 0, min(k, len(candidates)))
	for i := 0; i < len(candidates) && len(results) < k; i++ {
		// Dist stores -score, so negate back
		results = append(results, SearchResult{
			ID:    candidates[i].DocID,
			Score: -candidates[i].Dist,
		})
	}

	return results, nil
}

// SearchInternalWithFilter implements FilteredIndex.
// Restricts search to documents in the candidates set.
func (idx *IVFIndex) SearchInternalWithFilter(query []float64, k int, candidates map[string]bool) ([]SearchResult, error) {
	if len(query) != idx.dimension {
		return nil, fmt.Errorf("query dimension %d does not match index dimension %d",
			len(query), idx.dimension)
	}

	idx.mu.RLock()
	defer idx.mu.RUnlock()

	if len(idx.nodes) == 0 || idx.centroids == nil || len(candidates) == 0 {
		return nil, nil
	}

	// Step 1: Find the closest centroids
	centroidDists := make([]struct {
		idx  int
		dist float64
	}, len(idx.centroids))
	for c := range idx.centroids {
		d := idx.vectorDist(query, idx.centroids[c])
		centroidDists[c] = struct {
			idx  int
			dist float64
		}{c, d}
	}
	sort.Slice(centroidDists, func(i, j int) bool {
		return centroidDists[i].dist < centroidDists[j].dist
	})

	// Step 2: Search inverted lists, filtered by candidates
	nProbe := idx.nProbe
	if nProbe > len(centroidDists) {
		nProbe = len(centroidDists)
	}

	visited := make(map[string]bool)
	filtered := make([]distCandidate, 0)

	for p := 0; p < nProbe; p++ {
		c := centroidDists[p].idx
		if c >= len(idx.invertedLists) {
			continue
		}
		for _, docID := range idx.invertedLists[c] {
			if visited[docID] {
				continue
			}
			visited[docID] = true

			if !candidates[docID] {
				continue // skip if not in pre-filtered candidate set
			}

			node, ok := idx.nodes[docID]
			if !ok {
				continue
			}
			score := computeScore(idx.metric, query, node.Vector)
			filtered = append(filtered, distCandidate{DocID: docID, Dist: -score})
		}
	}

	// Step 3: Sort and select top-k
	sort.Slice(filtered, func(i, j int) bool {
		return filtered[i].Dist < filtered[j].Dist
	})

	results := make([]SearchResult, 0, min(k, len(filtered)))
	for i := 0; i < len(filtered) && len(results) < k; i++ {
		results = append(results, SearchResult{
			ID:    filtered[i].DocID,
			Score: -filtered[i].Dist,
		})
	}

	return results, nil
}

// findNearestCentroid finds the index of the centroid closest to the given vector.
func (idx *IVFIndex) findNearestCentroid(vector []float64) int {
	if len(idx.centroids) == 0 {
		return -1
	}
	bestC := 0
	bestD := math.MaxFloat64
	for c := range idx.centroids {
		d := idx.vectorDist(vector, idx.centroids[c])
		if d < bestD {
			bestD = d
			bestC = c
		}
	}
	return bestC
}

// Len returns the number of indexed vectors.
func (idx *IVFIndex) Len() int {
	idx.mu.RLock()
	defer idx.mu.RUnlock()
	return len(idx.nodes)
}

// Build trains the IVF index by running K-means clustering on all
// currently inserted vectors. This should be called after inserting
// a sufficient number of vectors.
func (idx *IVFIndex) Build() {
	idx.mu.Lock()
	defer idx.mu.Unlock()

	if len(idx.nodes) == 0 {
		return
	}

	// Run K-means
	idx.buildClusters()

	// Build inverted lists
	k := len(idx.centroids)
	idx.invertedLists = make([][]string, k)
	for _, node := range idx.nodes {
		c := idx.findNearestCentroid(node.Vector)
		if c >= 0 && c < k {
			idx.invertedLists[c] = append(idx.invertedLists[c], node.ID)
		}
	}
}

// ============================================================
// Persistence
// ============================================================

// Save persists the IVF index state to a writer.
func (idx *IVFIndex) Save(w io.Writer) error {
	idx.mu.RLock()
	defer idx.mu.RUnlock()

	snap := ivfSnapshot{
		Metric:       idx.metric,
		Dimension:    idx.dimension,
		Centroids:    idx.centroids,
		NumCentroids: idx.numCentroids,
		NProbe:       idx.nProbe,
		Nodes:        idx.nodes,
	}

	enc := json.NewEncoder(w)
	return enc.Encode(snap)
}

// Load restores the IVF index state from a reader.
func (idx *IVFIndex) Load(r io.Reader) error {
	var snap ivfSnapshot
	dec := json.NewDecoder(r)
	if err := dec.Decode(&snap); err != nil {
		return fmt.Errorf("ivf load: %w", err)
	}

	idx.mu.Lock()
	defer idx.mu.Unlock()

	idx.metric = snap.Metric
	idx.dimension = snap.Dimension
	idx.centroids = snap.Centroids
	idx.numCentroids = snap.NumCentroids
	idx.nProbe = snap.NProbe
	idx.nodes = snap.Nodes
	idx.rng = rand.New(rand.NewPCG(rand.Uint64(), rand.Uint64()))

	// Rebuild inverted lists from centroids + nodes
	if idx.centroids != nil {
		k := len(idx.centroids)
		idx.invertedLists = make([][]string, k)
		for _, node := range idx.nodes {
			c := idx.findNearestCentroid(node.Vector)
			if c >= 0 && c < k {
				idx.invertedLists[c] = append(idx.invertedLists[c], node.ID)
			}
		}
	}

	return nil
}
