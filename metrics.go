package flux

import (
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// ============================================================
// Flux Metrics — lightweight Prometheus-compatible metrics
//
// These metrics use atomic counters internally and produce
// Prometheus exposition format text on demand.
// ============================================================

// Metrics collects runtime statistics for the vector database.
type Metrics struct {
	mu sync.RWMutex

	// Counters
	searchTotal    atomic.Int64
	upsertTotal    atomic.Int64
	deleteTotal    atomic.Int64
	snapshotTotal  atomic.Int64

	// Search latency tracking
	searchDurationMs   atomic.Int64
	searchDurationCount atomic.Int64

	// Active state (snapshot at latest stats call)
	lastCollectionCount int
	lastVectorCount     int
	lastIndexCount      int

	startTime time.Time
}

// NewMetrics creates a new Metrics collector.
func NewMetrics() *Metrics {
	return &Metrics{
		startTime: time.Now(),
	}
}

// IncSearch increments the search counter and records latency.
func (m *Metrics) IncSearch(duration time.Duration) {
	m.searchTotal.Add(1)
	m.searchDurationMs.Add(duration.Milliseconds())
	m.searchDurationCount.Add(1)
}

// IncUpsert increments the upsert counter.
func (m *Metrics) IncUpsert() {
	m.upsertTotal.Add(1)
}

// IncDelete increments the delete counter.
func (m *Metrics) IncDelete() {
	m.deleteTotal.Add(1)
}

// IncSnapshot increments the snapshot counter.
func (m *Metrics) IncSnapshot() {
	m.snapshotTotal.Add(1)
}

// UpdateCollectionStats updates the collection-level snapshot metrics.
// Should be called periodically or on demand.
func (m *Metrics) UpdateCollectionStats(collections int, vectors int, indexes int) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.lastCollectionCount = collections
	m.lastVectorCount = vectors
	m.lastIndexCount = indexes
}

// Render produces a Prometheus-exposition-format text output.
func (m *Metrics) Render() string {
	m.mu.RLock()
	defer m.mu.RUnlock()

	var b strings.Builder

	up := time.Since(m.startTime).Seconds()

	// HELP and TYPE comments
	fmt.Fprintf(&b, "# HELP flux_uptime_seconds Flux server uptime\n")
	fmt.Fprintf(&b, "# TYPE flux_uptime_seconds gauge\n")
	fmt.Fprintf(&b, "flux_uptime_seconds %v\n", up)

	fmt.Fprintf(&b, "# HELP flux_search_total Total number of search requests\n")
	fmt.Fprintf(&b, "# TYPE flux_search_total counter\n")
	fmt.Fprintf(&b, "flux_search_total %d\n", m.searchTotal.Load())

	fmt.Fprintf(&b, "# HELP flux_upsert_total Total number of upsert requests\n")
	fmt.Fprintf(&b, "# TYPE flux_upsert_total counter\n")
	fmt.Fprintf(&b, "flux_upsert_total %d\n", m.upsertTotal.Load())

	fmt.Fprintf(&b, "# HELP flux_delete_total Total number of delete requests\n")
	fmt.Fprintf(&b, "# TYPE flux_delete_total counter\n")
	fmt.Fprintf(&b, "flux_delete_total %d\n", m.deleteTotal.Load())

	fmt.Fprintf(&b, "# HELP flux_snapshot_total Total number of snapshots\n")
	fmt.Fprintf(&b, "# TYPE flux_snapshot_total counter\n")
	fmt.Fprintf(&b, "flux_snapshot_total %d\n", m.snapshotTotal.Load())

	// Search latency
	var avgMs float64
	if c := m.searchDurationCount.Load(); c > 0 {
		avgMs = float64(m.searchDurationMs.Load()) / float64(c)
	}
	fmt.Fprintf(&b, "# HELP flux_search_duration_ms Average search duration in milliseconds\n")
	fmt.Fprintf(&b, "# TYPE flux_search_duration_ms gauge\n")
	fmt.Fprintf(&b, "flux_search_duration_ms %.3f\n", avgMs)

	fmt.Fprintf(&b, "# HELP flux_collection_count Number of collections\n")
	fmt.Fprintf(&b, "# TYPE flux_collection_count gauge\n")
	fmt.Fprintf(&b, "flux_collection_count %d\n", m.lastCollectionCount)

	fmt.Fprintf(&b, "# HELP flux_vector_count Total number of vectors across all collections\n")
	fmt.Fprintf(&b, "# TYPE flux_vector_count gauge\n")
	fmt.Fprintf(&b, "flux_vector_count %d\n", m.lastVectorCount)

	fmt.Fprintf(&b, "# HELP flux_index_count Number of collections with an active ANN index\n")
	fmt.Fprintf(&b, "# TYPE flux_index_count gauge\n")
	fmt.Fprintf(&b, "flux_index_count %d\n", m.lastIndexCount)

	return b.String()
}
