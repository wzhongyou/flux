#pragma once

#include <cstddef>
#include <vector>
#include "types.h"

namespace flux {

// ============================================================
// Distance / Similarity computation
//
// All functions return a "higher is better" score:
//   Cosine:       dot(a,b) / (|a| * |b|)    range [-1, 1]
//   Euclidean:    -sqrt( sum((a-b)^2) )       range [-inf, 0]
//   InnerProduct: dot(a,b)                    range [-inf, +inf]
//
// With SIMD acceleration via Google Highway.
// ============================================================

// Single score between two equal-length vectors.
double compute_score(DistanceMetric metric,
                     const double* a, const double* b, size_t dim);

// Convenience overload for std::vector.
inline double compute_score(DistanceMetric metric,
                            const std::vector<double>& a,
                            const std::vector<double>& b) {
    return compute_score(metric, a.data(), b.data(), a.size());
}

// ============================================================
// Building blocks (exposed for testing)
// ============================================================

// Dot product of two vectors.
double dot_product(const double* a, const double* b, size_t n);

// Squared L2 norm of a vector (sum of squares).
double squared_norm(const double* v, size_t n);

// Squared Euclidean distance (sum of squared differences).
double squared_l2(const double* a, const double* b, size_t n);

} // namespace flux
