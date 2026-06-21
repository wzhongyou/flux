#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include <random>

#include "distance.h"

using namespace flux;

// ============================================================
// Scalar building blocks
// ============================================================

TEST(DistanceTest, DotProduct_ZeroVector) {
    std::vector<double> a = {0.0, 0.0, 0.0};
    std::vector<double> b = {1.0, 2.0, 3.0};
    EXPECT_DOUBLE_EQ(dot_product(a.data(), b.data(), 3), 0.0);
}

TEST(DistanceTest, DotProduct_UnitVectors) {
    std::vector<double> a = {1.0, 0.0, 0.0};
    std::vector<double> b = {1.0, 0.0, 0.0};
    EXPECT_DOUBLE_EQ(dot_product(a.data(), b.data(), 3), 1.0);
}

TEST(DistanceTest, DotProduct_General) {
    std::vector<double> a = {1.0, 2.0, 3.0};
    std::vector<double> b = {4.0, 5.0, 6.0};
    // 1*4 + 2*5 + 3*6 = 4 + 10 + 18 = 32
    EXPECT_DOUBLE_EQ(dot_product(a.data(), b.data(), 3), 32.0);
}

TEST(DistanceTest, DotProduct_Empty) {
    EXPECT_DOUBLE_EQ(dot_product(nullptr, nullptr, 0), 0.0);
}

TEST(DistanceTest, SquaredNorm) {
    std::vector<double> v = {3.0, 4.0};
    // 3^2 + 4^2 = 25
    EXPECT_DOUBLE_EQ(squared_norm(v.data(), 2), 25.0);
}

TEST(DistanceTest, SquaredNorm_Empty) {
    EXPECT_DOUBLE_EQ(squared_norm(nullptr, 0), 0.0);
}

TEST(DistanceTest, SquaredL2_Same) {
    std::vector<double> a = {1.0, 2.0, 3.0};
    std::vector<double> b = {1.0, 2.0, 3.0};
    EXPECT_DOUBLE_EQ(squared_l2(a.data(), b.data(), 3), 0.0);
}

TEST(DistanceTest, SquaredL2_Different) {
    std::vector<double> a = {0.0, 0.0};
    std::vector<double> b = {3.0, 4.0};
    EXPECT_DOUBLE_EQ(squared_l2(a.data(), b.data(), 2), 25.0);
}

// ============================================================
// Distance metric tests (mirrors Go TestDistanceMetrics)
// ============================================================

TEST(DistanceTest, Cosine_SameVector) {
    std::vector<double> a = {1.0, 0.0, 0.0};
    double s = compute_score(DistanceMetric::Cosine, a, a);
    EXPECT_NEAR(s, 1.0, 1e-10);
}

TEST(DistanceTest, Cosine_Orthogonal) {
    std::vector<double> a = {1.0, 0.0, 0.0};
    std::vector<double> b = {0.0, 1.0, 0.0};
    double s = compute_score(DistanceMetric::Cosine, a, b);
    EXPECT_NEAR(s, 0.0, 1e-10);
}

TEST(DistanceTest, Cosine_Opposite) {
    std::vector<double> a = {1.0, 0.0, 0.0};
    std::vector<double> b = {-1.0, 0.0, 0.0};
    double s = compute_score(DistanceMetric::Cosine, a, b);
    EXPECT_NEAR(s, -1.0, 1e-10);
}

TEST(DistanceTest, L2_SameVector) {
    std::vector<double> a = {1.0, 0.0, 0.0};
    double s = compute_score(DistanceMetric::Euclidean, a, a);
    EXPECT_DOUBLE_EQ(s, 0.0);
}

TEST(DistanceTest, L2_Different) {
    std::vector<double> a = {0.0, 0.0};
    std::vector<double> b = {3.0, 4.0};
    double s = compute_score(DistanceMetric::Euclidean, a, b);
    EXPECT_DOUBLE_EQ(s, -5.0);
    EXPECT_LT(s, 0.0);
}

TEST(DistanceTest, InnerProduct_Same) {
    std::vector<double> a = {1.0, 0.0, 0.0};
    std::vector<double> b = {1.0, 0.0, 0.0};
    double s = compute_score(DistanceMetric::InnerProduct, a, b);
    EXPECT_DOUBLE_EQ(s, 1.0);
}

TEST(DistanceTest, InnerProduct_Orthogonal) {
    std::vector<double> a = {1.0, 0.0, 0.0};
    std::vector<double> b = {0.0, 1.0, 0.0};
    double s = compute_score(DistanceMetric::InnerProduct, a, b);
    EXPECT_DOUBLE_EQ(s, 0.0);
}

// ============================================================
// SIMD correctness: compare with known scalar results
// ============================================================

TEST(DistanceTest, SIMD_MatchesReference_LargeVectors) {
    // Generate 128-dim random vectors (typical embedding size)
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    std::vector<double> a(128), b(128);
    for (int i = 0; i < 128; ++i) {
        a[i] = dist(rng);
        b[i] = dist(rng);
    }

    // Compute SIMD results via compute_score
    double cosine_simd  = compute_score(DistanceMetric::Cosine, a.data(), b.data(), 128);
    double l2_simd      = compute_score(DistanceMetric::Euclidean, a.data(), b.data(), 128);
    double ip_simd      = compute_score(DistanceMetric::InnerProduct, a.data(), b.data(), 128);

    // Scalar verification
    double dot = 0, na = 0, nb = 0, dl2 = 0;
    for (int i = 0; i < 128; ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
        double d = a[i] - b[i];
        dl2 += d * d;
    }
    double cosine_ref = dot / (std::sqrt(na) * std::sqrt(nb));
    double l2_ref = -std::sqrt(dl2);
    double ip_ref = dot;

    EXPECT_NEAR(cosine_simd, cosine_ref, 1e-12);
    EXPECT_NEAR(l2_simd, l2_ref, 1e-12);
    EXPECT_NEAR(ip_simd, ip_ref, 1e-12);
}

TEST(DistanceTest, SIMD_HandlesUnalignedDimensions) {
    // Test non-multiple-of-SIMD-width dimensions
    std::mt19937 rng(99);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    for (int dim : {1, 2, 3, 7, 11, 17, 31, 65, 127, 129, 255, 257}) {
        std::vector<double> a(dim), b(dim);
        for (int i = 0; i < dim; ++i) {
            a[i] = dist(rng);
            b[i] = dist(rng);
        }

        // Scalar reference
        double dot = 0, na = 0, nb = 0, dl2 = 0;
        for (int i = 0; i < dim; ++i) {
            dot += a[i] * b[i];
            na += a[i] * a[i];
            nb += b[i] * b[i];
            double d = a[i] - b[i];
            dl2 += d * d;
        }

        double cos_simd = compute_score(DistanceMetric::Cosine, a, b);
        double cos_ref = dot / (std::sqrt(na) * std::sqrt(nb));

        double l2_simd = compute_score(DistanceMetric::Euclidean, a, b);
        double l2_ref = -std::sqrt(dl2);

        EXPECT_NEAR(cos_simd, cos_ref, 1e-12) << "Cosine mismatch at dim=" << dim;
        EXPECT_NEAR(l2_simd, l2_ref, 1e-12) << "L2 mismatch at dim=" << dim;
    }
}

// ============================================================
// Edge cases
// ============================================================

TEST(DistanceTest, Cosine_ZeroVector) {
    std::vector<double> a = {0.0, 0.0, 0.0};
    std::vector<double> b = {1.0, 2.0, 3.0};
    double s = compute_score(DistanceMetric::Cosine, a, b);
    EXPECT_DOUBLE_EQ(s, 0.0); // protect divide by zero
}

TEST(DistanceTest, EmptyVectors) {
    std::vector<double> a, b;
    EXPECT_DOUBLE_EQ(compute_score(DistanceMetric::Cosine, a, b), 0.0);
    EXPECT_DOUBLE_EQ(compute_score(DistanceMetric::Euclidean, a, b), 0.0);
    EXPECT_DOUBLE_EQ(compute_score(DistanceMetric::InnerProduct, a, b), 0.0);
}
