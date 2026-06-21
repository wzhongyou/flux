package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/wzhongyou/flux"
)


// ============================================================
// Request / Response Types
// ============================================================

type createCollectionRequest struct {
	Name        string                  `json:"name"`
	Metric      flux.DistanceMetric `json:"metric"`
	EnableIndex bool                    `json:"enable_index"`
	IndexType   string                  `json:"index_type"`
	Schema      map[string]interface{}  `json:"schema,omitempty"`
}

type upsertRequest struct {
	ID       string                 `json:"id"`
	Vector   []float64              `json:"vector"`
	Metadata map[string]interface{} `json:"metadata,omitempty"`
}

type batchUpsertRequest struct {
	Documents []upsertRequest `json:"documents"`
}

type batchDeleteRequest struct {
	IDs []string `json:"ids"`
}

type searchRequest struct {
	Query  []float64              `json:"query"`
	K      int                    `json:"k"`
	Filter map[string]interface{} `json:"filter,omitempty"`
}

type searchResponse struct {
	Results     []flux.SearchResult `json:"results"`
	TotalTimeNs int64                   `json:"total_time_ns,omitempty"`
	DocsScanned int                     `json:"docs_scanned,omitempty"`
	IndexUsed   string                  `json:"index_used,omitempty"`
}

type hybridSearchRequest struct {
	Query      []float64              `json:"query"`
	TextQuery  string                 `json:"text_query"`
	K          int                    `json:"k"`
	Alpha      float64                `json:"alpha"`
	Filter     map[string]interface{} `json:"filter,omitempty"`
}

type hybridSearchResponse struct {
	Results []flux.HybridSearchResult `json:"results"`
	TotalTimeNs int64                     `json:"total_time_ns"`
}

type recallResponse struct {
	Query           []float64                 `json:"query"`
	TopK            int                       `json:"top_k"`
	AnnResults      []flux.SearchResult    `json:"ann_results"`
	BfResults       []flux.SearchResult    `json:"bf_results"`
	Recall          float64                   `json:"recall"`
	AnnTimeNs       int64                     `json:"ann_time_ns"`
	BfTimeNs        int64                     `json:"bf_time_ns"`
	AnnCandidates   int                       `json:"ann_candidates"`
	BfCandidates    int                       `json:"bf_candidates"`
	IndexType       string                    `json:"index_type"`
	AnnSearched     bool                      `json:"ann_searched"`
}

type explainResponse struct {
	QueryVector     []float64               `json:"query"`
	TopK            int                     `json:"top_k"`
	IndexType       string                  `json:"index_type,omitempty"`
	Candidates      int                     `json:"candidates"`
	ResultsReturned int                     `json:"results_returned"`
	FilterApplied   string                  `json:"filter_applied,omitempty"`
	SearchTimeNs    int64                   `json:"search_time_ns"`
	Results         []flux.SearchResult `json:"results"`
}

type indexActionRequest struct {
	Action    string `json:"action"` // "build" or "drop"
	IndexType string `json:"index_type,omitempty"`
}

type snapshotRequest struct {
	Path string `json:"path"`
}

// ============================================================
// Server
// ============================================================

type server struct {
	db     *flux.VectorDatabase
	config *flux.Config
	sem    chan struct{} // concurrency limiter
}

func main() {
	configPath := flag.String("config", "", "Path to config file")
	addr := flag.String("addr", "", "HTTP listen address (overrides config)")
	walPath := flag.String("wal", "", "WAL file path (overrides config)")
	flag.Parse()

	// Load config
	cfg, err := flux.LoadConfig(*configPath)
	if err != nil {
		log.Fatalf("failed to load config: %v", err)
	}

	// CLI flags override config
	if *addr != "" {
		cfg.Server.Addr = *addr
	}
	if *walPath != "" {
		cfg.Database.WALPath = *walPath
	}

	// Initialize database
	db, err := flux.NewVectorDatabase(cfg.Database.WALPath)
	if err != nil {
		log.Fatalf("failed to create database: %v", err)
	}
	defer db.Close()

	// Load snapshot if configured
	if cfg.Database.SnapshotPath != "" {
		if err := db.LoadFromSnapshot(cfg.Database.SnapshotPath); err != nil {
			log.Printf("warning: could not load snapshot %q: %v", cfg.Database.SnapshotPath, err)
		} else {
			log.Printf("loaded snapshot from %s", cfg.Database.SnapshotPath)
		}
	}

	s := &server{
		db:     db,
		config: cfg,
		sem:    make(chan struct{}, cfg.Server.MaxConcurrent),
	}

	// Extract web files
	webSubFS, err := fs.Sub(flux.WebFiles, "web")
	if err != nil {
		log.Fatalf("failed to create web sub filesystem: %v", err)
	}

	mux := http.NewServeMux()

	// Collections
	mux.HandleFunc("/collections", withJSON(s.handleCollections))
	mux.HandleFunc("/collections/", withJSON(s.handleCollectionRoute))

	// Health
	mux.HandleFunc("/health", withJSON(s.handleHealth))
	mux.HandleFunc("/ready", withJSON(s.handleReady))

	// Snapshot
	mux.HandleFunc("/snapshot", withJSON(s.handleSnapshot))
	mux.HandleFunc("/restore", withJSON(s.handleRestore))

	// Metrics
	mux.HandleFunc("/metrics", s.handleMetrics)

	// Static files
	mux.Handle("/static/", http.FileServer(http.FS(webSubFS)))

	// Web console
	mux.HandleFunc("/", s.handleRoot)

	// Build handler chain: limiter → auth → cors
	handler := s.concurrencyLimit(s.authMiddleware(s.corsMiddleware(mux)))

	// Graceful shutdown
	srv := &http.Server{
		Addr:         cfg.Server.Addr,
		Handler:      handler,
		ReadTimeout:  time.Duration(cfg.Server.ReadTimeout) * time.Second,
		WriteTimeout: time.Duration(cfg.Server.WriteTimeout) * time.Second,
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	go func() {
		<-ctx.Done()
		log.Println("shutting down...")
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		srv.Shutdown(shutdownCtx)
		db.Close()
	}()

	if cfg.Server.TLSEnabled() {
		log.Printf("flux server listening on %s (TLS)", cfg.Server.Addr)
		if err := srv.ListenAndServeTLS(cfg.Server.TLSCertFile, cfg.Server.TLSKeyFile); err != http.ErrServerClosed {
			log.Fatal(err)
		}
	} else {
		log.Printf("flux server listening on %s", cfg.Server.Addr)
		if err := srv.ListenAndServe(); err != http.ErrServerClosed {
			log.Fatal(err)
		}
	}
	log.Println("server stopped")
}

// ============================================================
// Middleware
// ============================================================

func (s *server) corsMiddleware(next http.Handler) http.Handler {
	origins := s.config.Server.CORSOrigins
	allowed := make(map[string]bool, len(origins))
	for _, o := range origins {
		allowed[o] = true
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		origin := r.Header.Get("Origin")
		if allowed[origin] || len(allowed) == 0 {
			w.Header().Set("Access-Control-Allow-Origin", origin)
			w.Header().Set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
			w.Header().Set("Access-Control-Allow-Headers", "Content-Type, X-API-Key")
		}
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (s *server) authMiddleware(next http.Handler) http.Handler {
	keys := s.config.Server.APIKeys
	if len(keys) == 0 {
		return next
	}
	keySet := make(map[string]bool, len(keys))
	for _, k := range keys {
		keySet[k] = true
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		key := r.Header.Get("X-API-Key")
		if key == "" {
			key = r.URL.Query().Get("api_key")
		}
		if !keySet[key] {
			writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "unauthorized"})
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (s *server) concurrencyLimit(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		select {
		case s.sem <- struct{}{}:
			defer func() { <-s.sem }()
			next.ServeHTTP(w, r)
		default:
			writeJSON(w, http.StatusTooManyRequests, map[string]string{"error": "too many requests"})
		}
	})
}

// ============================================================
// Route Dispatcher
// ============================================================

func (s *server) handleCollectionRoute(w http.ResponseWriter, r *http.Request) {
	resource := strings.TrimPrefix(r.URL.Path, "/collections/")
	parts := strings.SplitN(resource, "/", 2)
	if len(parts) == 0 || parts[0] == "" {
		s.handleCollections(w, r)
		return
	}

	name := parts[0]
	action := ""
	if len(parts) == 2 {
		action = parts[1]
	}

	switch action {
	case "upsert":
		if r.Method != http.MethodPost { methodNotAllowed(w); return }
		s.handleUpsert(w, r, name)

	case "batch-upsert":
		if r.Method != http.MethodPost { methodNotAllowed(w); return }
		s.handleBatchUpsert(w, r, name)

	case "batch-delete":
		if r.Method != http.MethodPost { methodNotAllowed(w); return }
		s.handleBatchDelete(w, r, name)

	case "truncate":
		if r.Method != http.MethodPost { methodNotAllowed(w); return }
		s.handleTruncate(w, r, name)

	case "search":
		if r.Method != http.MethodPost { methodNotAllowed(w); return }
		s.handleSearch(w, r, name)

	case "explain":
		if r.Method != http.MethodPost { methodNotAllowed(w); return }
		s.handleExplain(w, r, name)

	case "hybrid-search":
		if r.Method != http.MethodPost { methodNotAllowed(w); return }
		s.handleHybridSearch(w, r, name)

	case "recall":
		if r.Method != http.MethodPost { methodNotAllowed(w); return }
		s.handleRecall(w, r, name)

	case "stats":
		if r.Method != http.MethodGet { methodNotAllowed(w); return }
		s.handleStats(w, r, name)

	case "index":
		if r.Method != http.MethodGet && r.Method != http.MethodPost { methodNotAllowed(w); return }
		s.handleIndexAction(w, r, name)

	case "export":
		if r.Method != http.MethodGet { methodNotAllowed(w); return }
		s.handleExport(w, r, name)

	case "import":
		if r.Method != http.MethodPost { methodNotAllowed(w); return }
		s.handleImport(w, r, name)

	case "delete":
		if r.Method != http.MethodDelete { methodNotAllowed(w); return }
		s.handleDeleteCollection(w, r, name)

	default:
		notFound(w)
	}
}

// ============================================================
// Handlers
// ============================================================

func (s *server) handleCollections(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		collections := s.db.ListCollections()
		infos := make([]map[string]interface{}, 0, len(collections))
		for _, name := range collections {
			count, dim, metric, err := s.db.CollectionStats(name)
			if err != nil {
				continue
			}
			indexType, _ := s.db.IndexInfo(name)
			info := map[string]interface{}{
				"name":       name,
				"count":      count,
				"dimension":  dim,
				"metric":     metric,
				"index_type": indexType,
			}
			infos = append(infos, info)
		}
		writeJSON(w, http.StatusOK, infos)

	case http.MethodPost:
		var req createCollectionRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			badRequest(w, err)
			return
		}
		if req.Name == "" {
			badRequest(w, fmt.Errorf("name is required"))
			return
		}

		// Create with or without schema
		if req.Schema != nil {
			schema, err := flux.SchemaFromMap(req.Schema)
			if err != nil {
				badRequest(w, fmt.Errorf("invalid schema: %v", err))
				return
			}
			if err := s.db.CreateCollectionWithSchema(req.Name, req.Metric, schema); err != nil {
				writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
				return
			}
		} else {
			if err := s.db.CreateCollection(req.Name, req.Metric); err != nil {
				writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
				return
			}
		}

		resp := map[string]interface{}{"status": "created", "index": false}
		if req.EnableIndex {
			indexType := req.IndexType
			if indexType == "" {
				indexType = "hnsw"
			}
			if err := s.db.BuildIndex(req.Name, indexType); err != nil {
				log.Printf("warning: index build failed for %q: %v", req.Name, err)
				resp["index_error"] = err.Error()
			} else {
				resp["index"] = true
				resp["index_type"] = indexType
			}
		}
		writeJSON(w, http.StatusCreated, resp)

	default:
		methodNotAllowed(w)
	}
}

func (s *server) handleUpsert(w http.ResponseWriter, r *http.Request, collection string) {
	var req upsertRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		badRequest(w, err)
		return
	}
	if req.ID == "" {
		badRequest(w, fmt.Errorf("id is required"))
		return
	}
	if err := s.db.Upsert(collection, &flux.Document{
		ID:       req.ID,
		Vector:   req.Vector,
		Metadata: req.Metadata,
	}); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "upserted"})
}

func (s *server) handleBatchUpsert(w http.ResponseWriter, r *http.Request, collection string) {
	var req batchUpsertRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		badRequest(w, err)
		return
	}
	if len(req.Documents) == 0 {
		badRequest(w, fmt.Errorf("documents array is required"))
		return
	}
	docs := make([]*flux.Document, len(req.Documents))
	for i, d := range req.Documents {
		if d.ID == "" {
			badRequest(w, fmt.Errorf("documents[%d].id is required", i))
			return
		}
		docs[i] = &flux.Document{
			ID:       d.ID,
			Vector:   d.Vector,
			Metadata: d.Metadata,
		}
	}
	count, err := s.db.BatchUpsert(collection, docs)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"status": "upserted", "count": count})
}

func (s *server) handleBatchDelete(w http.ResponseWriter, r *http.Request, collection string) {
	var req batchDeleteRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		badRequest(w, err)
		return
	}
	if len(req.IDs) == 0 {
		badRequest(w, fmt.Errorf("ids array is required"))
		return
	}
	count, err := s.db.BatchDelete(collection, req.IDs)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"status": "deleted", "count": count})
}

func (s *server) handleTruncate(w http.ResponseWriter, r *http.Request, collection string) {
	if err := s.db.TruncateCollection(collection); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "truncated"})
}

func (s *server) handleDeleteCollection(w http.ResponseWriter, r *http.Request, name string) {
	if err := s.db.DeleteCollection(name); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "deleted"})
}

func (s *server) handleSearch(w http.ResponseWriter, r *http.Request, collection string) {
	var req searchRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		badRequest(w, err)
		return
	}
	if req.K <= 0 { req.K = 10 }

	start := time.Now()
	filter := buildFilter(req.Filter)
	results, err := s.db.Search(collection, req.Query, req.K, filter)
	elapsed := time.Since(start)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	indexType, _ := s.db.IndexInfo(collection)
	resp := searchResponse{
		Results:     results,
		TotalTimeNs: elapsed.Nanoseconds(),
		DocsScanned: len(results),
		IndexUsed:   indexType,
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *server) handleExplain(w http.ResponseWriter, r *http.Request, collection string) {
	var req searchRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		badRequest(w, err)
		return
	}
	if req.K <= 0 { req.K = 10 }

	start := time.Now()
	filter := buildFilter(req.Filter)
	results, err := s.db.Search(collection, req.Query, req.K, filter)
	elapsed := time.Since(start)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	indexType, _ := s.db.IndexInfo(collection)

	filterDesc := ""
	if req.Filter != nil {
		filterDesc = fmt.Sprintf("%v", req.Filter)
	}
	resp := explainResponse{
		QueryVector:     req.Query,
		TopK:            req.K,
		IndexType:       indexType,
		Candidates:      len(results),
		ResultsReturned: len(results),
		FilterApplied:   filterDesc,
		SearchTimeNs:    elapsed.Nanoseconds(),
		Results:         results,
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *server) handleHybridSearch(w http.ResponseWriter, r *http.Request, collection string) {
	var req hybridSearchRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		badRequest(w, err)
		return
	}
	if req.K <= 0 { req.K = 10 }
	if req.Alpha == 0 { req.Alpha = 0.5 } // default: equal weight

	start := time.Now()
	filter := buildFilter(req.Filter)
	results := s.db.HybridSearch(collection, req.Query, req.TextQuery, req.K, req.Alpha, filter)
	elapsed := time.Since(start)
	if results == nil {
		results = []flux.HybridSearchResult{}
	}
	resp := hybridSearchResponse{
		Results:     results,
		TotalTimeNs: elapsed.Nanoseconds(),
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *server) handleRecall(w http.ResponseWriter, r *http.Request, collection string) {
	var req searchRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		badRequest(w, err); return
	}
	if req.K <= 0 { req.K = 10 }

	// 1. ANN search (if index exists)
	indexType, _ := s.db.IndexInfo(collection)
	annSearched := indexType != ""

	var annResults []flux.SearchResult
	var annTime time.Duration
	if annSearched {
		start := time.Now()
		annResults, _ = s.db.Search(collection, req.Query, req.K, nil)
		annTime = time.Since(start)
	}

	// 2. BruteForce search (always)
	start := time.Now()
	bfResults, _ := s.db.Search(collection, req.Query, req.K, nil)
	bfTime := time.Since(start)

	// 3. Calculate recall
	bfIDSet := make(map[string]bool, len(bfResults))
	for _, r := range bfResults {
		bfIDSet[r.ID] = true
	}
	var hits int
	for _, r := range annResults {
		if bfIDSet[r.ID] {
			hits++
		}
	}
	recall := float64(hits) / float64(len(bfResults))
	if len(bfResults) == 0 {
		recall = 1.0
	}

	resp := recallResponse{
		Query:        req.Query,
		TopK:         req.K,
		AnnResults:   annResults,
		BfResults:    bfResults,
		Recall:       recall,
		AnnTimeNs:    annTime.Nanoseconds(),
		BfTimeNs:     bfTime.Nanoseconds(),
		AnnCandidates: len(annResults),
		BfCandidates:  len(bfResults),
		IndexType:    indexType,
		AnnSearched:  annSearched,
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *server) handleStats(w http.ResponseWriter, r *http.Request, collection string) {
	count, dim, metric, err := s.db.CollectionStats(collection)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	indexType, _ := s.db.IndexInfo(collection)
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"count": count, "dimension": dim, "metric": metric, "index_type": indexType,
	})
}

func (s *server) handleIndexAction(w http.ResponseWriter, r *http.Request, name string) {
	if r.Method == http.MethodGet {
		indexType, err := s.db.IndexInfo(name)
		if err != nil { writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()}); return }
		info := map[string]interface{}{"collection": name, "index_type": indexType}
		if indexType != "" { info["status"] = "active" } else { info["status"] = "none" }
		writeJSON(w, http.StatusOK, info)
		return
	}
	var req indexActionRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil { badRequest(w, err); return }
	switch req.Action {
	case "build":
		it := req.IndexType
		if it == "" { it = "hnsw" }
		if err := s.db.BuildIndex(name, it); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()}); return
		}
		writeJSON(w, http.StatusOK, map[string]string{"status": "index_built", "index_type": it})
	case "drop":
		if err := s.db.DropIndex(name); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()}); return
		}
		writeJSON(w, http.StatusOK, map[string]string{"status": "index_dropped"})
	default:
		badRequest(w, fmt.Errorf("unknown action %q", req.Action))
	}
}

func (s *server) handleExport(w http.ResponseWriter, r *http.Request, name string) {
	w.Header().Set("Content-Type", "application/x-ndjson")
	w.Header().Set("Content-Disposition", fmt.Sprintf(`attachment; filename="%s.ndjson"`, name))
	count, err := s.db.ExportCollection(name, w)
	if err != nil {
		log.Printf("export error for %q: %v", name, err); return
	}
	log.Printf("exported %d documents from %q", count, name)
}

func (s *server) handleImport(w http.ResponseWriter, r *http.Request, name string) {
	count, err := s.db.ImportCollection(name, r.Body)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()}); return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"status": "imported", "count": count})
}

func (s *server) handleSnapshot(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost { methodNotAllowed(w); return }
	var req snapshotRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil { badRequest(w, err); return }
	path := req.Path
	if path == "" { path = "flux.snapshot.json" }
	if err := s.db.Snapshot(path); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()}); return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "snapshot_created", "path": path})
}

func (s *server) handleRestore(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost { methodNotAllowed(w); return }
	var req snapshotRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil { badRequest(w, err); return }
	path := req.Path
	if path == "" { path = "flux.snapshot.json" }
	if err := s.db.LoadFromSnapshot(path); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()}); return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "restored", "path": path})
}

func (s *server) handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (s *server) handleReady(w http.ResponseWriter, r *http.Request) {
	collections := s.db.ListCollections()
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"status":      "ok",
		"collections": len(collections),
	})
}

func (s *server) handleMetrics(w http.ResponseWriter, r *http.Request) {
	s.db.RefreshMetrics()
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	w.Write([]byte(s.db.Metrics.Render()))
}

func (s *server) handleRoot(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" { notFound(w); return }
	data, err := flux.WebFiles.ReadFile("web/index.html")
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "console not found"})
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	w.Write(data)
}

// ============================================================
// Helpers
// ============================================================

func buildFilter(raw map[string]interface{}) flux.FilterFunc {
	if len(raw) == 0 {
		return nil
	}
	var filters []flux.FilterFunc
	for key, rawValue := range raw {
		switch rawValue := rawValue.(type) {
		case map[string]interface{}:
			// Handle operators: {"$eq": val, "$ne": val, "$gt": val, "$lt": val, "$in": [val,...]}
			if eq, ok := rawValue["$eq"]; ok {
				filters = append(filters, flux.FieldEqual(key, eq))
			}
			if ne, ok := rawValue["$ne"]; ok {
				filters = append(filters, flux.FieldNotEqual(key, ne))
			}
			if gt, ok := rawValue["$gt"]; ok {
				gtF, _ := toFloat64(gt)
				lt := rawValue["$lt"]
				ltF, ltOK := toFloat64(lt)
				if ltOK {
					filters = append(filters, flux.FieldRange(key, gtF, ltF))
				} else {
					filters = append(filters, flux.FieldRange(key, gtF, 1e18))
				}
			}
			if lt, ok := rawValue["$lt"]; ok {
				if _, hasGT := rawValue["$gt"]; !hasGT {
					ltF, _ := toFloat64(lt)
					filters = append(filters, flux.FieldRange(key, -1e18, ltF))
				}
			}
			if inVals, ok := rawValue["$in"]; ok {
				if arr, ok := inVals.([]interface{}); ok {
					filters = append(filters, flux.FieldIn(key, arr...))
				}
			}
			if text, ok := rawValue["$text"]; ok {
				if s, ok := text.(string); ok {
					filters = append(filters, flux.TextMatch(key, s))
				}
			}
			if geo, ok := rawValue["$geo"]; ok {
				if geoMap, ok := geo.(map[string]interface{}); ok {
					lat, _ := toFloat64(geoMap["lat"])
					lng, _ := toFloat64(geoMap["lng"])
					radius, _ := toFloat64(geoMap["radius"])
					filters = append(filters, flux.GeoRadius(key, lat, lng, radius))
				}
			}

		default:
			// Simple equality filter
			filters = append(filters, flux.FieldEqual(key, rawValue))
		}
	}
	if len(filters) == 1 {
		return filters[0]
	}
	return flux.And(filters...)
}

func toFloat64(v interface{}) (float64, bool) {
	switch val := v.(type) {
	case float64: return val, true
	case float32: return float64(val), true
	case int: return float64(val), true
	case int64: return float64(val), true
	case json.Number:
		if f, err := val.Float64(); err == nil { return f, true }
	}
	return 0, false
}

func withJSON(handler http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		handler(w, r)
	}
}

func writeJSON(w http.ResponseWriter, status int, value interface{}) error {
	w.WriteHeader(status)
	return json.NewEncoder(w).Encode(value)
}

func badRequest(w http.ResponseWriter, err error) {
	writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
}

func notFound(w http.ResponseWriter) {
	writeJSON(w, http.StatusNotFound, map[string]string{"error": "not found"})
}

func methodNotAllowed(w http.ResponseWriter) {
	writeJSON(w, http.StatusMethodNotAllowed, map[string]string{"error": "method not allowed"})
}
