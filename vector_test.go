package flux

import (
	"fmt"
	"math"
	"testing"
)

// ============================================================
// 集合管理
// ============================================================

func TestCreateCollection(t *testing.T) {
	db, err := NewVectorDatabase("")
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()

	if err := db.CreateCollection("test", Cosine); err != nil {
		t.Fatalf("CreateCollection failed: %v", err)
	}

	// 重复创建应报错
	if err := db.CreateCollection("test", Cosine); err == nil {
		t.Fatal("expected error for duplicate collection")
	}

	names := db.ListCollections()
	if len(names) != 1 || names[0] != "test" {
		t.Fatalf("ListCollections = %v, want [test]", names)
	}
}

func TestCreateCollectionWithSchema(t *testing.T) {
	db, _ := NewVectorDatabase("")
	defer db.Close()

	schema := &Schema{
		Fields: []SchemaField{
			{Name: "tag", Type: FieldTypeString, Indexable: true},
			{Name: "price", Type: FieldTypeFloat},
			{Name: "active", Type: FieldTypeBool},
			{Name: "title", Type: FieldTypeText},
		},
	}

	if err := db.CreateCollectionWithSchema("typed", Cosine, schema); err != nil {
		t.Fatalf("CreateCollectionWithSchema failed: %v", err)
	}

	// 校验写入合法数据
	err := db.Upsert("typed", &Document{
		ID: "1", Vector: []float64{1, 0, 0},
		Metadata: map[string]interface{}{"tag": "news", "price": 10.5, "active": true, "title": "hello"},
	})
	if err != nil {
		t.Fatalf("valid upsert failed: %v", err)
	}

	// 校验写入非法类型
	err = db.Upsert("typed", &Document{
		ID: "2", Vector: []float64{0, 1, 0},
		Metadata: map[string]interface{}{"tag": 123}, // string 传了 int
	})
	if err == nil {
		t.Fatal("expected schema validation error")
	}
}

// ============================================================
// 文档写入
// ============================================================

func TestUpsertAndDelete(t *testing.T) {
	db, _ := NewVectorDatabase("")
	defer db.Close()
	db.CreateCollection("test", Cosine)

	// Upsert
	err := db.Upsert("test", &Document{ID: "1", Vector: []float64{1, 0, 0}})
	if err != nil {
		t.Fatalf("upsert failed: %v", err)
	}

	// 更新
	err = db.Upsert("test", &Document{ID: "1", Vector: []float64{0, 1, 0}})
	if err != nil {
		t.Fatalf("update failed: %v", err)
	}

	// Delete
	err = db.Delete("test", "1")
	if err != nil {
		t.Fatalf("delete failed: %v", err)
	}

	// Delete 不存在的 doc 不应报错
	err = db.Delete("test", "nonexistent")
	if err != nil {
		t.Fatalf("delete nonexistent failed: %v", err)
	}
}

func TestBatchUpsertAndDelete(t *testing.T) {
	db, _ := NewVectorDatabase("")
	defer db.Close()
	db.CreateCollection("test", Cosine)

	docs := make([]*Document, 10)
	for i := 0; i < 10; i++ {
		docs[i] = &Document{
			ID:     fmt.Sprintf("d%d", i),
			Vector: []float64{float64(i) / 10, 1 - float64(i)/10, 0},
		}
	}

	count, err := db.BatchUpsert("test", docs)
	if err != nil {
		t.Fatalf("batch upsert failed: %v", err)
	}
	if count != 10 {
		t.Fatalf("batch upsert count = %d, want 10", count)
	}

	ids := []string{"d0", "d1", "d2"}
	count, err = db.BatchDelete("test", ids)
	if err != nil {
		t.Fatalf("batch delete failed: %v", err)
	}
	if count != 3 {
		t.Fatalf("batch delete count = %d, want 3", count)
	}
}

// ============================================================
// 搜索
// ============================================================

func TestBruteForceSearch(t *testing.T) {
	db, _ := NewVectorDatabase("")
	defer db.Close()
	db.CreateCollection("test", Cosine)
	seedDocs(db, "test", 20)

	results, err := db.Search("test", []float64{1, 0, 0}, 5, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 5 {
		t.Fatalf("got %d results, want 5", len(results))
	}

	// 结果按 score 降序
	for i := 1; i < len(results); i++ {
		if results[i].Score > results[i-1].Score {
			t.Fatal("results not sorted by score descending")
		}
	}
}

func TestSearchWithFilter(t *testing.T) {
	db, _ := NewVectorDatabase("")
	defer db.Close()
	db.CreateCollection("test", Cosine)

	docs := []*Document{
		{ID: "a1", Vector: []float64{1, 0, 0}, Metadata: map[string]interface{}{"tag": "news", "val": 10}},
		{ID: "a2", Vector: []float64{0, 1, 0}, Metadata: map[string]interface{}{"tag": "blog", "val": 20}},
		{ID: "a3", Vector: []float64{0.9, 0.1, 0}, Metadata: map[string]interface{}{"tag": "news", "val": 15}},
	}
	for _, d := range docs {
		db.Upsert("test", d)
	}

	// 等值过滤
	results, _ := db.Search("test", []float64{1, 0, 0}, 5, FieldEqual("tag", "news"))
	if len(results) != 2 {
		t.Fatalf("news filter: got %d, want 2", len(results))
	}

	// 范围过滤
	results, _ = db.Search("test", []float64{1, 0, 0}, 5, FieldRange("val", 12, 20))
	if len(results) != 2 {
		t.Fatalf("range filter: got %d, want 2", len(results))
	}

	// 组合过滤
	results, _ = db.Search("test", []float64{1, 0, 0}, 5,
		And(FieldEqual("tag", "news"), FieldRange("val", 12, 20)))
	if len(results) != 1 || results[0].ID != "a3" {
		t.Fatalf("combo filter: got %d results, want 1 (a3)", len(results))
	}
}

func TestSearchWithInFilter(t *testing.T) {
	db, _ := NewVectorDatabase("")
	defer db.Close()
	db.CreateCollection("test", Cosine)

	for i := 0; i < 5; i++ {
		db.Upsert("test", &Document{
			ID: fmt.Sprintf("d%d", i), Vector: []float64{float64(i) / 5, 1 - float64(i)/5, 0},
			Metadata: map[string]interface{}{"group": fmt.Sprintf("g%d", i%3)},
		})
	}

	results, _ := db.Search("test", []float64{1, 0, 0}, 10, FieldIn("group", "g0", "g1"))
	if len(results) != 4 {
		t.Fatalf("IN filter: got %d, want 4", len(results))
	}
}

func TestSearchEmptyCollection(t *testing.T) {
	db, _ := NewVectorDatabase("")
	defer db.Close()
	db.CreateCollection("empty", Cosine)
	results, err := db.Search("empty", []float64{1, 0, 0}, 5, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 0 {
		t.Fatalf("expected empty results, got %d", len(results))
	}
}

// ============================================================
// 距离度量
// ============================================================

func TestDistanceMetrics(t *testing.T) {
	a := []float64{1, 0, 0}
	b := []float64{1, 0, 0}
	c := []float64{0, 1, 0}

	// Cosine: 相同向量 score=1
	if s := computeScore(Cosine, a, a); math.Abs(s-1) > 1e-10 {
		t.Fatalf("cosine same vector: got %f, want 1", s)
	}
	// Cosine: 正交 score=0
	if s := computeScore(Cosine, a, c); math.Abs(s) > 1e-10 {
		t.Fatalf("cosine orthogonal: got %f, want 0", s)
	}

	// L2: 相同 score=0
	if s := computeScore(Euclidean, a, a); s != 0 {
		t.Fatalf("l2 same: got %f, want 0", s)
	}
	// L2: 不同 score<0
	if s := computeScore(Euclidean, a, c); s >= 0 {
		t.Fatalf("l2 different: got %f, want negative", s)
	}

	// IP
	if s := computeScore(InnerProduct, a, b); math.Abs(s-1) > 1e-10 {
		t.Fatalf("ip: got %f, want 1", s)
	}
}

// ============================================================
// ANN 索引搜索
// ============================================================

func TestHNSWSearch(t *testing.T) {
	db, _ := NewVectorDatabase("")
	defer db.Close()
	db.CreateCollection("test", Cosine)
	seedDocs(db, "test", 100)

	if err := db.BuildIndex("test", "hnsw"); err != nil {
		t.Fatalf("BuildIndex failed: %v", err)
	}

	results, err := db.Search("test", []float64{0.5, 0.5, 0}, 10, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 10 {
		t.Fatalf("HNSW search: got %d results, want 10", len(results))
	}

	// 确认结果有序
	for i := 1; i < len(results); i++ {
		if results[i].Score > results[i-1].Score {
			t.Fatal("HNSW results not sorted")
		}
	}
}

func TestIVFSearch(t *testing.T) {
	db, _ := NewVectorDatabase("")
	defer db.Close()
	db.CreateCollection("test", Cosine)
	seedDocs(db, "test", 100)

	if err := db.BuildIndex("test", "ivf"); err != nil {
		t.Fatalf("IVF BuildIndex failed: %v", err)
	}

	results, err := db.Search("test", []float64{0.5, 0.5, 0}, 5, nil)
	if err != nil {
		t.Fatal(err)
	}
	// IVF 应该返回结果
	if len(results) == 0 {
		t.Fatal("IVF search returned 0 results")
	}
}

func TestIndexDrop(t *testing.T) {
	db, _ := NewVectorDatabase("")
	defer db.Close()
	db.CreateCollection("test", Cosine)
	seedDocs(db, "test", 10)
	db.BuildIndex("test", "hnsw")

	// 确认有索引
	it, _ := db.IndexInfo("test")
	if it != "hnsw" {
		t.Fatalf("expected hnsw, got %s", it)
	}

	// 删除索引
	db.DropIndex("test")
	it, _ = db.IndexInfo("test")
	if it != "" {
		t.Fatalf("expected no index, got %s", it)
	}
}

// ============================================================
// Schema 校验
// ============================================================

func TestSchemaValidate(t *testing.T) {
	schema := &Schema{
		Fields: []SchemaField{
			{Name: "name", Type: FieldTypeString},
			{Name: "count", Type: FieldTypeInt},
			{Name: "rate", Type: FieldTypeFloat},
			{Name: "ok", Type: FieldTypeBool},
			{Name: "loc", Type: FieldTypeGeo},
		},
	}

	tests := []struct {
		name     string
		metadata map[string]interface{}
		wantErr  bool
	}{
		{"valid all", map[string]interface{}{
			"name": "hello", "count": 42, "rate": 3.14, "ok": true,
			"loc": map[string]interface{}{"lat": 40.0, "lng": -74.0},
		}, false},
		{"string type error", map[string]interface{}{"name": 123}, true},
		{"int type error", map[string]interface{}{"count": "not_int"}, true},
		{"bool type error", map[string]interface{}{"ok": "yes"}, true},
		{"geo missing lng", map[string]interface{}{"loc": map[string]interface{}{"lat": 1.0}}, true},
		{"missing field is ok", map[string]interface{}{}, false},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := schema.Validate(tt.metadata)
			if (err != nil) != tt.wantErr {
				t.Errorf("Validate() error = %v, wantErr = %v", err, tt.wantErr)
			}
		})
	}
}

// ============================================================
// 快照
// ============================================================

func TestSnapshotRestore(t *testing.T) {
	db, _ := NewVectorDatabase("")
	defer db.Close()
	db.CreateCollection("test", Cosine)
	seedDocs(db, "test", 10)

	path := "/tmp/test_snapshot.json"
	if err := db.Snapshot(path); err != nil {
		t.Fatalf("Snapshot failed: %v", err)
	}

	db2, _ := NewVectorDatabase("")
	if err := db2.LoadFromSnapshot(path); err != nil {
		t.Fatalf("LoadFromSnapshot failed: %v", err)
	}
	defer db2.Close()

	count, _, _, _ := db2.CollectionStats("test")
	if count != 10 {
		t.Fatalf("after restore: count = %d, want 10", count)
	}
}



// ============================================================
// 并发安全
// ============================================================

func TestConcurrentReadWrite(t *testing.T) {
	db, _ := NewVectorDatabase("")
	defer db.Close()
	db.CreateCollection("test", Cosine)

	done := make(chan bool)
	for i := 0; i < 20; i++ {
		go func(n int) {
			id := fmt.Sprintf("d%d", n)
			db.Upsert("test", &Document{ID: id, Vector: []float64{float64(n) / 20, 1 - float64(n)/20, 0}})
			db.Search("test", []float64{0.5, 0.5, 0}, 3, nil)
			done <- true
		}(i)
	}
	for i := 0; i < 20; i++ {
		<-done
	}
	count, _, _, _ := db.CollectionStats("test")
	if count != 20 {
		t.Fatalf("concurrent test: count = %d, want 20", count)
	}
}

// ============================================================
// Helpers
// ============================================================

func seedDocs(db *VectorDatabase, collection string, n int) {
	for i := 0; i < n; i++ {
		x := float64(i) / float64(n)
		db.Upsert(collection, &Document{
			ID:     fmt.Sprintf("d%d", i),
			Vector: []float64{x, 1 - x, 0},
			Metadata: map[string]interface{}{
				"seq": i,
			},
		})
	}
}
