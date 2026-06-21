#include "distance.h"

#include <hwy/highway.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <stdexcept>

namespace flux {
namespace HWY_NAMESPACE {
namespace {

using hwy::HWY_NAMESPACE::ScalableTag;
using hwy::HWY_NAMESPACE::LoadU;
using hwy::HWY_NAMESPACE::Zero;
using hwy::HWY_NAMESPACE::MulAdd;
using hwy::HWY_NAMESPACE::Sub;
using hwy::HWY_NAMESPACE::Lanes;
using hwy::HWY_NAMESPACE::ReduceSum;

// ============================================================
// SIMD-accelerated vector operations for double precision
// ============================================================

double DotProductSIMD(const double* HWY_RESTRICT a,
                      const double* HWY_RESTRICT b,
                      size_t n) {
    const ScalableTag<double> d;
    using V = decltype(Zero(d));

    auto sum0 = Zero(d);
    auto sum1 = Zero(d);
    size_t i = 0;

    const size_t N = Lanes(d);
    // Process 2 vectors per iteration (unroll by 2 for instruction pipelining)
    for (; i + 2 * N <= n; i += 2 * N) {
        auto va0 = LoadU(d, a + i);
        auto vb0 = LoadU(d, b + i);
        sum0 = MulAdd(va0, vb0, sum0);

        auto va1 = LoadU(d, a + i + N);
        auto vb1 = LoadU(d, b + i + N);
        sum1 = MulAdd(va1, vb1, sum1);
    }

    // Remaining full vectors
    for (; i + N <= n; i += N) {
        auto va = LoadU(d, a + i);
        auto vb = LoadU(d, b + i);
        sum0 = MulAdd(va, vb, sum0);
    }

    double result = ReduceSum(d, sum0) + ReduceSum(d, sum1);

    // Tail (less than one vector width)
    for (; i < n; ++i) {
        result += a[i] * b[i];
    }
    return result;
}

double SquaredNormSIMD(const double* HWY_RESTRICT v, size_t n) {
    const ScalableTag<double> d;
    using V = decltype(Zero(d));

    auto sum0 = Zero(d);
    auto sum1 = Zero(d);
    size_t i = 0;
    const size_t N = Lanes(d);

    for (; i + 2 * N <= n; i += 2 * N) {
        auto v0 = LoadU(d, v + i);
        auto v1 = LoadU(d, v + i + N);
        sum0 = MulAdd(v0, v0, sum0);
        sum1 = MulAdd(v1, v1, sum1);
    }
    for (; i + N <= n; i += N) {
        auto va = LoadU(d, v + i);
        sum0 = MulAdd(va, va, sum0);
    }

    double result = ReduceSum(d, sum0) + ReduceSum(d, sum1);
    for (; i < n; ++i) {
        result += v[i] * v[i];
    }
    return result;
}

double SquaredL2SIMD(const double* HWY_RESTRICT a,
                     const double* HWY_RESTRICT b,
                     size_t n) {
    const ScalableTag<double> d;
    using V = decltype(Zero(d));

    auto sum0 = Zero(d);
    auto sum1 = Zero(d);
    size_t i = 0;
    const size_t N = Lanes(d);

    for (; i + 2 * N <= n; i += 2 * N) {
        auto va0 = LoadU(d, a + i);
        auto vb0 = LoadU(d, b + i);
        auto diff0 = Sub(va0, vb0);
        sum0 = MulAdd(diff0, diff0, sum0);

        auto va1 = LoadU(d, a + i + N);
        auto vb1 = LoadU(d, b + i + N);
        auto diff1 = Sub(va1, vb1);
        sum1 = MulAdd(diff1, diff1, sum1);
    }
    for (; i + N <= n; i += N) {
        auto va = LoadU(d, a + i);
        auto vb = LoadU(d, b + i);
        auto diff = Sub(va, vb);
        sum0 = MulAdd(diff, diff, sum0);
    }

    double result = ReduceSum(d, sum0) + ReduceSum(d, sum1);
    for (; i < n; ++i) {
        double delta = a[i] - b[i];
        result += delta * delta;
    }
    return result;
}

} // anonymous namespace
} // namespace HWY_NAMESPACE

// ============================================================
// Public API — dispatches to SIMD implementations
// ============================================================

double dot_product(const double* a, const double* b, size_t n) {
    if (n == 0) return 0.0;
    return HWY_NAMESPACE::DotProductSIMD(a, b, n);
}

double squared_norm(const double* v, size_t n) {
    if (n == 0) return 0.0;
    return HWY_NAMESPACE::SquaredNormSIMD(v, n);
}

double squared_l2(const double* a, const double* b, size_t n) {
    if (n == 0) return 0.0;
    return HWY_NAMESPACE::SquaredL2SIMD(a, b, n);
}

double compute_score(DistanceMetric metric,
                     const double* a, const double* b, size_t dim) {
    if (dim == 0) return 0.0;

    double dot = dot_product(a, b, dim);

    switch (metric) {
    case DistanceMetric::Euclidean: {
        double dist2 = squared_l2(a, b, dim);
        return -std::sqrt(dist2);
    }
    case DistanceMetric::InnerProduct:
        return dot;
    case DistanceMetric::Cosine:
    default: {
        double norm_a2 = squared_norm(a, dim);
        double norm_b2 = squared_norm(b, dim);
        if (norm_a2 == 0.0 || norm_b2 == 0.0) return 0.0;
        return dot / (std::sqrt(norm_a2) * std::sqrt(norm_b2));
    }
    }
}

// ============================================================
// DistanceMetric helpers
// ============================================================

DistanceMetric metric_from_string(const std::string& s) {
    if (s == "cosine")  return DistanceMetric::Cosine;
    if (s == "l2" || s == "euclidean") return DistanceMetric::Euclidean;
    if (s == "ip" || s == "inner_product") return DistanceMetric::InnerProduct;
    throw std::invalid_argument("unknown distance metric: " + s);
}

const char* metric_to_string(DistanceMetric m) {
    switch (m) {
    case DistanceMetric::Cosine:    return "cosine";
    case DistanceMetric::Euclidean: return "l2";
    case DistanceMetric::InnerProduct: return "ip";
    }
    return "cosine";
}

// ============================================================
// JSON serialization for types
// ============================================================

void to_json(nlohmann::json& j, const Document& d) {
    j = nlohmann::json{
        {"id", d.id},
        {"vector", d.vector},
        {"metadata", d.metadata},
    };
}

void from_json(const nlohmann::json& j, Document& d) {
    j.at("id").get_to(d.id);
    j.at("vector").get_to(d.vector);
    if (j.contains("metadata")) {
        j.at("metadata").get_to(d.metadata);
    } else {
        d.metadata = nlohmann::json::object();
    }
}

void to_json(nlohmann::json& j, const SearchResult& r) {
    j = nlohmann::json{
        {"id", r.id},
        {"score", r.score},
    };
    if (r.doc_ptr) {
        j["document"] = *r.doc_ptr;
    }
}

void to_json(nlohmann::json& j, const HybridSearchResult& r) {
    j = nlohmann::json{
        {"id", r.id},
        {"vector_score", r.vector_score},
        {"text_score", r.text_score},
        {"combined_score", r.combined_score},
    };
    if (r.doc_ptr) {
        j["document"] = *r.doc_ptr;
    }
}

Document Document::clone() const {
    return Document{id, vector, metadata};
}

} // namespace flux
