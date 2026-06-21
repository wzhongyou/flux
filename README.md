# Flux

轻量级向量数据库引擎。C++17 实现，核心算法自研，轻量外部依赖。

> **Flux** —— 四字母，硬朗，现代。寓意数据的持续"流动"与高吞吐，呼应向量数据库写入与检索的核心流程。与 RocksDB、Faiss 等知名项目命名风格一致。

## 快速开始

```bash
# 安装依赖（macOS）
brew install cmake nlohmann-json spdlog highway cpp-httplib cli11 yaml-cpp googletest

# 构建
make build

# 运行（HTTP 服务在 :9876）
./build/src/flux --addr :9876
```

启动后打开 http://localhost:9876 进入控制台。

### Docker

```bash
make docker
docker run -p 9876:9876 -v $(pwd)/data:/data flux:latest
```

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

# 4. 向量搜索
curl -s -X POST http://localhost:9876/collections/demo/search \
  -H 'Content-Type: application/json' \
  -d '{"query":[0.85,0.15,0.2],"k":5,"filter":{"tag":"news"}}'

# 5. 混合搜索（向量 + BM25 全文）
curl -s -X POST http://localhost:9876/collections/demo/hybrid-search \
  -H 'Content-Type: application/json' \
  -d '{"query":[0.85,0.15,0.2],"text_query":"深度学习","k":5,"alpha":0.7}'
```

## 定位

Flux 是轻量级向量数据库引擎，适用场景：

- **嵌入使用**：作为 C++ 库链接到应用进程中
- **边缘/私有部署**：单二进制，无需 Kubernetes、etcd、对象存储
- **CI/测试环境**：秒级启动，`make run` 即可
- **中小规模**：百万级以下向量，单节点够用
- **学习/研究**：SIMD 加速 + 手写 HNSW/IVF/BM25，代码清晰可读

## API 参考

协议完全稳定，上游 HTTP 客户端零感知。详见 [docs/api-protocol.md](docs/api-protocol.md)。

### 集合管理

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/collections` | 列出所有集合 |
| POST | `/collections` | 创建集合（可带 Schema） |
| GET | `/collections/{name}/stats` | 集合统计 |
| DELETE | `/collections/{name}/delete` | 删除集合 |

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

### 过滤操作符

| 操作符 | 示例 | 说明 |
|--------|------|------|
| `$eq`（默认） | `"category": "news"` | 精确匹配 |
| `$ne` | `"status": {"$ne": "deleted"}` | 不等于 |
| `$gt` | `"price": {"$gt": 5}` | 大于 |
| `$lt` | `"price": {"$lt": 100}` | 小于 |
| `$in` | `"tag": {"$in": ["ai", "ml"]}` | 属于集合 |
| `$text` | `"title": {"$text": "vector"}` | 文本子串匹配 |
| `$geo` | `"loc": {"$geo": {"lat":31.2,"lng":121.5,"radius":5000}}` | 地理半径 |

### 索引管理

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/collections/{name}/index` | 查看索引状态 |
| POST | `/collections/{name}/index` | 构建/删除索引 |

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
flux -a :9090 -w /data/flux.wal -c /etc/flux.yaml

# 环境变量
export FLUX_ADDR=:9090
export FLUX_WAL_PATH=/data/flux.wal
export FLUX_API_KEYS=key1,key2
export FLUX_LOG_LEVEL=debug
flux

# 配置文件
flux -c ./example_config.yaml
```

## C++ 库使用

```cpp
#include <flux/database.h>

flux::VectorDatabase db("demo.wal");

// 带 Schema 的集合
auto schema = std::make_unique<flux::Schema>(flux::Schema{{
    {"tag", flux::FieldType::String, true},
    {"title", flux::FieldType::Text},
}});
db.create_collection_with_schema("docs", flux::DistanceMetric::Cosine, std::move(schema));

// 写入
db.upsert("docs", {"1", {0.9, 0.1, 0.2}, {{"tag", "news"}, {"title", "Breaking"}}});

// 构建索引
db.build_index("docs", "hnsw");

// 搜索
auto results = db.search("docs", {0.8, 0.2, 0.1}, 5,
    flux::FieldEqual("tag", "news"));

// 混合搜索
auto hybrid = db.hybrid_search("docs", {0.8, 0.2, 0.1}, "news", 5, 0.7);
```

## 项目结构

```
├── CMakeLists.txt          # CMake 构建（C++17）
├── vcpkg.json              # 跨平台依赖清单
├── Makefile / Dockerfile
├── src/
│   ├── types.h             # Document, SearchResult
│   ├── distance.h/cpp      # SIMD 距离计算（Highway）
│   ├── config.h/cpp        # 配置管理
│   ├── schema.h/cpp        # Schema 类型校验
│   ├── filter.h/cpp        # 过滤引擎 + 倒排索引
│   ├── bm25.h/cpp          # BM25 全文索引
│   ├── wal.h/cpp           # WAL 预写日志
│   ├── database.h/cpp      # 引擎核心 + 混合搜索
│   ├── hnsw.h/cpp          # HNSW 图索引
│   ├── ivf.h/cpp           # IVF 聚类索引
│   ├── io.h/cpp            # 导入/导出/快照
│   ├── server.h/cpp        # HTTP 服务（16 端点）
│   └── main.cpp            # CLI 入口
├── tests/                  # 4 套测试，59 用例
├── web/                    # 控制台前端（HTML/JS/CSS）
├── docs/
│   ├── flux-design.md      # 技术设计文档
│   └── api-protocol.md     # HTTP API 协议文档
└── example_config.yaml
```

## 技术栈

| 维度 | 选型 |
|------|------|
| 语言 | C++17 |
| 构建 | CMake 3.25+ |
| SIMD | Google Highway（x86 AVX2/AVX-512, ARM NEON） |
| HTTP | cpp-httplib（单头文件） |
| JSON | nlohmann/json（单头文件） |
| 日志 | spdlog |
| CLI | CLI11 |
| 测试 | Google Test |
| 包管理 | vcpkg / Homebrew |

## 能力总览

| 维度 | 能力 |
|------|------|
| 索引 | HNSW、IVF、BruteForce |
| 搜索 | 向量搜索、BM25 全文、混合搜索（向量+文本） |
| 过滤 | eq / range / in / not / text / geo + And/Or/Not 组合 + 元数据倒排预过滤 |
| 度量 | Cosine、Euclidean (L2)、Inner Product |
| Schema | 类型化字段（string/float/int/bool/text/geo），写入校验 |
| 写入 | 单条 Upsert、Batch Upsert、Batch Delete |
| 持久化 | WAL（JSON Lines + CRC32）、全量快照 |
| 可观测 | Prometheus 指标、搜索执行计划、Recall@K 分析 |
| 运维 | 健康检查、API Key 认证、CORS、配置、优雅关闭 |
| 控制台 | 5 页面 SPA（Home/Collections/Documents/Search/Monitor） |
| 性能 | SIMD 加速距离计算（相对 Go 版本 3-8x） |
| 部署 | 单二进制 ~5MB，零运行时依赖 |
