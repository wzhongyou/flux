#include <gtest/gtest.h>

#include "database.h"
#include "hnsw.h"
#include "ivf.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

using namespace flux;

// ============================================================
// Test fixture
// ============================================================

class IndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        db = std::make_unique<VectorDatabase>("");
        db->create_collection("test", DistanceMetric::Cosine);
    }

    // Insert n random-ish 128-dim vectors
    void seed_docs(const std::string& collection, int n, int dim = 3) {
        std::mt19937 rng(42);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        for (int i = 0; i < n; i++) {
            std::vector<double> vec(dim);
            for (int d = 0; d < dim; d++) {
                vec[d] = dist(rng);
            }
            db->upsert(collection, Document{
                "d" + std::to_string(i), vec
            });
        }
    }

    // Compute recall: ANN results ∩ BF top-k / k
    double compute_recall(const std::vector<SearchResult>& ann,
                          const std::vector<SearchResult>& bf) {
        std::unordered_set<std::string> bf_ids;
        for (const auto& r : bf) bf_ids.insert(r.id);

        int hits = 0;
        for (const auto& r : ann) {
            if (bf_ids.count(r.id)) hits++;
        }
        if (bf.empty()) return 1.0;
        return static_cast<double>(hits) / bf.size();
    }

    std::unique_ptr<VectorDatabase> db;
};

// ============================================================
// HNSW tests
// ============================================================

TEST_F(IndexTest, HNSW_BuildAndSearch) {
    seed_docs("test", 100);
    db->build_index("test", "hnsw");

    auto results = db->search("test", {0.5, 0.5, 0.0}, 10);
    EXPECT_EQ(results.size(), 10);

    // Results sorted by score descending
    for (size_t i = 1; i < results.size(); i++) {
        EXPECT_GE(results[i - 1].score, results[i].score);
    }

    EXPECT_EQ(db->index_info("test"), "hnsw");
}

TEST_F(IndexTest, HNSW_RecallAtTen) {
    // Generate 100 random 128-dim vectors
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    // Create a separate DB for 128-dim testing
    VectorDatabase db2("");
    db2.create_collection("big", DistanceMetric::Cosine);

    for (int i = 0; i < 100; i++) {
        std::vector<double> vec(128);
        for (int d = 0; d < 128; d++) vec[d] = dist(rng);
        db2.upsert("big", Document{"d" + std::to_string(i), vec});
    }

    db2.build_index("big", "hnsw");

    // Test recall on 10 random queries
    double total_recall = 0.0;
    int queries = 10;

    for (int q = 0; q < queries; q++) {
        std::vector<double> query(128);
        for (int d = 0; d < 128; d++) query[d] = dist(rng);

        // ANN search
        auto ann_results = db2.search("big", query, 10);

        // Brute force reference (drop index, search, rebuild)
        db2.drop_index("big");
        auto bf_results = db2.search("big", query, 10);
        db2.build_index("big", "hnsw");

        total_recall += compute_recall(ann_results, bf_results);
    }

    double avg_recall = total_recall / queries;
    EXPECT_GE(avg_recall, 0.85) << "HNSW recall@10 should be >= 85%, got " << avg_recall;
}

TEST_F(IndexTest, HNSW_InsertAfterBuild) {
    seed_docs("test", 50);
    db->build_index("test", "hnsw");

    // Insert more documents after building
    for (int i = 50; i < 100; i++) {
        db->upsert("test", Document{
            "d" + std::to_string(i),
            {static_cast<double>(i % 17) / 17.0, 1.0 - static_cast<double>(i % 17) / 17.0, 0.0}
        });
    }

    auto results = db->search("test", {0.5, 0.5, 0.0}, 10);
    EXPECT_GE(results.size(), 1);
}

TEST_F(IndexTest, HNSW_DropIndex) {
    seed_docs("test", 10);
    db->build_index("test", "hnsw");

    EXPECT_EQ(db->index_info("test"), "hnsw");

    db->drop_index("test");
    EXPECT_EQ(db->index_info("test"), "");

    // Search should still work (brute force)
    auto results = db->search("test", {0.5, 0.5, 0.0}, 5);
    EXPECT_EQ(results.size(), 5);
}

// ============================================================
// IVF tests
// ============================================================

TEST_F(IndexTest, IVF_BuildAndSearch) {
    seed_docs("test", 100);
    db->build_index("test", "ivf");

    auto results = db->search("test", {0.5, 0.5, 0.0}, 5);
    EXPECT_GE(results.size(), 1);

    EXPECT_EQ(db->index_info("test"), "ivf");
}

TEST_F(IndexTest, IVF_RecallWithClusteredData) {
    // Generate vectors around 5 cluster centers — this is where IVF excels
    VectorDatabase db2("");
    db2.create_collection("clusters", DistanceMetric::Cosine);

    std::mt19937 rng(99);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    std::normal_distribution<double> noise(0.0, 0.2);

    // 5 cluster centers (128-dim random points)
    std::vector<std::vector<double>> centers(5);
    for (int c = 0; c < 5; c++) {
        centers[c].resize(128);
        for (int d = 0; d < 128; d++) centers[c][d] = dist(rng);
    }

    // 400 vectors total (80 per cluster)
    for (int c = 0; c < 5; c++) {
        for (int i = 0; i < 80; i++) {
            std::vector<double> vec(128);
            for (int d = 0; d < 128; d++) {
                vec[d] = centers[c][d] + noise(rng);
            }
            db2.upsert("clusters", Document{
                "c" + std::to_string(c) + "_d" + std::to_string(i), vec
            });
        }
    }

    // Build IVF with fewer centroids for clustered data
    db2.build_index("clusters", "ivf");

    double total_recall = 0.0;
    int query_count = 5;

    for (int q = 0; q < query_count; q++) {
        // Query near cluster center 0
        std::vector<double> query(128);
        for (int d = 0; d < 128; d++) query[d] = centers[q % 5][d] + noise(rng);

        auto ann = db2.search("clusters", query, 10);
        db2.drop_index("clusters");
        auto bf = db2.search("clusters", query, 10);
        db2.build_index("clusters", "ivf");

        total_recall += compute_recall(ann, bf);
    }

    double avg_recall = total_recall / query_count;
    // Clustered data + IVF: recall should be very good
    EXPECT_GE(avg_recall, 0.6) << "IVF recall@10 with clustered data: " << avg_recall;
}

// ============================================================
// Filtered index search
// ============================================================

TEST_F(IndexTest, HNSW_SearchWithFilter) {
    db->create_collection_with_schema("filtered", DistanceMetric::Cosine,
        std::make_unique<Schema>(Schema{{{ "tag", FieldType::String, true }}}));

    for (int i = 0; i < 50; i++) {
        db->upsert("filtered", Document{
            "d" + std::to_string(i),
            {static_cast<double>(i) / 50.0, 1.0 - static_cast<double>(i) / 50.0, 0.0},
            {{"tag", i % 3 == 0 ? "news" : "blog"}}
        });
    }

    db->build_index("filtered", "hnsw");

    auto results = db->search("filtered", {0.5, 0.5, 0.0}, 10, FieldEqual("tag", "news"));
    for (const auto& r : results) {
        EXPECT_EQ(r.doc_ptr->metadata["tag"], "news");
    }
}

// ============================================================
// HNSW standalone unit tests
// ============================================================

TEST(HNSWStandalone, InsertAndSearch) {
    HNSWIndex idx(DistanceMetric::Cosine, 3, HNSWIndex::Options{});
    idx.insert("a", {1.0, 0.0, 0.0});
    idx.insert("b", {0.0, 1.0, 0.0});
    idx.insert("c", {0.0, 0.0, 1.0});

    EXPECT_EQ(idx.size(), 3);

    auto results = idx.search_internal({1.0, 0.0, 0.0}, 2);
    EXPECT_EQ(results.size(), 2);
    EXPECT_EQ(results[0].id, "a");
    EXPECT_GT(results[0].score, results[1].score);
}

TEST(HNSWStandalone, Upsert) {
    HNSWIndex idx(DistanceMetric::Cosine, 3, HNSWIndex::Options{});
    idx.insert("a", {1.0, 0.0, 0.0});
    idx.insert("a", {0.0, 1.0, 0.0}); // update position

    auto results = idx.search_internal({0.0, 1.0, 0.0}, 1);
    EXPECT_EQ(results[0].id, "a");
}

TEST(HNSWStandalone, Delete) {
    HNSWIndex idx(DistanceMetric::Cosine, 3, HNSWIndex::Options{});
    idx.insert("a", {1.0, 0.0, 0.0});
    idx.insert("b", {0.0, 1.0, 0.0});
    idx.insert("c", {0.0, 0.0, 1.0});

    idx.remove("b");
    EXPECT_EQ(idx.size(), 2);

    auto results = idx.search_internal({0.0, 1.0, 0.0}, 3);
    // "b" should not appear
    for (const auto& r : results) {
        EXPECT_NE(r.id, "b");
    }
}

TEST(HNSWStandalone, FilteredSearch) {
    HNSWIndex idx(DistanceMetric::Cosine, 3, HNSWIndex::Options{});
    idx.insert("a", {1.0, 0.0, 0.0});
    idx.insert("b", {0.0, 1.0, 0.0});
    idx.insert("c", {0.0, 0.0, 1.0});

    std::unordered_set<std::string> candidates = {"a", "c"};
    auto results = idx.search_internal_with_filter({0.5, 0.5, 0.0}, 3, candidates);

    // Should only return a and c
    EXPECT_EQ(results.size(), 2);
}

// ============================================================
// IVF standalone unit tests
// ============================================================

TEST(IVFStandalone, InsertBuildAndSearch) {
    IVFIndex idx(DistanceMetric::Cosine, 3, IVFIndex::Options{});

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    for (int i = 0; i < 50; i++) {
        std::vector<double> v(3);
        for (int d = 0; d < 3; d++) v[d] = dist(rng);
        idx.insert("d" + std::to_string(i), v);
    }

    idx.build(); // train K-means

    EXPECT_EQ(idx.size(), 50);

    auto results = idx.search_internal({1.0, 0.0, 0.0}, 5);
    EXPECT_GE(results.size(), 1);
}

TEST(IVFStandalone, UpsertAndDelete) {
    IVFIndex idx(DistanceMetric::Cosine, 3, IVFIndex::Options{});

    for (int i = 0; i < 30; i++) {
        idx.insert("d" + std::to_string(i),
                   {static_cast<double>(i) / 30.0, 1.0 - static_cast<double>(i) / 30.0, 0.0});
    }
    idx.build();

    // Upsert
    idx.insert("d0", {0.0, 1.0, 0.0});

    // Delete
    idx.remove("d15");
    EXPECT_EQ(idx.size(), 29);
}
