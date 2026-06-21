// Flux 快速入门示例
//
// 运行:  go run examples/quickstart/main.go
//
// 这个示例演示了 Flux 的核心流程:
//   创建集合 → 写入向量 → 建立索引 → 搜索 → 查看效果

package main

import (
	"fmt"
	"log"

	"github.com/wzhongyou/flux"
)

func main() {
	// ============================================================
	// 1. 创建数据库（WAL 持久化路径）
	// ============================================================
	db, err := flux.NewVectorDatabase("./example.wal")
	if err != nil {
		log.Fatalf("failed to initialize database: %v", err)
	}
	defer db.Close()

	// ============================================================
	// 2. 创建集合 + 写入数据
	// ============================================================
	// 集合 = 向量数据库的"表"，可以有多个
	db.CreateCollection("articles", flux.Cosine)

	// 每条文档 = ID + 向量 + 元数据
	// 向量是 []float64，元数据可以是任意结构化字段
	docs := []*flux.Document{
		{ID: "doc1", Vector: []float64{0.9, 0.1, 0.2},
			Metadata: map[string]interface{}{"category": "news", "source": "site-a"}},
		{ID: "doc2", Vector: []float64{0.1, 0.8, 0.3},
			Metadata: map[string]interface{}{"category": "blog", "source": "site-b"}},
		{ID: "doc3", Vector: []float64{0.75, 0.25, 0.0},
			Metadata: map[string]interface{}{"category": "news", "source": "site-c"}},
	}
	for _, doc := range docs {
		if err := db.Upsert("articles", doc); err != nil {
			log.Fatalf("upsert: %v", err)
		}
	}
	fmt.Printf("✅ 已写入 %d 条文档\n", len(docs))

	// ============================================================
	// 3. 建立 HNSW 索引（加速搜索）
	// ============================================================
	// 不建索引也能搜，但会全量扫描。建索引后只扫描一小部分。
	if err := db.BuildIndex("articles", "hnsw"); err != nil {
		log.Fatalf("build index: %v", err)
	}
	fmt.Println("✅ HNSW 索引已建立")

	// ============================================================
	// 4. 搜索
	// ============================================================
	query := []float64{0.8, 0.2, 0.1}
	filter := flux.FieldEqual("category", "news")

	results, _ := db.Search("articles", query, 3, filter)

	fmt.Printf("\n🔎 查询向量: [0.8, 0.2, 0.1]\n")
	fmt.Printf("   过滤条件: category = news\n")
	fmt.Printf("   返回 top-3\n\n")

	for i, hit := range results {
		// Score = 余弦相似度（0~1，越接近 1 越相似）
		fmt.Printf("  #%d  id=%-4s  score=%.4f  metadata=%v\n",
			i+1, hit.ID, hit.Score, hit.Document.Metadata)
	}

	// ============================================================
	// 5. 查看集合统计
	// ============================================================
	count, dim, metric, _ := db.CollectionStats("articles")
	fmt.Printf("\n📊 集合统计: count=%d  dimension=%d  metric=%s\n", count, dim, metric)

	// 预期输出:
	//
	// ✅ 已写入 3 条文档
	// ✅ HNSW 索引已建立
	//
	// 🔎 查询向量: [0.8, 0.2, 0.1]
	//    过滤条件: category = news
	//    返回 top-3
	//
	//   #1  id=doc3  score=0.9898  metadata=map[category:news source:site-c]
	//   #2  id=doc1  score=0.9866  metadata=map[category:news source:site-a]
	//
	// 📊 集合统计: count=3  dimension=3  metric=cosine
}
