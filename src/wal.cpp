#include "wal.h"
#include <spdlog/spdlog.h>
#include <sys/stat.h>

namespace flux {

// ============================================================
// Simple CRC32 (IEEE 802.3 polynomial)
// ============================================================

static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

static void init_crc32_table() {
    if (crc32_table_initialized) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
        }
        crc32_table[i] = crc;
    }
    crc32_table_initialized = true;
}

static uint32_t crc32_compute(const uint8_t* data, size_t len) {
    init_crc32_table();
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFFUL;
}

// ============================================================
// WAL
// ============================================================

WAL::WAL(const std::string& path) : path_(path) {
    file_.open(path, std::ios::in | std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        // Create if doesn't exist
        file_.clear();
        file_.open(path, std::ios::out);
        file_.close();
        file_.open(path, std::ios::in | std::ios::out | std::ios::app);
        if (!file_.is_open()) {
            throw std::runtime_error("wal: cannot open " + path);
        }
    }
}

WAL::~WAL() {
    close();
}

void WAL::append(WALEvent event) {
    std::lock_guard lock(mu_);

    // Compute CRC32
    event.crc32 = 0;
    nlohmann::json j = event;
    std::string data = j.dump();
    uint32_t crc = crc32_compute(
        reinterpret_cast<const uint8_t*>(data.data()), data.size());
    event.crc32 = crc;

    // Marshal with checksum
    j = event;
    std::string line = j.dump() + "\n";

    file_ << line;
    file_.flush();
}

void WAL::truncate() {
    std::lock_guard lock(mu_);
    file_.close();
    file_.open(path_, std::ios::out | std::ios::trunc);
    file_.close();
    file_.open(path_, std::ios::in | std::ios::out | std::ios::app);
}

void WAL::close() {
    std::lock_guard lock(mu_);
    if (file_.is_open()) {
        file_.close();
    }
}

bool WAL::replay(WALHandler handler) {
    std::lock_guard lock(mu_);

    // Seek to beginning
    file_.seekg(0, std::ios::beg);

    std::string line;
    int line_number = 0;

    while (std::getline(file_, line)) {
        line_number++;
        if (line.empty()) continue;

        try {
            auto j = nlohmann::json::parse(line);

            // Verify checksum
            if (j.contains("crc32") && j["crc32"].get<uint32_t>() != 0) {
                uint32_t saved_crc = j["crc32"];
                j["crc32"] = 0;
                std::string clean_data = j.dump();
                uint32_t computed_crc = crc32_compute(
                    reinterpret_cast<const uint8_t*>(clean_data.data()), clean_data.size());
                if (saved_crc != computed_crc) {
                    spdlog::error("WAL replay line {}: checksum mismatch", line_number);
                    return false;
                }
                j["crc32"] = saved_crc;
            }

            WALEvent event = j;
            if (!handler(event)) return false;

        } catch (const std::exception& e) {
            spdlog::error("WAL replay line {}: {}", line_number, e.what());
            return false;
        }
    }

    // Reset EOF flag and seek back to end for appending
    file_.clear();
    file_.seekp(0, std::ios::end);

    return true;
}

uint32_t WAL::compute_crc32(const std::string& data) const {
    return crc32_compute(
        reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

} // namespace flux
