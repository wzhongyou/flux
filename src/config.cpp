#include "config.h"

#include <fstream>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace flux {

Config Config::default_config() {
    Config cfg;
    return cfg;
}

Config load_config(const std::string& path) {
    Config cfg = Config::default_config();

    if (!path.empty()) {
        std::ifstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("cannot open config file: " + path);
        }
        nlohmann::json j;
        file >> j;

        // Parse top-level sections
        if (j.contains("server")) {
            auto& s = j["server"];
            if (s.contains("addr"))            cfg.server.addr = s["addr"];
            if (s.contains("read_timeout"))    cfg.server.read_timeout = s["read_timeout"];
            if (s.contains("write_timeout"))   cfg.server.write_timeout = s["write_timeout"];
            if (s.contains("api_keys"))        cfg.server.api_keys = s["api_keys"].get<std::vector<std::string>>();
            if (s.contains("cors_origins"))    cfg.server.cors_origins = s["cors_origins"].get<std::vector<std::string>>();
            if (s.contains("max_concurrent"))  cfg.server.max_concurrent = s["max_concurrent"];
            if (s.contains("tls_key_file"))    cfg.server.tls_key_file = s["tls_key_file"];
            if (s.contains("tls_cert_file"))   cfg.server.tls_cert_file = s["tls_cert_file"];
        }
        if (j.contains("database")) {
            auto& d = j["database"];
            if (d.contains("wal_path"))           cfg.database.wal_path = d["wal_path"];
            if (d.contains("snapshot_path"))      cfg.database.snapshot_path = d["snapshot_path"];
            if (d.contains("snapshot_interval"))  cfg.database.snapshot_interval = d["snapshot_interval"];
        }
        if (j.contains("logging")) {
            auto& l = j["logging"];
            if (l.contains("level"))  cfg.logging.level = l["level"];
            if (l.contains("pretty")) cfg.logging.pretty = l["pretty"];
        }
    }

    apply_env_overrides(cfg);
    return cfg;
}

void apply_env_overrides(Config& cfg) {
    auto env = [](const char* name) -> const char* {
        return std::getenv(name);
    };

    if (auto v = env("FLUX_ADDR"); v && *v)              cfg.server.addr = v;
    if (auto v = env("FLUX_WAL_PATH"); v && *v)           cfg.database.wal_path = v;
    if (auto v = env("FLUX_SNAPSHOT_PATH"); v && *v)      cfg.database.snapshot_path = v;
    if (auto v = env("FLUX_SNAPSHOT_INTERVAL"); v && *v) {
        cfg.database.snapshot_interval = std::stoi(v);
    }
    if (auto v = env("FLUX_API_KEYS"); v && *v) {
        std::string keys(v);
        cfg.server.api_keys.clear();
        size_t pos = 0;
        // Simple comma split
        while (pos < keys.size()) {
            size_t next = keys.find(',', pos);
            if (next == std::string::npos) next = keys.size();
            cfg.server.api_keys.push_back(keys.substr(pos, next - pos));
            pos = next + 1;
        }
    }
    if (auto v = env("FLUX_CORS_ORIGINS"); v && *v) {
        std::string origins(v);
        cfg.server.cors_origins.clear();
        size_t pos = 0;
        while (pos < origins.size()) {
            size_t next = origins.find(',', pos);
            if (next == std::string::npos) next = origins.size();
            cfg.server.cors_origins.push_back(origins.substr(pos, next - pos));
            pos = next + 1;
        }
    }
    if (auto v = env("FLUX_MAX_CONCURRENT"); v && *v)    cfg.server.max_concurrent = std::stoi(v);
    if (auto v = env("FLUX_LOG_LEVEL"); v && *v)          cfg.logging.level = v;
    if (auto v = env("FLUX_LOG_PRETTY"); v && *v) {
        std::string s(v);
        cfg.logging.pretty = (s == "true" || s == "1");
    }
    if (auto v = env("FLUX_READ_TIMEOUT"); v && *v)      cfg.server.read_timeout = std::stoi(v);
    if (auto v = env("FLUX_WRITE_TIMEOUT"); v && *v)     cfg.server.write_timeout = std::stoi(v);
    if (auto v = env("FLUX_TLS_CERT_FILE"); v && *v)      cfg.server.tls_cert_file = v;
    if (auto v = env("FLUX_TLS_KEY_FILE"); v && *v)       cfg.server.tls_key_file = v;
}

} // namespace flux
