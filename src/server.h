#pragma once

#include "database.h"
#include "config.h"

#include <httplib.h>
#include <memory>
#include <string>
#include <vector>

namespace flux {

// ============================================================
// HTTP Server — wraps cpp-httplib with Flux API endpoints
// (mirrors Go cmd/flux/main.go)
// ============================================================

class Server {
public:
    Server(std::unique_ptr<VectorDatabase> db, Config config);
    ~Server();

    // Start the HTTP server (blocking).
    // Returns when the server shuts down.
    void run();

    // Signal shutdown gracefully.
    void shutdown();

private:
    // ---- Middleware ----
    void setup_middleware();
    void cors_middleware(httplib::Request& req, httplib::Response& res);
    bool auth_middleware(httplib::Request& req, httplib::Response& res);
    bool concurrency_limit(httplib::Request& req, httplib::Response& res);

    // ---- Route registration ----
    void register_routes();

    // ---- Helpers ----
    static FilterFunc build_filter(const nlohmann::json& raw);
    static double json_to_double(const nlohmann::json& v);
    static void write_json(httplib::Response& res, int status, const nlohmann::json& body);

    // ---- Collection handlers ----
    void handle_collections(const httplib::Request& req, httplib::Response& res);
    void handle_collection_route(const httplib::Request& req, httplib::Response& res);

    // ---- Document handlers ----
    void handle_upsert(const httplib::Request& req, httplib::Response& res,
                       const std::string& collection);
    void handle_batch_upsert(const httplib::Request& req, httplib::Response& res,
                              const std::string& collection);
    void handle_batch_delete(const httplib::Request& req, httplib::Response& res,
                              const std::string& collection);
    void handle_truncate(const httplib::Request& req, httplib::Response& res,
                          const std::string& collection);
    void handle_delete_collection(const httplib::Request& req, httplib::Response& res,
                                   const std::string& name);

    // ---- Search handlers ----
    void handle_search(const httplib::Request& req, httplib::Response& res,
                        const std::string& collection);
    void handle_explain(const httplib::Request& req, httplib::Response& res,
                         const std::string& collection);
    void handle_hybrid_search(const httplib::Request& req, httplib::Response& res,
                               const std::string& collection);
    void handle_recall(const httplib::Request& req, httplib::Response& res,
                        const std::string& collection);

    // ---- Stats / Index / IO ----
    void handle_stats(const httplib::Request& req, httplib::Response& res,
                       const std::string& collection);
    void handle_index_action(const httplib::Request& req, httplib::Response& res,
                              const std::string& name);
    void handle_export(const httplib::Request& req, httplib::Response& res,
                        const std::string& name);
    void handle_import(const httplib::Request& req, httplib::Response& res,
                        const std::string& name);

    // ---- Snapshot / Health / Metrics ----
    void handle_snapshot(const httplib::Request& req, httplib::Response& res);
    void handle_restore(const httplib::Request& req, httplib::Response& res);
    void handle_health(const httplib::Request& req, httplib::Response& res);
    void handle_ready(const httplib::Request& req, httplib::Response& res);
    void handle_metrics(const httplib::Request& req, httplib::Response& res);

    // ---- Web console ----
    void handle_root(const httplib::Request& req, httplib::Response& res);

    // ---- Members ----
    std::unique_ptr<VectorDatabase> db_;
    Config config_;
    httplib::Server svr_;
    std::unique_ptr<httplib::ThreadPool> thread_pool_;
    std::atomic<bool> running_{false};
};

} // namespace flux
