#pragma once

#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <mutex>
#include <fstream>

namespace flux {

// ============================================================
// WAL — Write-Ahead Log (mirrors Go wal.go)
//
// JSON Lines format with CRC32 checksum.
// ============================================================

// A single WAL event. Mirrors Go walEvent struct.
struct WALEvent {
    std::string action;
    std::string collection;
    std::string metric;        // for create_collection
    nlohmann::json document;   // for upsert (single)
    nlohmann::json documents;  // for batch_upsert (array)
    std::string id;            // for delete (single)
    nlohmann::json ids;        // for batch_delete (array)
    uint32_t crc32 = 0;

    // JSON serialization
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(WALEvent,
        action, collection, metric, document, documents, id, ids, crc32)
};

// WAL replay handler: called for each event during recovery.
// Return false to stop replay (on error).
using WALHandler = std::function<bool(const WALEvent& event)>;

class WAL {
public:
    explicit WAL(const std::string& path);
    ~WAL();

    WAL(const WAL&) = delete;
    WAL& operator=(const WAL&) = delete;

    // Append a single event to the WAL. Thread-safe.
    void append(WALEvent event);

    // Truncate the WAL file to zero length (after snapshot).
    void truncate();

    // Close the WAL.
    void close();

    // Replay all events. Thread-safe (blocks writes).
    bool replay(WALHandler handler);

private:
    std::string path_;
    std::fstream file_;
    std::mutex mu_;

    uint32_t compute_crc32(const std::string& data) const;
};

} // namespace flux
