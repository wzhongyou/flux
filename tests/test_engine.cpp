#include <gtest/gtest.h>

#include "database.h"
#include "distance.h"
#include "schema.h"
#include "filter.h"
#include "bm25.h"
#include "wal.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstdio>

using namespace flux;

// ============================================================
// Test fixtures
// ============================================================

class EngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        db = std::make_unique<VectorDatabase>("");
    }
    void TearDown() override {
        db.reset();
        // Clean up test WAL files
        std::remove("/tmp/test_engine.wal");
    }

    void seed_docs(const std::string& collection, int n) {
        for (int i = 0; i < n; i++) {
            double x = static_cast<double>(i) / static_cast<double>(n);
            db->upsert(collection, Document{
                "d" + std::to_string(i),
                {x, 1.0 - x, 0.0},
                {{"seq", i}}
            });
        }
    }

    std::unique_ptr<VectorDatabase> db;
};

// ============================================================
// Collection management
// ============================================================

TEST_F(EngineTest, CreateCollection) {
    db->create_collection("test", DistanceMetric::Cosine);

    EXPECT_THROW(db->create_collection("test", DistanceMetric::Cosine), std::invalid_argument);

    auto names = db->list_collections();
    ASSERT_EQ(names.size(), 1);
    EXPECT_EQ(names[0], "test");
}

TEST_F(EngineTest, CreateCollectionWithSchema) {
    auto schema = std::make_unique<Schema>(Schema{{
        {"tag", FieldType::String, true},
        {"price", FieldType::Float},
        {"active", FieldType::Bool},
        {"title", FieldType::Text},
    }});

    db->create_collection_with_schema("typed", DistanceMetric::Cosine, std::move(schema));

    // Valid upsert
    EXPECT_NO_THROW(db->upsert("typed", Document{
        "1", {1.0, 0.0, 0.0},
        {{"tag", "news"}, {"price", 10.5}, {"active", true}, {"title", "hello"}}
    }));

    // Invalid type
    EXPECT_THROW(db->upsert("typed", Document{
        "2", {0.0, 1.0, 0.0},
        {{"tag", 123}}
    }), std::invalid_argument);
}

TEST_F(EngineTest, DeleteCollection) {
    db->create_collection("test");
    seed_docs("test", 10);

    EXPECT_NO_THROW(db->delete_collection("test"));
    EXPECT_THROW(db->collection_stats("test"), std::invalid_argument);
}

TEST_F(EngineTest, TruncateCollection) {
    db->create_collection("test");
    seed_docs("test", 10);

    db->truncate_collection("test");
    auto stats = db->collection_stats("test");
    EXPECT_EQ(stats.doc_count, 0);
}

// ============================================================
// Document operations
// ============================================================

TEST_F(EngineTest, UpsertAndDelete) {
    db->create_collection("test", DistanceMetric::Cosine);

    db->upsert("test", Document{"1", {1.0, 0.0, 0.0}});
    db->upsert("test", Document{"1", {0.0, 1.0, 0.0}}); // update

    db->remove("test", "1");
    db->remove("test", "nonexistent"); // should not throw
}

TEST_F(EngineTest, BatchOperations) {
    db->create_collection("test", DistanceMetric::Cosine);

    std::vector<Document> docs;
    for (int i = 0; i < 10; i++) {
        docs.push_back(Document{
            "d" + std::to_string(i),
            {static_cast<double>(i) / 10.0, 1.0 - static_cast<double>(i) / 10.0, 0.0}
        });
    }

    int count = db->batch_upsert("test", docs);
    EXPECT_EQ(count, 10);

    auto stats = db->collection_stats("test");
    EXPECT_EQ(stats.doc_count, 10);

    count = db->batch_remove("test", {"d0", "d1", "d2"});
    EXPECT_EQ(count, 3);

    stats = db->collection_stats("test");
    EXPECT_EQ(stats.doc_count, 7);
}

TEST_F(EngineTest, UpsertInvalidDimensions) {
    db->create_collection("test", DistanceMetric::Cosine);
    db->upsert("test", Document{"a", {1.0, 0.0, 0.0}});

    EXPECT_THROW(db->upsert("test", Document{"b", {1.0, 0.0}}), std::invalid_argument);
}

// ============================================================
// Brute force search
// ============================================================

TEST_F(EngineTest, BruteForceSearch) {
    db->create_collection("test", DistanceMetric::Cosine);
    seed_docs("test", 20);

    auto results = db->search("test", {1.0, 0.0, 0.0}, 5);
    ASSERT_EQ(results.size(), 5);

    // Results should be sorted by score descending
    for (size_t i = 1; i < results.size(); i++) {
        EXPECT_GE(results[i - 1].score, results[i].score);
    }

    // First result should have doc_ptr populated
    EXPECT_NE(results[0].doc_ptr, nullptr);
}

TEST_F(EngineTest, BruteForceSearchEmptyCollection) {
    db->create_collection("empty", DistanceMetric::Cosine);
    EXPECT_NO_THROW({
        auto results = db->search("empty", {1.0, 0.0, 0.0}, 5);
        EXPECT_TRUE(results.empty());
    });
}

TEST_F(EngineTest, SearchInvalidK) {
    db->create_collection("test");
    EXPECT_THROW(db->search("test", {1.0, 0.0}, 0), std::invalid_argument);
}

// ============================================================
// Filtered search
// ============================================================

TEST_F(EngineTest, SearchWithEqualityFilter) {
    db->create_collection("test", DistanceMetric::Cosine);
    db->upsert("test", Document{"a1", {1.0, 0.0, 0.0}, {{"tag", "news"}, {"val", 10}}});
    db->upsert("test", Document{"a2", {0.0, 1.0, 0.0}, {{"tag", "blog"}, {"val", 20}}});
    db->upsert("test", Document{"a3", {0.9, 0.1, 0.0}, {{"tag", "news"}, {"val", 15}}});

    auto results = db->search("test", {1.0, 0.0, 0.0}, 5, FieldEqual("tag", "news"));
    EXPECT_EQ(results.size(), 2);
}

TEST_F(EngineTest, SearchWithRangeFilter) {
    db->create_collection("test", DistanceMetric::Cosine);
    db->upsert("test", Document{"a1", {1.0, 0.0, 0.0}, {{"val", 10}}});
    db->upsert("test", Document{"a2", {0.0, 1.0, 0.0}, {{"val", 20}}});
    db->upsert("test", Document{"a3", {0.9, 0.1, 0.0}, {{"val", 15}}});

    auto results = db->search("test", {1.0, 0.0, 0.0}, 5, FieldRange("val", 12, 20));
    EXPECT_EQ(results.size(), 2);
}

TEST_F(EngineTest, SearchWithCompositeFilter) {
    db->create_collection("test", DistanceMetric::Cosine);
    db->upsert("test", Document{"a1", {1.0, 0.0, 0.0}, {{"tag", "news"}, {"val", 10}}});
    db->upsert("test", Document{"a2", {0.0, 1.0, 0.0}, {{"tag", "blog"}, {"val", 20}}});
    db->upsert("test", Document{"a3", {0.9, 0.1, 0.0}, {{"tag", "news"}, {"val", 15}}});

    auto results = db->search("test", {1.0, 0.0, 0.0}, 5,
        And({FieldEqual("tag", "news"), FieldRange("val", 12, 20)}));
    EXPECT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].id, "a3");
}

TEST_F(EngineTest, SearchWithInFilter) {
    db->create_collection("test", DistanceMetric::Cosine);
    for (int i = 0; i < 5; i++) {
        db->upsert("test", Document{
            "d" + std::to_string(i),
            {static_cast<double>(i) / 5.0, 1.0 - static_cast<double>(i) / 5.0, 0.0},
            {{"group", "g" + std::to_string(i % 3)}}
        });
    }

    auto results = db->search("test", {1.0, 0.0, 0.0}, 10,
        FieldIn("group", {nlohmann::json("g0"), nlohmann::json("g1")}));
    EXPECT_EQ(results.size(), 4);
}

// ============================================================
// BM25 full-text index
// ============================================================

TEST(BM25Test, Tokenize) {
    auto tokens = bm25_tokenize("Hello, World! AI in 2025.");
    std::vector<std::string> expected = {"hello", "world", "ai", "in", "2025"};
    EXPECT_EQ(tokens, expected);
}

TEST(BM25Test, EmptyText) {
    auto tokens = bm25_tokenize("");
    EXPECT_TRUE(tokens.empty());
}

TEST(BM25Test, IndexAndScore) {
    BM25Index idx;
    idx.index_document("d1", "vector database engine");
    idx.index_document("d2", "database query optimization");
    idx.index_document("d3", "machine learning with vector embeddings");

    EXPECT_EQ(idx.doc_count(), 3);

    auto scores = idx.score("vector database");
    EXPECT_FALSE(scores.empty());

    // "d1" should rank highest (has both "vector" and "database")
    double d1_score = scores["d1"];
    double d2_score = scores["d2"];
    double d3_score = scores["d3"];
    EXPECT_GT(d1_score, d2_score);
    EXPECT_GT(d1_score, d3_score);
}

TEST(BM25Test, RemoveAndClear) {
    BM25Index idx;
    idx.index_document("d1", "hello world");
    idx.index_document("d2", "hello vector");

    EXPECT_EQ(idx.doc_count(), 2);

    idx.remove_document("d1");
    EXPECT_EQ(idx.doc_count(), 1);

    // "hello" should still be indexed (d2)
    auto scores = idx.score("hello");
    EXPECT_EQ(scores.size(), 1);

    idx.clear();
    EXPECT_EQ(idx.doc_count(), 0);
}

// ============================================================
// Hybrid Search
// ============================================================

TEST_F(EngineTest, HybridSearchWithBM25) {
    auto schema = std::make_unique<Schema>(Schema{{
        {"title", FieldType::Text, true},
    }});
    db->create_collection_with_schema("docs", DistanceMetric::Cosine, std::move(schema));

    db->upsert("docs", Document{"d1", {1.0, 0.0, 0.0}, {{"title", "vector database engine"}}});
    db->upsert("docs", Document{"d2", {0.0, 1.0, 0.0}, {{"title", "SQL database query"}}});
    db->upsert("docs", Document{"d3", {0.7, 0.3, 0.0}, {{"title", "machine learning with vectors"}}});

    auto results = db->hybrid_search("docs", {0.9, 0.1, 0.0}, "vector database", 3, 0.5);
    ASSERT_GE(results.size(), 1);

    // d1 should rank high (matches both vector and text)
    EXPECT_EQ(results[0].id, "d1");
    EXPECT_GT(results[0].combined_score, 0.0);
}

// ============================================================
// WAL persistence
// ============================================================

TEST(WALTest, AppendAndReplay) {
    const std::string path = "/tmp/test_flux.wal";
    std::remove(path.c_str());

    {
        WAL wal(path);

        WALEvent ev;
        ev.action = "create_collection";
        ev.collection = "test";
        ev.metric = "cosine";
        wal.append(ev);

        WALEvent ev2;
        ev2.action = "upsert";
        ev2.collection = "test";
        ev2.document = Document{"d1", {1.0, 0.5}, {{"tag", "news"}}};
        wal.append(ev2);
    }

    // Replay
    {
        WAL wal(path);
        std::vector<WALEvent> events;
        bool ok = wal.replay([&](const WALEvent& ev) {
            events.push_back(ev);
            return true;
        });
        EXPECT_TRUE(ok);
        EXPECT_EQ(events.size(), 2);
        EXPECT_EQ(events[0].action, "create_collection");
        EXPECT_EQ(events[1].action, "upsert");
    }

    std::remove(path.c_str());
}

// Helper: seed docs into a specific db instance
static void seed_docs_using_db(VectorDatabase& db, const std::string& collection, int n) {
    for (int i = 0; i < n; i++) {
        double x = static_cast<double>(i) / static_cast<double>(n);
        db.upsert(collection, Document{
            "d" + std::to_string(i),
            {x, 1.0 - x, 0.0}
        });
    }
}

TEST_F(EngineTest, WALRoundTrip) {
    const std::string wal_path = "/tmp/test_engine.wal";
    std::remove(wal_path.c_str());

    // Write
    {
        VectorDatabase db1(wal_path);
        db1.create_collection("test", DistanceMetric::Cosine);
        seed_docs_using_db(db1, "test", 5);
    }

    // Read back
    {
        VectorDatabase db2(wal_path);
        auto names = db2.list_collections();
        ASSERT_EQ(names.size(), 1);
        EXPECT_EQ(names[0], "test");

        auto stats = db2.collection_stats("test");
        EXPECT_EQ(stats.doc_count, 5);
        EXPECT_EQ(stats.dimension, 3);
        EXPECT_EQ(stats.metric, DistanceMetric::Cosine);
    }

    std::remove(wal_path.c_str());
}

// ============================================================
// Distance accuracy with engine
// ============================================================

TEST_F(EngineTest, CosineAccuracy) {
    db->create_collection("test", DistanceMetric::Cosine);
    db->upsert("test", Document{"ref", {1.0, 0.0, 0.0}});
    db->upsert("test", Document{"near", {0.99, 0.01, 0.0}});
    db->upsert("test", Document{"far", {-1.0, 0.0, 0.0}});

    auto results = db->search("test", {1.0, 0.0, 0.0}, 3);
    ASSERT_EQ(results.size(), 3);

    EXPECT_EQ(results[0].id, "ref");
    EXPECT_NEAR(results[0].score, 1.0, 1e-6);
    EXPECT_GT(results[1].score, results[2].score);
}

// ============================================================
// Multiple collections
// ============================================================

TEST_F(EngineTest, MultipleCollections) {
    db->create_collection("a", DistanceMetric::Cosine);
    db->create_collection("b", DistanceMetric::Euclidean);

    db->upsert("a", Document{"x", {1.0, 0.0}});
    db->upsert("b", Document{"y", {0.0, 1.0}});

    auto names = db->list_collections();
    EXPECT_EQ(names.size(), 2);

    EXPECT_EQ(db->collection_stats("a").doc_count, 1);
    EXPECT_EQ(db->collection_stats("b").doc_count, 1);
    EXPECT_EQ(db->collection_stats("a").metric, DistanceMetric::Cosine);
    EXPECT_EQ(db->collection_stats("b").metric, DistanceMetric::Euclidean);
}
