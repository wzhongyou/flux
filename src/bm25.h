#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>

namespace flux {

// ============================================================
// BM25 Index — BM25Okapi full-text search
// (mirrors Go bm25.go)
// ============================================================

class BM25Index {
public:
    static constexpr double kDefaultK1 = 1.2;
    static constexpr double kDefaultB  = 0.75;

    explicit BM25Index(double k1 = kDefaultK1, double b = kDefaultB);

    // Index a document's text content. Re-indexes if doc_id already exists.
    void index_document(const std::string& doc_id, const std::string& text);

    // Remove a document from the index.
    void remove_document(const std::string& doc_id);

    // Score all documents against a query string.
    // Returns map of doc_id → BM25 score.
    std::unordered_map<std::string, double> score(const std::string& query) const;

    // Number of indexed documents.
    int doc_count() const;

    // Remove all documents.
    void clear();

private:
    void remove_document_locked(const std::string& doc_id);
    void update_avg_doc_len();

    // Tokenize text into lowercase terms.
    static std::vector<std::string> tokenize(const std::string& text);

    mutable std::shared_mutex mu_;
    double avg_doc_len_ = 0.0;
    int total_docs_ = 0;

    std::unordered_map<std::string, int> doc_lengths_;                // doc_id → total term count
    std::unordered_map<std::string,
        std::unordered_map<std::string, int>> term_freq_;            // term → doc_id → frequency
    std::unordered_map<std::string, int> doc_freq_;                  // term → document count

    double k1_;
    double b_;
};

// Public tokenize function (exposed for testing).
std::vector<std::string> bm25_tokenize(const std::string& text);

} // namespace flux
