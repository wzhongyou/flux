package flux

import (
	"fmt"
	"math/rand/v2"
	"testing"
)

// ============================================================
// 搜索延迟基准测试
// ============================================================

// setupBenchDB creates a database with n random 128-dim vectors.
func setupBenchDB(b *testing.B, n int, indexType string) *VectorDatabase {
	db, _ := NewVectorDatabase("")
	db.CreateCollection("bench", Cosine)

	rng := rand.New(rand.NewPCG(42, 0))
	for i := 0; i < n; i++ {
		vec := make([]float64, 128)
		for d := range vec {
			vec[d] = rng.Float64()
		}
		db.Upsert("bench", &Document{
			ID:     fmt.Sprintf("d%d", i),
			Vector: vec,
			Metadata: map[string]interface{}{"seq": i},
		})
	}
	if indexType != "" {
		db.BuildIndex("bench", indexType)
	}
	return db
}

func BenchmarkBF_100(b *testing.B) {
	db := setupBenchDB(b, 100, "")
	defer db.Close()
	query := randomVector(128)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		db.Search("bench", query, 10, nil)
	}
}

func BenchmarkBF_1000(b *testing.B) {
	db := setupBenchDB(b, 1000, "")
	defer db.Close()
	query := randomVector(128)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		db.Search("bench", query, 10, nil)
	}
}

func BenchmarkHNSW_100(b *testing.B) {
	db := setupBenchDB(b, 100, "hnsw")
	defer db.Close()
	query := randomVector(128)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		db.Search("bench", query, 10, nil)
	}
}

func BenchmarkHNSW_1000(b *testing.B) {
	db := setupBenchDB(b, 1000, "hnsw")
	defer db.Close()
	query := randomVector(128)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		db.Search("bench", query, 10, nil)
	}
}

func BenchmarkIVF_100(b *testing.B) {
	db := setupBenchDB(b, 100, "ivf")
	defer db.Close()
	query := randomVector(128)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		db.Search("bench", query, 10, nil)
	}
}

func BenchmarkIVF_1000(b *testing.B) {
	db := setupBenchDB(b, 1000, "ivf")
	defer db.Close()
	query := randomVector(128)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		db.Search("bench", query, 10, nil)
	}
}

func randomVector(dim int) []float64 {
	rng := rand.New(rand.NewPCG(rand.Uint64(), rand.Uint64()))
	v := make([]float64, dim)
	for i := range v {
		v[i] = rng.Float64()
	}
	return v
}
