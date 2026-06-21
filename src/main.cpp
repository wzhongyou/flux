#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <csignal>

#include "config.h"
#include "database.h"
#include "server.h"
#include "io.h"

std::function<void()> shutdown_hook;

static void signal_handler(int) {
    if (shutdown_hook) shutdown_hook();
}

int main(int argc, char* argv[]) {
    std::string config_path;
    std::string addr_override;
    std::string wal_override;

    CLI::App app{"Flux — Vector database recall engine (v2.0.0, C++17)"};
    app.add_option("-c,--config", config_path, "Path to config file");
    app.add_option("-a,--addr", addr_override, "HTTP listen address");
    app.add_option("-w,--wal", wal_override, "WAL file path");

    CLI11_PARSE(app, argc, argv);

    // ---- Load configuration ----
    flux::Config cfg;
    try {
        cfg = flux::load_config(config_path);
    } catch (const std::exception& e) {
        spdlog::error("Failed to load config: {}", e.what());
        return 1;
    }

    if (!addr_override.empty()) cfg.server.addr = addr_override;
    if (!wal_override.empty())  cfg.database.wal_path = wal_override;

    // ---- Setup logging ----
    spdlog::set_level(spdlog::level::from_str(cfg.logging.level));
    spdlog::info("Flux v2.0.0 (C++17) starting...");

    // ---- Initialize database engine ----
    auto db = std::make_unique<flux::VectorDatabase>(cfg.database.wal_path);
    spdlog::info("Database engine initialized, wal={}", cfg.database.wal_path);

    // ---- Load snapshot if configured ----
    if (!cfg.database.snapshot_path.empty()) {
        try {
            flux::restore_from_file(*db, cfg.database.snapshot_path);
            spdlog::info("Loaded snapshot from {}", cfg.database.snapshot_path);
        } catch (const std::exception& e) {
            spdlog::warn("Could not load snapshot: {}", e.what());
        }
    }

    // ---- Start HTTP server ----
    flux::Server server(std::move(db), cfg);

    // Graceful shutdown on SIGINT/SIGTERM
    shutdown_hook = [&server]() {
        spdlog::info("Shutting down...");
        server.shutdown();
    };
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    server.run();
    spdlog::info("Server stopped");
    return 0;
}
