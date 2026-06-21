#pragma once

#include "database.h"
#include <iostream>
#include <string>

namespace flux {

// Export collection as JSON Lines (NDJSON) to a stream.
// Returns number of documents exported.
int export_collection_jsonl(const VectorDatabase& db,
                            const std::string& collection_name,
                            std::ostream& os);

// Import collection from JSON Lines (NDJSON) stream.
// Returns number of documents imported.
int import_collection_jsonl(VectorDatabase& db,
                            const std::string& collection_name,
                            std::istream& is);

// Snapshot entire database to a JSON file.
void snapshot_to_file(const VectorDatabase& db, const std::string& path);

// Restore database from a JSON snapshot file.
void restore_from_file(VectorDatabase& db, const std::string& path);

} // namespace flux
