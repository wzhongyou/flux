#include <gtest/gtest.h>

#include "config.h"
#include "types.h"

using namespace flux;

TEST(ConfigTest, DefaultConfig) {
    auto cfg = Config::default_config();
    EXPECT_EQ(cfg.server.addr, ":9876");
    EXPECT_EQ(cfg.server.max_concurrent, 100);
    EXPECT_EQ(cfg.server.read_timeout, 30);
    EXPECT_EQ(cfg.database.wal_path, "flux.wal");
    EXPECT_EQ(cfg.database.snapshot_interval, 0);
    EXPECT_FALSE(cfg.server.tls_enabled());
}

TEST(ConfigTest, TLSEnabled) {
    auto cfg = Config::default_config();
    EXPECT_FALSE(cfg.server.tls_enabled());

    cfg.server.tls_cert_file = "/tmp/cert.pem";
    cfg.server.tls_key_file = "/tmp/key.pem";
    EXPECT_TRUE(cfg.server.tls_enabled());
}

TEST(ConfigTest, MetricFromString) {
    EXPECT_EQ(metric_from_string("cosine"), DistanceMetric::Cosine);
    EXPECT_EQ(metric_from_string("l2"), DistanceMetric::Euclidean);
    EXPECT_EQ(metric_from_string("euclidean"), DistanceMetric::Euclidean);
    EXPECT_EQ(metric_from_string("ip"), DistanceMetric::InnerProduct);
    EXPECT_EQ(metric_from_string("inner_product"), DistanceMetric::InnerProduct);

    EXPECT_THROW(metric_from_string("unknown"), std::invalid_argument);
}

TEST(ConfigTest, MetricToString) {
    EXPECT_STREQ(metric_to_string(DistanceMetric::Cosine), "cosine");
    EXPECT_STREQ(metric_to_string(DistanceMetric::Euclidean), "l2");
    EXPECT_STREQ(metric_to_string(DistanceMetric::InnerProduct), "ip");
}
