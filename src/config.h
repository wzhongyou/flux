#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace flux {

// ============================================================
// Server configuration (maps to Go ServerConfig)
// ============================================================
struct ServerConfig {
    std::string addr = ":9876";
    int read_timeout = 30;
    int write_timeout = 30;
    std::vector<std::string> api_keys;
    std::vector<std::string> cors_origins;
    int max_concurrent = 100;
    std::string tls_key_file;
    std::string tls_cert_file;

    bool tls_enabled() const {
        return !tls_cert_file.empty() && !tls_key_file.empty();
    }
};

// ============================================================
// Database configuration (maps to Go DatabaseConfig)
// ============================================================
struct DatabaseConfig {
    std::string wal_path = "flux.wal";
    std::string snapshot_path;
    int snapshot_interval = 0; // seconds, 0 = disabled
};

// ============================================================
// Logging configuration
// ============================================================
struct LoggingConfig {
    std::string level = "info";
    bool pretty = false;
};

// ============================================================
// Full configuration
// ============================================================
struct Config {
    ServerConfig server;
    DatabaseConfig database;
    LoggingConfig logging;

    static Config default_config();
};

// Load from JSON file (with environment variable overrides).
Config load_config(const std::string& path);

// Apply FLUX_* environment variable overrides.
void apply_env_overrides(Config& cfg);

} // namespace flux
