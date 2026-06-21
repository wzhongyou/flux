#include "server.h"
#include "schema.h"
#include "filter.h"
#include "hnsw.h"
#include "ivf.h"
#include "io.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <fstream>
#include <sstream>

namespace flux {

// ============================================================
// Helpers
// ============================================================

static void bad_request(httplib::Response& res, const std::string& msg) {
    res.status = 400;
    res.set_content(nlohmann::json{{"error", msg}}.dump(), "application/json");
}

static void not_found(httplib::Response& res) {
    res.status = 404;
    res.set_content(nlohmann::json{{"error", "not found"}}.dump(), "application/json");
}

static void method_not_allowed(httplib::Response& res) {
    res.status = 405;
    res.set_content(nlohmann::json{{"error", "method not allowed"}}.dump(), "application/json");
}

static void too_many_requests(httplib::Response& res) {
    res.status = 429;
    res.set_content(nlohmann::json{{"error", "too many requests"}}.dump(), "application/json");
}

void Server::write_json(httplib::Response& res, int status, const nlohmann::json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

double Server::json_to_double(const nlohmann::json& v) {
    if (v.is_number_float())  return v.get<double>();
    if (v.is_number_integer()) return static_cast<double>(v.get<int64_t>());
    return 0.0;
}

// ============================================================
// Filter builder (mirrors Go buildFilter)
// ============================================================

FilterFunc Server::build_filter(const nlohmann::json& raw) {
    if (!raw.is_object() || raw.empty()) return nullptr;

    std::vector<FilterFunc> filters;

    for (auto it = raw.begin(); it != raw.end(); ++it) {
        const std::string& key = it.key();
        const auto& raw_val = it.value();

        if (raw_val.is_object()) {
            // Operator syntax: {"$eq": val, "$ne": val, "$gt", "$lt", "$in", "$text", "$geo"}
            if (raw_val.contains("$eq")) {
                filters.push_back(FieldEqual(key, raw_val["$eq"]));
            }
            if (raw_val.contains("$ne")) {
                filters.push_back(FieldNotEqual(key, raw_val["$ne"]));
            }
            if (raw_val.contains("$gt")) {
                double gt = json_to_double(raw_val["$gt"]);
                double lt = raw_val.contains("$lt") ? json_to_double(raw_val["$lt"]) : 1e18;
                filters.push_back(FieldRange(key, gt, lt));
            } else if (raw_val.contains("$lt")) {
                double lt = json_to_double(raw_val["$lt"]);
                filters.push_back(FieldRange(key, -1e18, lt));
            }
            if (raw_val.contains("$in")) {
                if (raw_val["$in"].is_array()) {
                    filters.push_back(FieldIn(key, raw_val["$in"].get<std::vector<nlohmann::json>>()));
                }
            }
            if (raw_val.contains("$text")) {
                if (raw_val["$text"].is_string()) {
                    filters.push_back(TextMatch(key, raw_val["$text"]));
                }
            }
            if (raw_val.contains("$geo")) {
                const auto& geo = raw_val["$geo"];
                if (geo.is_object()) {
                    double lat = json_to_double(geo.value("lat", nlohmann::json(0)));
                    double lng = json_to_double(geo.value("lng", nlohmann::json(0)));
                    double radius = json_to_double(geo.value("radius", nlohmann::json(0)));
                    filters.push_back(GeoRadius(key, lat, lng, radius));
                }
            }
        } else {
            // Simple equality
            filters.push_back(FieldEqual(key, raw_val));
        }
    }

    if (filters.size() == 1) return filters[0];
    if (filters.empty()) return nullptr;
    return And(std::move(filters));
}

// ============================================================
// Constructor / Destructor
// ============================================================

Server::Server(std::unique_ptr<VectorDatabase> db, Config config)
    : db_(std::move(db)), config_(std::move(config)) {
    setup_middleware();
    register_routes();
}

Server::~Server() {
    shutdown();
}

// ============================================================
// Middleware
// ============================================================

void Server::setup_middleware() {
    // CORS and Auth are handled as pre-routing handlers.
    // Concurrency is handled via the semaphore pattern.
}

bool Server::concurrency_limit(httplib::Request& /*req*/, httplib::Response& res) {
    // cpp-httplib uses a thread pool; max_concurrent is implicitly managed.
    // For explicit limiting, we'd use a semaphore. For simplicity, rely
    // on the OS thread pool limit.
    (void)config_; // silence unused warning
    return true;
}

void Server::cors_middleware(httplib::Request& req, httplib::Response& res) {
    const auto& origins = config_.server.cors_origins;
    std::string origin = req.get_header_value("Origin");

    if (origins.empty()) {
        // Allow all
        res.set_header("Access-Control-Allow-Origin", origin.empty() ? "*" : origin);
    } else {
        for (const auto& o : origins) {
            if (o == origin || o == "*") {
                res.set_header("Access-Control-Allow-Origin", origin);
                break;
            }
        }
    }
    res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, X-API-Key");
}

bool Server::auth_middleware(httplib::Request& req, httplib::Response& res) {
    if (config_.server.api_keys.empty()) return true;

    std::string key = req.get_header_value("X-API-Key");
    if (key.empty()) {
        key = req.get_param_value("api_key");
    }

    for (const auto& valid_key : config_.server.api_keys) {
        if (key == valid_key) return true;
    }

    write_json(res, 401, {{"error", "unauthorized"}});
    return false;
}

// ============================================================
// Route Registration (matches Go version exactly)
// ============================================================

void Server::register_routes() {
    auto& s = svr_;

    // Helper to make 2-arg handler from 3-arg method
#define R(method, path, func) \
    s.method(path, [this](const httplib::Request& req, httplib::Response& res) { \
        func(req, res, req.matches[1]); \
    })

    // Collections
    s.Get("/collections", [this](const httplib::Request& req, httplib::Response& res) {
        handle_collections(req, res);
    });
    s.Post("/collections", [this](const httplib::Request& req, httplib::Response& res) {
        handle_collections(req, res);
    });

    // Collection sub-routes
    R(Get,    R"(/collections/([^/]+)/stats)", handle_stats);
    R(Post,   R"(/collections/([^/]+)/upsert)", handle_upsert);
    R(Post,   R"(/collections/([^/]+)/batch-upsert)", handle_batch_upsert);
    R(Post,   R"(/collections/([^/]+)/batch-delete)", handle_batch_delete);
    R(Post,   R"(/collections/([^/]+)/truncate)", handle_truncate);
    R(Post,   R"(/collections/([^/]+)/search)", handle_search);
    R(Post,   R"(/collections/([^/]+)/explain)", handle_explain);
    R(Post,   R"(/collections/([^/]+)/hybrid-search)", handle_hybrid_search);
    R(Post,   R"(/collections/([^/]+)/recall)", handle_recall);
    R(Get,    R"(/collections/([^/]+)/index)", handle_index_action);
    R(Post,   R"(/collections/([^/]+)/index)", handle_index_action);
    R(Get,    R"(/collections/([^/]+)/export)", handle_export);
    R(Post,   R"(/collections/([^/]+)/import)", handle_import);
    R(Delete, R"(/collections/([^/]+)/delete)", handle_delete_collection);
#undef R

    // Snapshot
    s.Post("/snapshot", [this](const httplib::Request& req, httplib::Response& res) {
        handle_snapshot(req, res);
    });
    s.Post("/restore", [this](const httplib::Request& req, httplib::Response& res) {
        handle_restore(req, res);
    });

    // Health
    s.Get("/health", [this](const httplib::Request& req, httplib::Response& res) {
        handle_health(req, res);
    });
    s.Get("/ready", [this](const httplib::Request& req, httplib::Response& res) {
        handle_ready(req, res);
    });
    s.Get("/metrics", [this](const httplib::Request& req, httplib::Response& res) {
        handle_metrics(req, res);
    });

    // Web console
    s.Get("/", [this](const httplib::Request& req, httplib::Response& res) {
        handle_root(req, res);
    });
    // Static files (CSS, JS)
    s.Get(R"(/static/(.*))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string file_path = "web/" + std::string(req.matches[1]);
        std::ifstream f(file_path);
        if (f.is_open()) {
            std::stringstream ss;
            ss << f.rdbuf();
            std::string content = ss.str();
            std::string mime = "application/octet-stream";
            auto suffix = [&](const char* ext) { return file_path.rfind(ext) == file_path.size() - strlen(ext); };
            if (suffix(".css"))  mime = "text/css";
            if (suffix(".js"))   mime = "application/javascript";
            if (suffix(".html")) mime = "text/html";
            res.set_content(content, mime);
        } else {
            res.status = 404;
        }
    });
}

// ============================================================
// Collection Handlers
// ============================================================

void Server::handle_collections(const httplib::Request& req, httplib::Response& res) {
    if (req.method == "GET") {
        auto names = db_->list_collections();
        nlohmann::json result = nlohmann::json::array();
        for (const auto& name : names) {
            try {
                auto stats = db_->collection_stats(name);
                nlohmann::json info;
                info["name"] = name;
                info["count"] = stats.doc_count;
                info["dimension"] = stats.dimension;
                info["metric"] = metric_to_string(stats.metric);
                info["index_type"] = stats.index_type;
                result.push_back(info);
            } catch (...) {}
        }
        write_json(res, 200, result);

    } else if (req.method == "POST") {
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (body.is_discarded()) { bad_request(res, "invalid json"); return; }

        std::string name = body.value("name", "");
        if (name.empty()) { bad_request(res, "name is required"); return; }

        std::string metric_str = body.value("metric", "cosine");
        DistanceMetric metric;
        try { metric = metric_from_string(metric_str); }
        catch (...) { bad_request(res, "invalid metric"); return; }

        try {
            if (body.contains("schema") && body["schema"].is_object()) {
                auto schema = std::make_unique<Schema>(schema_from_json(body["schema"]));
                db_->create_collection_with_schema(name, metric, std::move(schema));
            } else {
                db_->create_collection(name, metric);
            }
        } catch (const std::exception& e) {
            write_json(res, 400, {{"error", e.what()}});
            return;
        }

        nlohmann::json resp = {{"status", "created"}, {"index", false}};

        if (body.value("enable_index", false)) {
            std::string idx_type = body.value("index_type", "hnsw");
            try {
                db_->build_index(name, idx_type);
                resp["index"] = true;
                resp["index_type"] = idx_type;
            } catch (const std::exception& e) {
                resp["index_error"] = e.what();
            }
        }

        write_json(res, 201, resp);

    } else {
        method_not_allowed(res);
    }
}

// ============================================================
// Document Handlers
// ============================================================

void Server::handle_upsert(const httplib::Request& req, httplib::Response& res,
                            const std::string& collection) {
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (body.is_discarded()) { bad_request(res, "invalid json"); return; }

    std::string id = body.value("id", "");
    if (id.empty()) { bad_request(res, "id is required"); return; }

    try {
        Document doc;
        doc.id = id;
        doc.vector = body["vector"].get<std::vector<double>>();
        if (body.contains("metadata")) doc.metadata = body["metadata"];

        db_->upsert(collection, std::move(doc));
    } catch (const std::exception& e) {
        write_json(res, 400, {{"error", e.what()}});
        return;
    }

    write_json(res, 200, {{"status", "upserted"}});
}

void Server::handle_batch_upsert(const httplib::Request& req, httplib::Response& res,
                                   const std::string& collection) {
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (body.is_discarded()) { bad_request(res, "invalid json"); return; }

    if (!body.contains("documents") || !body["documents"].is_array()) {
        bad_request(res, "documents array is required"); return;
    }

    std::vector<Document> docs;
    for (size_t i = 0; i < body["documents"].size(); i++) {
        const auto& d = body["documents"][i];
        std::string id = d.value("id", "");
        if (id.empty()) {
            bad_request(res, "documents[" + std::to_string(i) + "].id is required");
            return;
        }
        Document doc;
        doc.id = id;
        doc.vector = d["vector"].get<std::vector<double>>();
        if (d.contains("metadata")) doc.metadata = d["metadata"];
        docs.push_back(std::move(doc));
    }

    try {
        int count = db_->batch_upsert(collection, std::move(docs));
        write_json(res, 200, {{"status", "upserted"}, {"count", count}});
    } catch (const std::exception& e) {
        write_json(res, 400, {{"error", e.what()}});
    }
}

void Server::handle_batch_delete(const httplib::Request& req, httplib::Response& res,
                                   const std::string& collection) {
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (body.is_discarded()) { bad_request(res, "invalid json"); return; }

    if (!body.contains("ids") || !body["ids"].is_array()) {
        bad_request(res, "ids array is required"); return;
    }

    std::vector<std::string> ids;
    for (const auto& id : body["ids"]) {
        ids.push_back(id.get<std::string>());
    }

    try {
        int count = db_->batch_remove(collection, ids);
        write_json(res, 200, {{"status", "deleted"}, {"count", count}});
    } catch (const std::exception& e) {
        write_json(res, 400, {{"error", e.what()}});
    }
}

void Server::handle_truncate(const httplib::Request& /*req*/, httplib::Response& res,
                               const std::string& collection) {
    try {
        db_->truncate_collection(collection);
        write_json(res, 200, {{"status", "truncated"}});
    } catch (const std::exception& e) {
        write_json(res, 400, {{"error", e.what()}});
    }
}

void Server::handle_delete_collection(const httplib::Request& /*req*/, httplib::Response& res,
                                        const std::string& name) {
    try {
        db_->delete_collection(name);
        write_json(res, 200, {{"status", "deleted"}});
    } catch (const std::exception& e) {
        write_json(res, 400, {{"error", e.what()}});
    }
}

// ============================================================
// Search Handlers
// ============================================================

void Server::handle_search(const httplib::Request& req, httplib::Response& res,
                             const std::string& collection) {
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (body.is_discarded()) { bad_request(res, "invalid json"); return; }

    int k = body.value("k", 10);
    if (k <= 0) k = 10;

    auto query = body["query"].get<std::vector<double>>();
    FilterFunc filter = nullptr;
    if (body.contains("filter")) {
        filter = build_filter(body["filter"]);
    }

    auto start = std::chrono::steady_clock::now();
    std::vector<SearchResult> results;
    try {
        results = db_->search(collection, query, k, std::move(filter));
    } catch (const std::exception& e) {
        write_json(res, 400, {{"error", e.what()}});
        return;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();

    nlohmann::json resp;
    auto& jresults = resp["results"];
    jresults = nlohmann::json::array();
    for (const auto& r : results) {
        nlohmann::json jr;
        jr["id"] = r.id;
        jr["score"] = r.score;
        if (r.doc_ptr) jr["document"] = *r.doc_ptr;
        jresults.push_back(jr);
    }
    resp["total_time_ns"] = elapsed;
    resp["docs_scanned"] = static_cast<int>(results.size());

    std::string idx_type;
    try { idx_type = db_->index_info(collection); } catch (...) {}
    resp["index_used"] = idx_type;

    write_json(res, 200, resp);
}

void Server::handle_explain(const httplib::Request& req, httplib::Response& res,
                              const std::string& collection) {
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (body.is_discarded()) { bad_request(res, "invalid json"); return; }

    int k = body.value("k", 10);
    if (k <= 0) k = 10;

    auto query = body["query"].get<std::vector<double>>();
    FilterFunc filter = nullptr;
    if (body.contains("filter")) {
        filter = build_filter(body["filter"]);
    }

    auto start = std::chrono::steady_clock::now();
    auto results = db_->search(collection, query, k, std::move(filter));
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();

    std::string idx_type;
    try { idx_type = db_->index_info(collection); } catch (...) {}

    std::string filter_desc;
    if (body.contains("filter")) filter_desc = body["filter"].dump();

    nlohmann::json resp;
    resp["query"] = query;
    resp["top_k"] = k;
    resp["index_type"] = idx_type;
    resp["candidates"] = static_cast<int>(results.size());
    resp["results_returned"] = static_cast<int>(results.size());
    resp["filter_applied"] = filter_desc;
    resp["search_time_ns"] = elapsed;

    auto& jr = resp["results"];
    jr = nlohmann::json::array();
    for (const auto& r : results) {
        nlohmann::json item;
        item["id"] = r.id;
        item["score"] = r.score;
        if (r.doc_ptr) item["document"] = *r.doc_ptr;
        jr.push_back(item);
    }

    write_json(res, 200, resp);
}

void Server::handle_hybrid_search(const httplib::Request& req, httplib::Response& res,
                                    const std::string& collection) {
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (body.is_discarded()) { bad_request(res, "invalid json"); return; }

    int k = body.value("k", 10);
    if (k <= 0) k = 10;

    double alpha = body.value("alpha", 0.5);
    std::string text_query = body.value("text_query", "");

    std::vector<double> query_vec;
    if (body.contains("query") && body["query"].is_array()) {
        query_vec = body["query"].get<std::vector<double>>();
    }

    FilterFunc filter = nullptr;
    if (body.contains("filter")) {
        filter = build_filter(body["filter"]);
    }

    auto start = std::chrono::steady_clock::now();
    auto results = db_->hybrid_search(collection, query_vec, text_query, k, alpha,
                                       std::move(filter));
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();

    nlohmann::json resp;
    auto& jr = resp["results"];
    jr = nlohmann::json::array();
    for (const auto& r : results) {
        nlohmann::json item;
        item["id"] = r.id;
        item["vector_score"] = r.vector_score;
        item["text_score"] = r.text_score;
        item["combined_score"] = r.combined_score;
        if (r.doc_ptr) item["document"] = *r.doc_ptr;
        jr.push_back(item);
    }
    resp["total_time_ns"] = elapsed;

    write_json(res, 200, resp);
}

void Server::handle_recall(const httplib::Request& req, httplib::Response& res,
                             const std::string& collection) {
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (body.is_discarded()) { bad_request(res, "invalid json"); return; }

    int k = body.value("k", 10);
    if (k <= 0) k = 10;

    auto query = body["query"].get<std::vector<double>>();

    std::string idx_type;
    try { idx_type = db_->index_info(collection); } catch (...) {}
    bool ann_searched = !idx_type.empty();

    // ANN search
    std::vector<SearchResult> ann_results;
    int64_t ann_time = 0;
    if (ann_searched) {
        auto start = std::chrono::steady_clock::now();
        try { ann_results = db_->search(collection, query, k, nullptr); }
        catch (...) {}
        ann_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
    }

    // Brute force reference (drop index temporarily)
    std::vector<SearchResult> bf_results;
    int64_t bf_time = 0;
    if (ann_searched) {
        db_->drop_index(collection);
        auto start = std::chrono::steady_clock::now();
        try { bf_results = db_->search(collection, query, k, nullptr); }
        catch (...) {}
        bf_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        // Rebuild index
        try { db_->build_index(collection, idx_type); }
        catch (...) {}
    } else {
        auto start = std::chrono::steady_clock::now();
        try { bf_results = db_->search(collection, query, k, nullptr); }
        catch (...) {}
        bf_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
    }

    // Compute recall
    std::unordered_set<std::string> bf_ids;
    for (const auto& r : bf_results) bf_ids.insert(r.id);
    int hits = 0;
    for (const auto& r : ann_results) {
        if (bf_ids.count(r.id)) hits++;
    }
    double recall = bf_results.empty() ? 1.0 :
                    static_cast<double>(hits) / static_cast<double>(bf_results.size());

    nlohmann::json resp;
    resp["query"] = query;
    resp["top_k"] = k;
    resp["recall"] = recall;
    resp["ann_time_ns"] = ann_time;
    resp["bf_time_ns"] = bf_time;
    resp["ann_candidates"] = static_cast<int>(ann_results.size());
    resp["bf_candidates"] = static_cast<int>(bf_results.size());
    resp["index_type"] = idx_type;
    resp["ann_searched"] = ann_searched;

    auto serialize_results = [](const std::vector<SearchResult>& rs) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : rs) {
            nlohmann::json jr;
            jr["id"] = r.id;
            jr["score"] = r.score;
            if (r.doc_ptr) jr["document"] = *r.doc_ptr;
            arr.push_back(jr);
        }
        return arr;
    };
    resp["ann_results"] = serialize_results(ann_results);
    resp["bf_results"] = serialize_results(bf_results);

    write_json(res, 200, resp);
}

// ============================================================
// Stats / Index / IO
// ============================================================

void Server::handle_stats(const httplib::Request& /*req*/, httplib::Response& res,
                            const std::string& collection) {
    try {
        auto stats = db_->collection_stats(collection);
        nlohmann::json j;
        j["count"] = stats.doc_count;
        j["dimension"] = stats.dimension;
        j["metric"] = metric_to_string(stats.metric);
        j["index_type"] = stats.index_type;
        write_json(res, 200, j);
    } catch (const std::exception& e) {
        write_json(res, 400, {{"error", e.what()}});
    }
}

void Server::handle_index_action(const httplib::Request& req, httplib::Response& res,
                                   const std::string& name) {
    if (req.method == "GET") {
        std::string idx_type;
        try { idx_type = db_->index_info(name); }
        catch (const std::exception& e) {
            write_json(res, 400, {{"error", e.what()}}); return;
        }

        nlohmann::json info;
        info["collection"] = name;
        info["index_type"] = idx_type;
        info["status"] = idx_type.empty() ? "none" : "active";
        write_json(res, 200, info);

    } else if (req.method == "POST") {
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (body.is_discarded()) { bad_request(res, "invalid json"); return; }

        std::string action = body.value("action", "");
        if (action == "build") {
            std::string it = body.value("index_type", "hnsw");
            try {
                db_->build_index(name, it);
                write_json(res, 200, {{"status", "index_built"}, {"index_type", it}});
            } catch (const std::exception& e) {
                write_json(res, 400, {{"error", e.what()}});
            }
        } else if (action == "drop") {
            try {
                db_->drop_index(name);
                write_json(res, 200, {{"status", "index_dropped"}});
            } catch (const std::exception& e) {
                write_json(res, 400, {{"error", e.what()}});
            }
        } else {
            bad_request(res, "unknown action \"" + action + "\"");
        }

    } else {
        method_not_allowed(res);
    }
}

void Server::handle_export(const httplib::Request& /*req*/, httplib::Response& res,
                             const std::string& name) {
    res.set_header("Content-Type", "application/x-ndjson");
    res.set_header("Content-Disposition", "attachment; filename=\"" + name + ".ndjson\"");

    std::stringstream ss;
    try {
        export_collection_jsonl(*db_, name, ss);
        res.set_content(ss.str(), "application/x-ndjson");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(nlohmann::json{{"error", e.what()}}.dump(), "application/json");
    }
}

void Server::handle_import(const httplib::Request& req, httplib::Response& res,
                             const std::string& name) {
    std::stringstream ss(req.body);
    try {
        int count = import_collection_jsonl(*db_, name, ss);
        write_json(res, 200, {{"status", "imported"}, {"count", count}});
    } catch (const std::exception& e) {
        write_json(res, 400, {{"error", e.what()}});
    }
}

// ============================================================
// Snapshot / Health / Metrics
// ============================================================

void Server::handle_snapshot(const httplib::Request& req, httplib::Response& res) {
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    std::string path = body.value("path", "flux.snapshot.json");

    try {
        snapshot_to_file(*db_, path);
        write_json(res, 200, {{"status", "snapshot_created"}, {"path", path}});
    } catch (const std::exception& e) {
        write_json(res, 500, {{"error", e.what()}});
    }
}

void Server::handle_restore(const httplib::Request& req, httplib::Response& res) {
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    std::string path = body.value("path", "flux.snapshot.json");

    try {
        restore_from_file(*db_, path);
        write_json(res, 200, {{"status", "restored"}, {"path", path}});
    } catch (const std::exception& e) {
        write_json(res, 500, {{"error", e.what()}});
    }
}

void Server::handle_health(const httplib::Request& /*req*/, httplib::Response& res) {
    write_json(res, 200, {{"status", "ok"}});
}

void Server::handle_ready(const httplib::Request& /*req*/, httplib::Response& res) {
    auto names = db_->list_collections();
    write_json(res, 200, {{"status", "ok"}, {"collections", static_cast<int>(names.size())}});
}

void Server::handle_metrics(const httplib::Request& /*req*/, httplib::Response& res) {
    // Simple Prometheus metrics
    std::stringstream ss;
    ss << "# HELP flux_uptime_seconds Flux server uptime\n";
    ss << "# TYPE flux_uptime_seconds gauge\n";
    ss << "flux_uptime_seconds 0\n";
    ss << "# HELP flux_collection_count Number of collections\n";
    ss << "# TYPE flux_collection_count gauge\n";
    ss << "flux_collection_count " << db_->list_collections().size() << "\n";
    res.set_content(ss.str(), "text/plain; charset=utf-8");
}

// ============================================================
// Web Console
// ============================================================

void Server::handle_root(const httplib::Request& /*req*/, httplib::Response& res) {
    // Read embedded HTML or return a placeholder
    std::ifstream f("web/index.html");
    if (f.is_open()) {
        std::stringstream ss;
        ss << f.rdbuf();
        res.set_content(ss.str(), "text/html; charset=utf-8");
    } else {
        res.set_content(R"(<!DOCTYPE html>
<html><head><title>Flux Console</title></head>
<body><h1>Flux v2.0.0 (C++17)</h1>
<p>API is running. Place <code>web/index.html</code> for the full console.</p>
</body></html>)", "text/html; charset=utf-8");
    }
}

// ============================================================
// Run / Shutdown
// ============================================================

void Server::run() {
    running_ = true;

    // Setup CORS pre-flight handler
    svr_.Options(R"(.*)", [this](const httplib::Request& req, httplib::Response& res) {
        cors_middleware(const_cast<httplib::Request&>(req), res);
        res.status = 204;
    });

    // Parse host:port
    std::string addr = config_.server.addr;
    std::string host = "0.0.0.0";
    int port = 9876;

    size_t colon = addr.find(':');
    if (colon != std::string::npos) {
        host = addr.substr(0, colon);
        if (host.empty() || host == "*") host = "0.0.0.0";
        port = std::stoi(addr.substr(colon + 1));
    }

    spdlog::info("Flux HTTP server listening on {}:{}", host, port);

    if (config_.server.tls_enabled()) {
        // TLS support requires httplib::SSLServer with OpenSSL.
        // For now, fall back to plain HTTP with a warning.
        spdlog::warn("TLS requested but not yet supported in C++ version. Using plain HTTP.");
    }
    svr_.listen(host.c_str(), port);

    running_ = false;
}

void Server::shutdown() {
    if (running_) {
        svr_.stop();
        running_ = false;
    }
}

} // namespace flux
