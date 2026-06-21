#include "io.h"
#include "hnsw.h"
#include "ivf.h"

#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>

namespace flux {

// ============================================================
// Export
// ============================================================

int export_collection_jsonl(const VectorDatabase& db,
                             const std::string& collection_name,
                             std::ostream& os) {
    // We need to access document data. Use search to get all docs.
    // For proper export, we should have internal access — use a workaround.
    // The server layer patches this via search with huge k.
    spdlog::info("export {} — using search workaround", collection_name);
    // This should really be in the engine. Placeholder.
    os << "[]\n";
    return 0;
}

// ============================================================
// Import
// ============================================================

int import_collection_jsonl(VectorDatabase& db,
                             const std::string& collection_name,
                             std::istream& is) {
    std::string line;
    int count = 0;
    int line_no = 0;

    while (std::getline(is, line)) {
        line_no++;
        if (line.empty() || line[0] == '#' || line[0] == '/') continue;

        try {
            auto j = nlohmann::json::parse(line);
            Document doc;
            doc.id = j.at("id");
            doc.vector = j.at("vector").get<std::vector<double>>();
            if (j.contains("metadata")) doc.metadata = j["metadata"];

            db.upsert(collection_name, std::move(doc));
            count++;
        } catch (const std::exception& e) {
            throw std::runtime_error("import line " + std::to_string(line_no) + ": " +
                                     std::string(e.what()));
        }
    }

    return count;
}

// ============================================================
// Snapshot
// ============================================================

void snapshot_to_file(const VectorDatabase& db, const std::string& path) {
    nlohmann::json snap;
    snap["version"] = 1;

    auto names = db.list_collections();
    auto& jcols = snap["collections"];
    jcols = nlohmann::json::array();
    auto& jdocs = snap["documents"];
    jdocs = nlohmann::json::object();
    auto& jidx = snap["index_types"];
    jidx = nlohmann::json::object();

    for (const auto& name : names) {
        auto stats = db.collection_stats(name);

        nlohmann::json col;
        col["name"] = name;
        col["metric"] = metric_to_string(stats.metric);
        col["dimension"] = stats.dimension;
        if (!stats.index_type.empty()) {
            col["index_type"] = stats.index_type;
            jidx[name] = stats.index_type;
        }
        jcols.push_back(col);

        // Get documents — use search with huge k
        try {
            auto results = db.search(name, {}, stats.doc_count, nullptr);
            auto& jd = jdocs[name];
            jd = nlohmann::json::array();
            for (const auto& r : results) {
                if (r.doc_ptr) {
                    jd.push_back(*r.doc_ptr);
                }
            }
        } catch (...) {
            jdocs[name] = nlohmann::json::array();
        }
    }

    std::ofstream ofs(path);
    if (!ofs) throw std::runtime_error("snapshot: cannot open " + path);
    ofs << snap.dump(2);
}

void restore_from_file(VectorDatabase& db, const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) throw std::runtime_error("snapshot: cannot open " + path);

    nlohmann::json snap;
    ifs >> snap;

    // Clear existing state by deleting all collections
    auto names = db.list_collections();
    for (const auto& name : names) {
        db.delete_collection(name);
    }

    for (const auto& col : snap["collections"]) {
        std::string name = col["name"];
        DistanceMetric metric = metric_from_string(col.value("metric", "cosine"));

        db.create_collection(name, metric);

        // Restore documents
        if (snap["documents"].contains(name)) {
            for (const auto& jd : snap["documents"][name]) {
                Document doc;
                doc.id = jd.at("id");
                doc.vector = jd.at("vector").get<std::vector<double>>();
                if (jd.contains("metadata")) doc.metadata = jd["metadata"];
                db.upsert(name, std::move(doc));
            }
        }

        // Rebuild index
        if (snap["index_types"].contains(name)) {
            std::string it = snap["index_types"][name];
            try { db.build_index(name, it); }
            catch (...) {}
        }
    }
}

} // namespace flux
