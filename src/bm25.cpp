#include "bm25.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <unordered_set>

namespace flux {

// ============================================================
// Tokenizer
// ============================================================

std::vector<std::string> bm25_tokenize(const std::string& text) {
    std::vector<std::string> terms;
    std::string current;

    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            current += static_cast<char>(std::tolower(c));
        } else {
            if (!current.empty()) {
                terms.push_back(std::move(current));
                current.clear();
            }
        }
    }
    if (!current.empty()) {
        terms.push_back(std::move(current));
    }
    return terms;
}

// ============================================================
// BM25Index
// ============================================================

BM25Index::BM25Index(double k1, double b) : k1_(k1), b_(b) {}

void BM25Index::index_document(const std::string& doc_id, const std::string& text) {
    std::unique_lock lock(mu_);

    // Remove old entry if exists
    if (doc_lengths_.count(doc_id)) {
        remove_document_locked(doc_id);
    }

    auto terms = bm25_tokenize(text);
    if (terms.empty()) return;

    doc_lengths_[doc_id] = static_cast<int>(terms.size());

    // Count term frequencies within this document
    std::unordered_map<std::string, int> tf;
    for (const auto& term : terms) {
        tf[term]++;
    }

    for (const auto& [term, count] : tf) {
        term_freq_[term][doc_id] = count;
        doc_freq_[term]++;
    }

    total_docs_++;
    update_avg_doc_len();
}

void BM25Index::remove_document(const std::string& doc_id) {
    std::unique_lock lock(mu_);
    remove_document_locked(doc_id);
}

void BM25Index::remove_document_locked(const std::string& doc_id) {
    if (!doc_lengths_.count(doc_id)) return;

    // Remove term frequencies for this document
    for (auto& [term, doc_map] : term_freq_) {
        if (doc_map.count(doc_id)) {
            doc_map.erase(doc_id);
            doc_freq_[term]--;
            if (doc_freq_[term] <= 0) {
                doc_freq_.erase(term);
            }
        }
    }

    // Clean empty term entries
    auto term_it = term_freq_.begin();
    while (term_it != term_freq_.end()) {
        if (term_it->second.empty()) {
            term_it = term_freq_.erase(term_it);
        } else {
            ++term_it;
        }
    }

    doc_lengths_.erase(doc_id);
    total_docs_--;

    // Recalculate average
    if (total_docs_ > 0) {
        int total_len = 0;
        for (const auto& [_, len] : doc_lengths_) total_len += len;
        avg_doc_len_ = static_cast<double>(total_len) / total_docs_;
    } else {
        avg_doc_len_ = 0.0;
    }
}

std::unordered_map<std::string, double> BM25Index::score(const std::string& query) const {
    std::shared_lock lock(mu_);
    if (total_docs_ == 0) return {};

    auto query_terms = bm25_tokenize(query);
    if (query_terms.empty()) return {};

    // Deduplicate query terms
    std::unordered_set<std::string> seen;
    std::vector<std::string> unique_terms;
    for (const auto& t : query_terms) {
        if (seen.insert(t).second) {
            unique_terms.push_back(t);
        }
    }

    double N = static_cast<double>(total_docs_);
    std::unordered_map<std::string, double> scores;

    for (const auto& term : unique_terms) {
        auto df_it = doc_freq_.find(term);
        if (df_it == doc_freq_.end()) continue;

        int df = df_it->second;
        double idf = std::log(1.0 + (N - df + 0.5) / (df + 0.5));

        const auto& doc_map = term_freq_.at(term);
        for (const auto& [doc_id, tf] : doc_map) {
            double doc_len = static_cast<double>(doc_lengths_.at(doc_id));
            double denom = tf + k1_ * (1.0 - b_ + b_ * doc_len / avg_doc_len_);
            double term_score = idf * (tf * (k1_ + 1.0) / denom);
            scores[doc_id] += term_score;
        }
    }

    return scores;
}

int BM25Index::doc_count() const {
    std::shared_lock lock(mu_);
    return total_docs_;
}

void BM25Index::clear() {
    std::unique_lock lock(mu_);
    doc_lengths_.clear();
    term_freq_.clear();
    doc_freq_.clear();
    total_docs_ = 0;
    avg_doc_len_ = 0.0;
}

void BM25Index::update_avg_doc_len() {
    int total_len = 0;
    for (const auto& [_, len] : doc_lengths_) total_len += len;
    avg_doc_len_ = static_cast<double>(total_len) / total_docs_;
}

} // namespace flux
