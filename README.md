# Proximia

用 Go 实现的向量数据库引擎。核心算法自实现，零第三方依赖。

## 安装

```bash
# 一键运行（需要 Go 1.22+）
go run github.com/wzhongyou/proximia/cmd/proximia@latest

# 或从源码构建
git clone https://github.com/wzhongyou/proximia.git
cd proximia
go build -o proximia ./cmd/proximia
./proximia
```

启动后打开 http://localhost:9876 进入控制台。

## 快速入门

```bash
# 1. 创建集合（带类型化 Schema）
curl -s -X POST http://localhost:9876/collections \
  -H 'Content-Type: application/json' \
  -d '{"name":"demo","schema":{"fields":[{"name":"tag","type":"string","indexable":true},{"name":"title","type":"text"},{"name":"price","type":"float"}]}}'

# 2. 写入向量
curl -s -X POST http://localhost:9876/collections/demo/upsert \
  -H 'Content-Type: application/json' \
  -d '{"id":"doc1","vector":[0.9,0.1,0.2],"metadata":{"tag":"news","title":"深度学习 Transformer 架构","price":10}}'

curl -s -X POST http://localhost:9876/collections/demo/upsert \
  -H 'Content-Type: application/json' \
  -d '{"id":"doc2","vector":[0.1,0.9,0.3],"metadata":{"tag":"blog","title":"数据库索引优化实践","price":20}}'

# 3. 建立 HNSW 索引
curl -s -X POST http://localhost:9876/collections/demo/index \
  -H 'Content-Type: application/json' \
  -d '{"action":"build","index_type":"hnsw"}'

# 4. 搜索
curl -s -X POST http://localhost:9876/collections/demo/search \
  -H 'Content-Type: application/json' \
  -d '{"query":[0.85,0.15,0.2],"k":5,"filter":{"tag":"news"}}'

# 5. 混合搜索（向量 + BM25 全文）
curl -s -X POST http://localhost:9876/collections/demo/hybrid-search \
  -H 'Content-Type: application/json' \
  -d '{"query":[0.85,0.15,0.2],"text_query":"深度学习","k":5,"alpha":0.7}'
```

## 定位

Proximia 不是 Milvus 或 Qdrant 的替代品。适用场景：

- **嵌入使用**：`import "github.com/wzhongyou/proximia"` 在进程内调用
- **边缘/私有部署**：单二进制，无需 Kubernetes、etcd、对象存储
- **CI/测试环境**：`go run` 秒级启动，无需 Docker
- **中小规模**：百万级以下向量，单节点够用
- **学习/调试**：5350 行代码，一天可读完

## API 参考

### 集合管理

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/collections` | 列出所有集合 |
| POST | `/collections` | 创建集合（可带 Schema） |
| GET | `/collections/{name}/stats` | 集合统计 |
| DELETE | `/collections/{name}` | 删除集合 |

```json
// POST /collections 请求体
{
  "name": "my_collection",
  "metric": "cosine",           // cosine | l2 | ip
  "enable_index": true,
  "index_type": "hnsw",         // hnsw | ivf
  "schema": {
    "fields": [
      {"name": "category", "type": "string", "indexable": true},
      {"name": "price", "type": "float"},
      {"name": "title", "type": "text"},
      {"name": "active", "type": "bool"},
      {"name": "location", "type": "geo"}
    ]
  }
}
```

### 文档写入

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/collections/{name}/upsert` | 单条写入/更新 |
| POST | `/collections/{name}/batch-upsert` | 批量写入 |
| POST | `/collections/{name}/batch-delete` | 批量删除 |
| POST | `/collections/{name}/truncate` | 清空集合 |

### 搜索

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/collections/{name}/search` | 向量搜索 |
| POST | `/collections/{name}/hybrid-search` | 混合搜索（向量+文本） |
| POST | `/collections/{name}/explain` | 搜索执行计划 |
| POST | `/collections/{name}/recall` | 召回率分析（ANN vs BF） |

```json
// POST /collections/{name}/search 请求体
{
  "query": [0.1, 0.2, 0.3],   // 查询向量
  "k": 10,                      // 返回 top-k
  "filter": {
    "tag": "news",              // 简单等值过滤
    "price": {"$gt": 10, "$lt": 100},  // 范围过滤
    "category": {"$in": ["a", "b"]},   // IN 过滤
    "title": {"$text": "hello"},       // 文本子串匹配
    "loc": {"$geo": {"lat": 40.71, "lng": -74.0, "radius": 10}}  // 地理半径
  }
}

// POST /collections/{name}/hybrid-search 请求体
{
  "query": [0.1, 0.2, 0.3],
  "text_query": "搜索关键词",
  "k": 10,
  "alpha": 0.7          // 0=纯文本, 1=纯向量, -1=RRF 融合
}
```

### 索引管理

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/collections/{name}/index` | 查看索引状态 |
| POST | `/collections/{name}/index` | 构建/删除索引 |

```json
// POST /collections/{name}/index 请求体（构建）
{"action": "build", "index_type": "hnsw"}
// POST /collections/{name}/index 请求体（删除）
{"action": "drop"}
```

### 运维

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/health` | 健康检查 |
| GET | `/ready` | 就绪检查 |
| GET | `/metrics` | Prometheus 指标 |
| POST | `/snapshot` | 创建快照 |
| POST | `/restore` | 恢复快照 |
| GET | `/collections/{name}/export` | 导出为 NDJSON |
| POST | `/collections/{name}/import` | 从 NDJSON 导入 |

## 配置

优先级：CLI 参数 > 环境变量 > 配置文件 > 默认值

```bash
# CLI 参数
proximia -addr :9090 -wal /data/proximia.wal -config /etc/proximia.yaml

# 环境变量
export PROXIMIA_ADDR=:9090
export PROXIMIA_WAL_PATH=/data/proximia.wal
export PROXIMIA_API_KEYS=key1,key2
export PROXIMIA_LOG_LEVEL=debug
proximia

# 配置文件（YAML 或 JSON）
proximia -config ./example_config.yaml
```

所有配置项见 [example_config.yaml](./example_config.yaml)。

## Go SDK 使用

```go
import "github.com/wzhongyou/proximia"

db, _ := proximia.NewVectorDatabase("demo.wal")
defer db.Close()

// 带 Schema 的集合
schema := &proximia.Schema{
    Fields: []proximia.SchemaField{
        {Name: "tag", Type: "string", Indexable: true},
        {Name: "title", Type: "text"},
    },
}
db.CreateCollectionWithSchema("docs", proximia.Cosine, schema)

// 写入
db.Upsert("docs", &proximia.Document{
    ID: "1", Vector: []float64{0.9, 0.1, 0.2},
    Metadata: map[string]interface{}{"tag": "news", "title": "Breaking news"},
})

// 构建索引
db.BuildIndex("docs", "hnsw")

// 向量搜索
results, _ := db.Search("docs", []float64{0.8, 0.2, 0.1}, 5,
    proximia.And(proximia.FieldEqual("tag", "news"), proximia.FieldRange("score", 0, 100)))

// 混合搜索
hybrid := db.HybridSearch("docs", []float64{0.8, 0.2, 0.1}, "news", 5, 0.7, nil)
```

## 控制台

启动后浏览器访问 http://localhost:9876：

| 页面 | 功能 |
|------|------|
| Dashboard | 集合概览、向量总数、索引状态 |
| Schema | 可视化创建集合、添加类型化字段 |
| Data | 写入/批量写入文档、浏览数据 |
| Search | 向量搜索、混合搜索、调试过滤条件 |
| Recall  | ANN vs BruteForce 召回对比 |
| Index | 构建/删除索引、调节 HNSW/IVF 参数 |
| Explain | 查看搜索执行计划、耗时分布 |
| API | 交互式 curl 命令生成 |

## 项目结构

```
├── *.go                引擎核心（13 文件，包名 proximia）
├── web/                控制台前端（HTML/CSS/JS）
├── cmd/proximia/       HTTP 服务入口
├── examples/quickstart Go SDK 示例
├── Dockerfile          多阶段构建
├── docker-compose.yml  单节点部署
└── example_config.yaml 配置示例
```

## 能力总览

| 维度 | 能力 |
|------|------|
| 索引 | HNSW、IVF、BruteForce |
| 搜索 | 向量搜索、BM25 全文、混合搜索（向量+文本） |
| 过滤 | eq / range / in / not / text / geo + And/Or/Not 组合 + 元数据倒排预过滤 |
| 度量 | Cosine、Euclidean (L2)、Inner Product |
| Schema | 类型化字段（string/float/int/bool/text/geo），写入校验 |
| 写入 | 单条 Upsert、Batch Upsert、Batch Delete |
| 持久化 | WAL（JSON Lines）、全量快照、WAL 截断 |
| 可观测 | Prometheus 指标 |
| 运维 | 健康检查、API Key 认证、CORS、配置、优雅关闭 |
| 控制台 | 8 页面 SPA |
| 部署 | 单二进制，零外部依赖 |
