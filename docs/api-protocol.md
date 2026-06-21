# Flux HTTP API 协议文档

## 基本信息

- **Base URL**: `http://localhost:9876`（可通过 `--addr` 指定端口）
- **Content-Type**: `application/json`
- **认证**: 可选，配置 `api_keys` 后通过请求头 `X-API-Key: <key>` 或查询参数 `?api_key=<key>` 传入

---

## 1. 集合（Collection）操作

### 1.1 创建集合

创建一个集合（类比关系数据库的"表"），可指定距离度量、初始索引和类型化 Schema。

```
POST /collections
```

**请求体：**

```json
{
  "name": "my_collection",
  "metric": "cosine",
  "enable_index": true,
  "index_type": "hnsw",
  "schema": {
    "fields": [
      {"name": "category", "type": "string", "indexable": true},
      {"name": "title",    "type": "text",   "indexable": true},
      {"name": "price",    "type": "float",  "indexable": true},
      {"name": "count",    "type": "int",    "indexable": false}
    ]
  }
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | ✅ | 集合名称，唯一标识 |
| `metric` | string | ❌ | 距离度量：`cosine`（默认）、`l2`、`ip` |
| `enable_index` | bool | ❌ | 是否同时创建 ANN 索引 |
| `index_type` | string | ❌ | 索引类型：`hnsw`（默认）、`ivf` |
| `schema` | object | ❌ | 类型化 Schema，不传则无 Schema 约束 |

**Schema 字段类型：**

| 类型 | 说明 | 可建倒排索引 |
|------|------|:--:|
| `string` | 字符串，支持 `$eq`、`$ne`、`$in` 过滤 | ✅ |
| `text` | 文本，支持 `$text` 子串匹配 + 自动 BM25 全文索引 | ✅ |
| `float` | 浮点数，支持 `$eq`、`$ne`、`$gt`、`$lt`、`$in` | ✅ |
| `int` | 整数，同 float | ✅ |
| `bool` | 布尔值 | ❌ |
| `geo` | 地理坐标，支持 `$geo` 半径过滤 | ❌ |

**响应（201）：**
```json
{
  "status": "created",
  "index": true,
  "index_type": "hnsw"
}
```

**响应（400，名称已存在）：**
```json
{
  "error": "collection already exists"
}
```

---

### 1.2 列出所有集合

```
GET /collections
```

**响应（200）：**
```json
[
  {
    "name": "my_collection",
    "count": 1000,
    "dimension": 768,
    "metric": "cosine",
    "index_type": "hnsw"
  }
]
```

---

### 1.3 查看集合统计

```
GET /collections/{name}/stats
```

**响应（200）：**
```json
{
  "count": 1000,
  "dimension": 768,
  "metric": "cosine",
  "index_type": "hnsw"
}
```

---

### 1.4 删除集合

```
DELETE /collections/{name}/delete
```

**响应（200）：**
```json
{"status": "deleted"}
```

---

### 1.5 清空集合数据（保留集合）

```
POST /collections/{name}/truncate
```

**响应（200）：**
```json
{"status": "truncated"}
```

---

## 2. 数据写入

### 2.1 写入/更新单条文档

```
POST /collections/{name}/upsert
```

**请求体：**

```json
{
  "id": "doc_001",
  "vector": [0.12, 0.34, 0.56, 0.78],
  "metadata": {
    "category": "news",
    "title": "AI advances in 2025",
    "price": 9.99,
    "count": 42
  }
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `id` | string | ✅ | 文档唯一 ID，已存在则覆盖更新 |
| `vector` | float64[] | ✅ | 浮点数向量，维度需一致 |
| `metadata` | object | ❌ | 任意 JSON 对象；若有 Schema 则校验 |

**响应（200）：**
```json
{"status": "upserted"}
```

---

### 2.2 批量写入

```
POST /collections/{name}/batch-upsert
```

**请求体：**

```json
{
  "documents": [
    {"id": "doc_001", "vector": [0.1, 0.2, 0.3], "metadata": {"category": "a"}},
    {"id": "doc_002", "vector": [0.4, 0.5, 0.6], "metadata": {"category": "b"}}
  ]
}
```

**响应（200）：**
```json
{
  "status": "upserted",
  "count": 2
}
```

---

### 2.3 批量删除

```
POST /collections/{name}/batch-delete
```

**请求体：**

```json
{"ids": ["doc_001", "doc_002"]}
```

**响应（200）：**
```json
{
  "status": "deleted",
  "count": 2
}
```

---

## 3. 向量检索

### 3.1 向量搜索

```
POST /collections/{name}/search
```

**请求体：**

```json
{
  "query": [0.12, 0.34, 0.56, 0.78],
  "k": 10,
  "filter": {
    "category": "news",
    "price": {"$gt": 5, "$lt": 50},
    "status": {"$in": ["published", "draft"]},
    "title": {"$text": "AI"}
  }
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `query` | float64[] | ✅ | 查询向量 |
| `k` | int | ❌ | 返回 Top-K 条，默认 10 |
| `filter` | object | ❌ | 元数据过滤条件（详见下方） |

**过滤操作符：**

| 操作符 | 示例 | 说明 |
|--------|------|------|
| `$eq`（默认） | `"category": "news"` | 精确匹配 |
| `$ne` | `"status": {"$ne": "deleted"}` | 不等于 |
| `$gt` | `"price": {"$gt": 5}` | 大于 |
| `$lt` | `"price": {"$lt": 100}` | 小于 |
| `$in` | `"tag": {"$in": ["ai", "ml"]}` | 属于集合 |
| `$text` | `"title": {"$text": "vector"}` | 文本子串匹配 |
| `$geo` | `"loc": {"$geo": {"lat":31.2,"lng":121.5,"radius":5000}}` | 地理半径（米） |

> 多个字段条件之间是 **AND** 关系。同一个字段的 `$gt` + `$lt` 组合成范围查询。

**响应（200）：**
```json
{
  "results": [
    {
      "id": "doc_001",
      "score": 0.9866,
      "document": {
        "id": "doc_001",
        "vector": [0.1, 0.2, 0.3, 0.4],
        "metadata": {
          "category": "news",
          "title": "AI advances in 2025",
          "price": 9.99
        }
      }
    }
  ],
  "total_time_ns": 23583,
  "docs_scanned": 5,
  "index_used": "hnsw"
}
```

| 字段 | 说明 |
|------|------|
| `score` | 相似度分数。Cosine: [0,1]，越接近 1 越相似；L2: 越低越好 |
| `docs_scanned` | 实际扫描的文档数（ANN 会远小于总数） |
| `index_used` | 当前使用的索引类型，`"hnsw"` / `"ivf"` / `""` （暴力扫描） |

---

### 3.2 混合搜索（向量 + 全文）

同时利用向量相似度和 BM25 全文检索，通过加权融合排序。

```
POST /collections/{name}/hybrid-search
```

**请求体：**

```json
{
  "query": [0.12, 0.34, 0.56, 0.78],
  "text_query": "vector database algorithm",
  "k": 10,
  "alpha": 0.5,
  "filter": {"category": "blog"}
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `query` | float64[] | ❌ | 向量查询（可为空数组 `[]`，纯文本搜索） |
| `text_query` | string | ❌ | 全文搜索关键词（可为空字符串，纯向量搜索） |
| `k` | int | ❌ | 返回 Top-K，默认 10 |
| `alpha` | float | ❌ | 融合权重：`0` = 纯文本，`1` = 纯向量，默认 `0.5`。`< 0` 时使用 RRF 融合 |
| `filter` | object | ❌ | 元数据过滤，同向量搜索 |

**响应（200）：**
```json
{
  "results": [
    {
      "id": "doc_004",
      "vector_score": 0.5052,
      "text_score": 0.82,
      "combined_score": 0.6626,
      "document": {
        "id": "doc_004",
        "vector": [0.3, 0.3, 0.8, 0.2],
        "metadata": {
          "category": "blog",
          "title": "Understanding vector databases for AI applications",
          "price": 15.0
        }
      }
    }
  ],
  "total_time_ns": 38209
}
```

| 字段 | 说明 |
|------|------|
| `vector_score` | 归一化后的向量相似度 [0,1] |
| `text_score` | 归一化后的 BM25 文本分数 [0,1] |
| `combined_score` | 加权融合分数：`alpha * vector_score + (1-alpha) * text_score` |

---

### 3.3 执行计划（Explain）

查看搜索的详细执行信息，用于调优。

```
POST /collections/{name}/explain
```

**请求体：** 与 `/search` 完全一致。

**响应（200）：**
```json
{
  "query": [0.8, 0.2, 0.1],
  "top_k": 3,
  "index_type": "hnsw",
  "candidates": 3,
  "results_returned": 3,
  "filter_applied": "map[category:news]",
  "search_time_ns": 19542,
  "results": [...] 
}
```

---

### 3.4 召回率分析

同一查询同时跑 ANN 和暴力搜索，对比结果计算召回率。用于评估索引质量。

```
POST /collections/{name}/recall
```

**请求体：** 与 `/search` 一致（`filter` 会被忽略，固定暴力扫描）。

**响应（200）：**
```json
{
  "query": [0.8, 0.2, 0.1],
  "top_k": 5,
  "ann_results": [...],
  "bf_results": [...],
  "recall": 1.0,
  "ann_time_ns": 9083,
  "bf_time_ns": 4000,
  "index_type": "hnsw",
  "ann_searched": true
}
```

| 字段 | 说明 |
|------|------|
| `recall` | 召回率 = ANN 结果 ∩ BF 结果数 / BF 结果数，[0,1] |
| `ann_time_ns` | ANN 搜索耗时（纳秒） |
| `bf_time_ns` | 暴力扫描耗时（纳秒） |

---

## 4. 索引管理

### 4.1 查看索引状态

```
GET /collections/{name}/index
```

**响应（200）：**
```json
{
  "collection": "my_collection",
  "index_type": "hnsw",
  "status": "active"
}
```

### 4.2 构建索引

```
POST /collections/{name}/index
```

```json
{"action": "build", "index_type": "hnsw"}
```

| `index_type` | 说明 |
|-------------|------|
| `hnsw` | 分层可导航小世界图，适合高召回场景 |
| `ivf` | 倒排文件 + KMeans 聚类，适合大规模数据 |

**响应（200）：**
```json
{"status": "index_built", "index_type": "hnsw"}
```

### 4.3 删除索引

```
POST /collections/{name}/index
```

```json
{"action": "drop"}
```

**响应（200）：**
```json
{"status": "index_dropped"}
```

---

## 5. 数据导入导出

### 5.1 导出（NDJSON 流）

```
GET /collections/{name}/export
```

返回 `Content-Type: application/x-ndjson`，每行一个 JSON 文档。

### 5.2 导入（NDJSON）

```
POST /collections/{name}/import
Content-Type: application/x-ndjson

{"id":"doc_001","vector":[0.1,0.2],"metadata":{}}
{"id":"doc_002","vector":[0.3,0.4],"metadata":{}}
```

---

## 6. 快照

### 6.1 创建快照

```
POST /snapshot
```

```json
{"path": "backup_20250101.snapshot.json"}
```

### 6.2 从快照恢复

```
POST /restore
```

```json
{"path": "backup_20250101.snapshot.json"}
```

---

## 7. 健康检查 & 监控

### 7.1 健康检查

```
GET /health
```
→ `{"status": "ok"}`

### 7.2 就绪检查

```
GET /ready
```
→ `{"status": "ok", "collections": 3}`

### 7.3 Prometheus 指标

```
GET /metrics
```
→ 返回 Prometheus text format，包含：

| 指标 | 说明 |
|------|------|
| `flux_vector_count` | 向量总数 |
| `flux_search_total` | 搜索次数 |
| `flux_upsert_total` | 写入次数 |
| `flux_delete_total` | 删除次数 |
| `flux_search_duration_ms` | 搜索延迟 |

---

## 8. 典型调用流程

### Go SDK（直接嵌入，无网络开销）

```go
import "github.com/wzhongyou/flux"

// 1. 打开数据库
db, _ := flux.NewVectorDatabase("app.wal")
defer db.Close()

// 2. 建库
schema := &flux.Schema{
    Fields: []flux.FieldSchema{
        {Name: "category", Type: flux.FieldTypeString, Indexable: true},
        {Name: "content",  Type: flux.FieldTypeText,   Indexable: true},
    },
}
db.CreateCollectionWithSchema("docs", flux.Cosine, schema)

// 3. 写数据
db.Upsert("docs", &flux.Document{
    ID:       "doc1",
    Vector:   []float64{0.1, 0.2, 0.3},
    Metadata: map[string]interface{}{"category": "tech", "content": "vector database"},
})

// 4. 建索引
db.BuildIndex("docs", "hnsw")

// 5. 查
results, _ := db.Search("docs", []float64{0.15, 0.25, 0.35}, 10,
    flux.FieldEqual("category", "tech"))

// 6. 混合查
hybrid := db.HybridSearch("docs", queryVec, "vector database", 10, 0.5, nil)
```

### HTTP API（跨语言调用）

```bash
# 1. 建库
curl -X POST http://localhost:9876/collections \
  -H 'Content-Type: application/json' \
  -d '{"name":"docs","metric":"cosine","enable_index":true,"index_type":"hnsw","schema":{"fields":[{"name":"category","type":"string","indexable":true}]}}'

# 2. 写数据
curl -X POST http://localhost:9876/collections/docs/batch-upsert \
  -H 'Content-Type: application/json' \
  -d '{"documents":[{"id":"d1","vector":[0.1,0.2],"metadata":{"category":"tech"}}]}'

# 3. 建索引（如果创建时没开）
curl -X POST http://localhost:9876/collections/docs/index \
  -H 'Content-Type: application/json' \
  -d '{"action":"build","index_type":"hnsw"}'

# 4. 检索
curl -X POST http://localhost:9876/collections/docs/search \
  -H 'Content-Type: application/json' \
  -d '{"query":[0.15,0.25],"k":5,"filter":{"category":"tech"}}'

# 5. 混合检索
curl -X POST http://localhost:9876/collections/docs/hybrid-search \
  -H 'Content-Type: application/json' \
  -d '{"query":[0.1,0.2],"text_query":"database","k":5,"alpha":0.5}'
```

### Python 调用示例

```python
import requests

BASE = "http://localhost:9876"

# 建库
requests.post(f"{BASE}/collections", json={
    "name": "docs",
    "metric": "cosine",
    "enable_index": True,
    "index_type": "hnsw",
    "schema": {
        "fields": [
            {"name": "category", "type": "string", "indexable": True},
            {"name": "title",    "type": "text",   "indexable": True},
        ]
    }
})

# 写数据
requests.post(f"{BASE}/collections/docs/batch-upsert", json={
    "documents": [
        {"id": "d1", "vector": [0.1, 0.2, 0.3], "metadata": {"category": "tech"}},
        {"id": "d2", "vector": [0.4, 0.5, 0.6], "metadata": {"category": "news"}},
    ]
})

# 检索
resp = requests.post(f"{BASE}/collections/docs/search", json={
    "query": [0.1, 0.2, 0.3],
    "k": 10,
    "filter": {"category": "tech"}
})
for r in resp.json()["results"]:
    print(f"  {r['id']} score={r['score']:.4f}")

# 混合检索
resp = requests.post(f"{BASE}/collections/docs/hybrid-search", json={
    "query": [0.1, 0.2, 0.3],
    "text_query": "database",
    "k": 5,
    "alpha": 0.5
})
```
