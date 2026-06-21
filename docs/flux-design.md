# Flux 技术设计与项目定位

> 版本: 1.1 | 最后更新: 2026-05-31

## 修订历史

| 版本 | 日期 | 变更 |
|------|------|------|
| 1.1 | 2026-05-31 | 补充 Schema、BM25、混合搜索、控制台设计；更新架构图对齐实装；补充运算符语义说明 |
| 1.0 | 2026-05-30 | 初始版本，完成 HNSW/IVF 索引、WAL、HTTP API、控制台、快照、指标、导入导出 |

## 1. 项目定位

Flux 目标是构建一个面向智能体/搜索引擎场景的向量数据库。

核心目标：
- 支持智能体语义检索、检索增强生成（RAG）、知识库搜索、推荐、相似度匹配等场景
- 提供向量数据库关键能力，对齐主流同类产品的核心能力集
- 提供可调试、可观察的 Web 控制台，帮助理解向量数据库的使用方法与内部原理

## 2. 架构

### 2.1 三层架构总览

Flux 整体架构分为三层：**应用层 → 能力层 → 核心依赖层**，上层对下层进行能力调用与组合。

```mermaid
graph TB
    %% 样式定义
    classDef app fill:#e1f5fe,stroke:#0288d1,stroke-width:2px,color:#01579b
    classDef cap fill:#fff3e0,stroke:#f57c00,stroke-width:2px,color:#bf360c
    classDef core fill:#e8f5e9,stroke:#388e3c,stroke-width:2px,color:#1b5e20

    subgraph AppLayer["应 用 层"]
        direction TB
        RAG["RAG / 知识库检索"]
        Agent["智能体语义检索"]
        Recommend["推荐系统"]
        SearchGraph["相似度搜索"]
        Classify["向量分类"]
    end

    subgraph CapLayer["能 力 层"]
        direction TB

        subgraph StorageCapa["向量存储"]
            S1["多集合 / Namespace"]
            S2["向量写入 / 更新 / 删除"]
            S3["元数据绑定"]
            S4["批量操作"]
        end

        subgraph IndexCapa["索引与搜索"]
            I1["精确搜索（BruteForce）"]
            I2["ANN 索引（HNSW / IVF）+ BM25 全文"]
            I3["相似度度量（Cosine / L2 / IP）"]
            I4["元数据过滤（布尔 / 范围 / 文本）"]
        end

        subgraph ObserveCapa["可观测性"]
            O1["Prometheus 指标"]
            O2["查询耗时 / 写入统计"]
            O3["执行计划与解释"]
            O4["索引状态监控"]
        end

        subgraph ExtCapa["生态"]
            E1["HTTP REST API"]
            E2["C++ 库嵌入"]
            E3["Web 调试控制台"]
            E4["LangChain / LlamaIndex 集成"]
        end
    end

    subgraph CoreLayer["核 心 依 赖"]
        direction TB

        subgraph Lang["语言与运行时"]
            L1["C++17 (GCC 13+ / Clang 17+)"]
            L2["cpp-httplib HTTP 服务"]
            L3["std::shared_mutex 并发控制"]
        end

        subgraph Store["持久化"]
            D1["JSON Lines WAL + CRC32"]
            D2["内存向量存储"]
            D3["JSON 快照"]
        end

        subgraph AlgoC["算法"]
            A1["HNSW 图算法"]
            A2["IVF 聚类 (K-means++)"]
            A3["BM25 全文检索"]
            A4["余弦 / L2 / 内积 + SIMD"]
        end

        subgraph Tool["工具链"]
            T1["CMake + vcpkg"]
            T2["HTML / JS 控制台前端"]
            T3["Prometheus 集成"]
        end
    end

    AppLayer --> CapLayer
    CapLayer --> CoreLayer
```

### 2.2 数据流设计

#### 写入路径

所有写入操作先记录预写日志（WAL）确保持久性，再更新内存索引结构。

```mermaid
sequenceDiagram
    participant C as Client
    participant API as HTTP API
    participant W as WAL
    participant DB as VectorDatabase
    participant Col as Collection

    C->>API: POST /collections/{name}/upsert
    API->>DB: Upsert(collection, documents)
    DB->>W: Append("upsert" record)
    W-->>DB: ok
    DB->>Col: documents[id] = doc
    Col-->>DB: ok
    DB-->>API: 200 {"upserted": n}
    API-->>C: JSON Response
```

#### 查询路径

搜索请求经过索引选择（HNSW/IVF/BF）→ 元数据过滤（预过滤或后处理）→ 相似度计算 → 排序 → top-k 截断的完整链路。

```mermaid
sequenceDiagram
    participant C as Client
    participant API as HTTP API
    participant DB as VectorDatabase
    participant Col as Collection
    participant Idx as ANN Index
    participant Meta as 元数据倒排索引
    participant Score as Score计算

    C->>API: POST /collections/{name}/search
    API->>DB: Search(collection, query, k, filter)
    DB->>Col: col.Search(query, k, filter)
    alt 有 ANN 索引
        Col->>Idx: Index.SearchInternal(query, k)
        alt 有元数据倒排索引
            Col->>Meta: MetaIndex.Match(field, value)
            Meta-->>Col: 候选 ID 集
            Col->>Idx: FilteredIndex.SearchWithFilter(candidates)
        else 无预过滤
            note over Col: 后处理：过采样 + 逐条过滤
        end
        Idx-->>Col: 索引结果
    else 无索引
        note over Col: 全量 BruteForce 扫描
    end
    Col->>Score: computeScore(query, doc)
    Score-->>Col: score
    Col->>Col: 排序取 top-k
    Col-->>DB: results
    DB-->>API: 200 {"results": [...]}
    API-->>C: JSON Response
```

### 2.3 当前实现状态

| 模块 | 状态 | 说明 |
|------|------|------|
| HTTP REST API（16+ 个端点） | ✅ C++17 | cpp-httplib，API 协议兼容 |
| 内存向量存储 | ✅ | Collection 管理，shared_mutex 并发控制 |
| WAL 持久化 | ✅ | JSON Lines + CRC32，启动时重放恢复 |
| 精确搜索（BruteForce） | ✅ | Highway SIMD 加速 |
| 三种距离度量 | ✅ | Cosine、Euclidean (L2)、InnerProduct |
| 元数据过滤 | ✅ | 等式/范围/IN/NOT/文本/地理半径 + And/Or/Not 组合器 + 倒排索引预过滤 |
| HNSW 索引 | ✅ | recall@10 ≥ 85%，支持 efSearch/efConstruction/M 参数 |
| IVF 索引 | ✅ | K-means++ 聚类 + 倒排列表，支持 nProbe 参数 |
| BM25 全文检索 | ✅ | BM25Okapi，支持 IndexDocument / Remove / Score |
| 混合搜索 | ✅ | 加权融合 + RRF（Reciprocal Rank Fusion） |
| Web 控制台 | ✅ | 编译期嵌入 HTML/JS SPA |
| Prometheus 指标 | ✅ | 轻量 text format 输出 |
| 查询解释器 | ✅ | 搜索路径、候选数、耗时展示 |
| 导入 / 导出 | ✅ | NDJSON 流式 |
| 快照 / 恢复 | ✅ | JSON 全量保存 + WAL 截断 |

### 2.4 模块依赖关系

```mermaid
graph LR
    classDef done fill:#c8e6c9,stroke:#388e3c,stroke-width:2px
    classDef vital fill:#bbdefb,stroke:#1565c0,stroke-width:2px

    subgraph HTTP层
        A["src/server.cpp<br/>HTTP 服务器"]
    end

    subgraph 引擎层 (flux_core)
        B["database.cpp<br/>VectorDatabase + Collection"]
        C["wal.cpp<br/>WAL 持久化"]
        D["hnsw.cpp<br/>HNSW 图索引"]
        E["ivf.cpp<br/>IVF 聚类索引"]
        F["index.h<br/>Index / FilteredIndex 接口"]
        G["schema.cpp<br/>Schema 类型化字段"]
        H["filter.cpp<br/>过滤引擎 + 元数据倒排索引"]
        I["bm25.cpp<br/>BM25 全文索引"]
        K["hybrid.cpp<br/>混合搜索融合"]
        L["snapshot.cpp<br/>快照/恢复"]
        M["io.cpp<br/>导入导出"]
        N["metrics.cpp<br/>Prometheus 指标"]
        O["config.cpp<br/>配置管理"]
        P["server.cpp<br/>HTTP 服务 + 静态资源"]
    end

    subgraph 前端
        J["web/<br/>控制台前端 (HTML/JS/CSS)"]
    end

    J --> P
    A --> B
    A --> P
    B --> C
    B --> D
    B --> E
    B --> F
    B --> G
    B --> H
    B --> I
    B --> K
    B --> L
    B --> M
    B --> N
    B --> O

    class A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P vital
```

### 2.5 搜索执行流程

```mermaid
flowchart TD
    Start["收到搜索请求"] --> Parse["解析参数<br/>query_vector, top_k, filter, text_query"]
    Parse --> CheckIndex{"是否有 ANN 索引？"}
    
    CheckIndex -- "否" --> Brute["全量扫描"]
    Brute --> FilterDocs["应用元数据过滤"]
    FilterDocs --> Score["计算相似度<br/>Cosine / L2 / InnerProduct"]
    Score --> Sort["降序排序"]
    Sort --> TopK["截取 top-k"]
    TopK --> Return["返回结果"]
    
    CheckIndex -- "是" --> ANNIndex{"选择索引路径"}
    ANNIndex -- "HNSW" --> HN["HNSW 图遍历<br/>参数：efSearch"]
    ANNIndex -- "IVF" --> IV["IVF 聚类搜索<br/>参数：nProbe"]
    ANNIndex -- "混合搜索" --> Hybrid["BM25 + 向量融合<br/>alpha / RRF"]

    HN --> PreFilter{"有元数据倒排索引？"}
    IV --> PreFilter
    PreFilter -- "是" --> FIndex["预过滤：元数据倒排索引<br/>→ 候选 ID 集"]
    PreFilter -- "否" --> FPost["后处理：过采样 3x + 过滤"]
    FIndex --> FilterDocs
    FPost --> FilterDocs
    Hybrid --> FilterDocs
```

### 2.6 系统架构全景图

```mermaid
graph TB
    %% ========== 样式定义 ==========
    classDef ext fill:#f3e5f5,stroke:#9c27b0,stroke-width:2px,color:#4a148c
    classDef http fill:#e1f5fe,stroke:#0288d1,stroke-width:2px,color:#01579b
    classDef engine fill:#fff3e0,stroke:#f57c00,stroke-width:2px,color:#bf360c
    classDef store fill:#e8f5e9,stroke:#388e3c,stroke-width:2px,color:#1b5e20
    classDef index fill:#fce4ec,stroke:#c62828,stroke-width:2px,color:#b71c1c
    classDef obs fill:#fff8e1,stroke:#f9a825,stroke-width:2px,color:#f57f17
    classDef future stroke-dasharray:6 4,stroke-width:1.5

    %% ========== 外部客户端 ==========
    subgraph Client["🧑 客户端"]
        direction LR
        CLI["curl / Postman<br/>(HTTP Client)"]
        SDK["C++ 库嵌入<br/>(libflux_core)"]
        Console["Web 控制台<br/>(浏览器)"]
    end

    %% ========== HTTP API 层 ==========
    subgraph HTTP["🌐 HTTP API 层 (src/server.cpp)"]
        direction TB
        Router["cpp-httplib 路由<br/>mux.HandleFunc"]

        subgraph Middleware["中间件"]
            MW_Auth["API Key 认证<br/>X-API-Key"]
            MW_CORS["CORS<br/>允许来源"]
            MW_Limit["并发限制<br/>max_concurrent"]
        end

        subgraph Handlers["请求处理器 (16+ 端点)"]
            POST_Create["POST /collections<br/>创建集合（可选 Schema）"]
            POST_Upsert["POST /collections/{name}/upsert<br/>写入向量"]
            POST_Batch["POST /collections/{name}/batch-upsert<br/>批量写入"]
            POST_Search["POST /collections/{name}/search<br/>搜索向量"]
            POST_Hybrid["POST /collections/{name}/hybrid-search<br/>混合搜索"]
            POST_Explain["POST /collections/{name}/explain<br/>执行计划"]
            POST_Index["POST /collections/{name}/index<br/>构建/删除索引"]
            GET_Stats["GET /collections/{name}/stats<br/>集合统计"]
            GET_Health["GET /health<br/>健康检查"]
        end
    end

    %% ========== 引擎核心层 ==========
    subgraph Engine["⚙️ 引擎核心层 (libflux_core)"]
        direction TB

        DB["VectorDatabase<br/>(数据库入口)"]

        subgraph CollMgr["集合管理"]
            Collections["collections<br/>unordered_map"]
            RWLock["shared_mutex<br/>并发控制"]
            CollOps["CreateCollection / CreateCollectionWithSchema<br/>DeleteCollection / Truncate"]
        end

        subgraph SchemaMgr["Schema 管理"]
            SC["Schema 定义<br/>string/float/int/bool/text/geo"]
            SC_Validate["Schema 校验<br/>Upsert 时类型检查"]
            SC_Indexable["可索引字段标记<br/>Indexable: true → 建立倒排索引"]
        end

        subgraph DocMgr["文档管理"]
            Docs["Document 存储<br/>map[string]*Document"]
            MetaIdx["MetadataInvertedIndex<br/>元数据倒排索引"]
            BM25Idx["BM25Index<br/>全文索引（text 字段）"]
        end

        subgraph FilterEng["过滤引擎"]
            FE_Equal["FieldEqual 等式"]
            FE_Range["FieldRange 范围"]
            FE_In["FieldIn 集合"]
            FE_Not["FieldNotEqual / Not"]
            FE_Or["Or / And 组合器"]
            FE_Text["TextMatch 子串匹配"]
            FE_Geo["GeoRadius 地理半径"]
        end

        subgraph DistCalc["距离计算"]
            Cosine["cosine<br/>余弦相似度"]
            L2["euclidean<br/>欧氏距离"]
            IP["ip<br/>内积"]
        end

        subgraph SearchEng["搜索引擎"]
            BF["BruteForce<br/>全量扫描"]
            HNSW["HNSW 图搜索<br/>efSearch/efConstruction/M"]
            IVF["IVF 聚类搜索<br/>K-means + nProbe"]
            Hybrid["HybridSearch<br/>BM25 + 向量 + alpha/RRF 融合"]
            FSearch["FilteredIndex<br/>预过滤：候选集内搜索"]
        end
    end

    %% ========== 持久化层 ==========
    subgraph Persist["💾 持久化层"]
        WAL["WAL<br/>预写日志<br/>(JSON Lines)"]
        WAL_Lock["sync.Mutex<br/>写入互斥"]
        Snapshot["JSON 快照<br/>collections + documents + 索引状态"]
        WAL_Truncate["WAL 截断<br/>快照后清空"]
    end

    %% ========== 可观测性 ==========
    subgraph Observability["📊 可观测性"]
        Metrics["Prometheus 指标<br/>search_total / upsert_total / latency"]
        Explain["SearchExplanation<br/>执行计划 + 耗时"]
    end

    %% ========== 运维 ==========
    subgraph Ops["🔧 运维"]
        Config["配置管理<br/>YAML / 环境变量 / CLI"]
        Auth["API Key 认证"]
        CORS["CORS 跨域"]
        Graceful["优雅关闭<br/>SIGTERM handler"]
    end

    %% ========== 依赖关系 ==========
    Client -->|HTTP JSON| Router
    Router --> Middleware
    Middleware --> Handlers

    Handlers --> DB
    DB --> RWLock
    DB --> CollMgr
    DB --> SchemaMgr

    POST_Create & POST_Batch & POST_Upsert -->|先写 WAL| WAL
    WAL --> WAL_Lock
    WAL -->|重放恢复| DB

    POST_Upsert --> SC_Validate
    SC_Validate --> DocMgr

    DB --> FilterEng
    DB --> DistCalc
    DB --> SearchEng
    DB --> DocMgr

    POST_Search --> SearchEng
    POST_Hybrid --> SearchEng
    POST_Explain --> Explain

    SearchEng --> BF
    SearchEng --> HNSW
    SearchEng --> IVF
    SearchEng --> Hybrid
    SearchEng --> FSearch

    FSearch --> DocMgr
    BF --> DistCalc
    BF --> FilterEng
    HNSW --> DistCalc

    SDK -->|直接调用| DB
    Console --> |浏览器| Router

    subgraph Legend["图例"]
        L1["全部为实线 — 当前已实现"]
    end
```

图中清晰展示了当前完整架构：
- **三层架构**：HTTP API 层（16+ 端点 + 中间件）→ 引擎核心层 → 持久化层
- **Schema 管理**：类型化字段定义、写入时校验、自动建立倒排索引
- **过滤引擎**：7 种过滤原语，支持复合组合
- **搜索引擎**：4 条搜索路径（BF/HNSW/IVF/Hybrid）+ 预过滤
- **全文检索**：BM25 索引（text 类型字段）
- **运维能力**：配置、认证、CORS、优雅关闭
- **全部已实现**（无虚线/规划中标记）

## 3. 核心能力定义

向量数据库必须具备的能力：

### 3.1 向量存储与索引
- 向量数据的高效写入与持久化
- 支持多种索引结构：HNSW、IVF、倒排索引（元数据 + BM25）
- 支持向量替换、删除、批量写入与批量更新
- 支持 PQ 向量量化（⏳ 规划中）
- 自动分片与数据分区设计（⏳ 规划中）

### 3.2 相似度搜索
- 支持欧氏距离、余弦相似度、内积等多种相似度度量
- 支持近似最近邻（ANN）和精确搜索
- 支持 top-k 搜索（半径搜索 ⏳ 规划中）
- 支持动态搜索参数调优，如 efSearch、nProbe

### 3.3 元数据与过滤
- 向量条目关联结构化元数据字段
- 支持基于元数据字段的布尔过滤、范围过滤、文本过滤
- 支持混合查询：向量相似度 + 结构化过滤的组合查询

### 3.4 数据模型与API
- 支持多集合/空间（namespace）管理
- 支持 schema 约束、字段定义、索引配置
- 提供 HTTP/REST API，兼容常见向量数据库调用模式
- 提供 C++ 库嵌入用于集成与开发

### 3.5 可观测性与调试
- API 日志、查询耗时、写入耗时统计
- 向量索引状态、节点/分片状态、搜索命中率指标
- 支持导出指标给 Prometheus 或自定义监控
- 支持执行计划与查询解释功能

### 3.6 数据管理与运维能力
- 支持索引构建/重建
- 支持数据导入/导出、备份与恢复
- 支持版本管理、迁移流程
- 支持配置管理与运行时参数调优

### 3.7 生态与扩展
- 支持插件式距离度量、索引算法扩展
- 支持与主流 LLM/检索框架集成（如 LangChain、LlamaIndex）
- 支持向量数据与文本数据联合处理

## 4. 主流同类产品对标

Flux 的能力应覆盖或对标以下主流产品的核心维度：

- Pinecone：高性能索引、向量 + 元数据过滤、REST API
- Milvus：多种索引、分布式扩展、数据管理能力
- Weaviate：知识图谱、混合查询、元数据过滤
- Qdrant：高性能查询、向量 + 结构化过滤、REST API / gRPC
- Elasticsearch 向量功能：混合搜索、文本与向量联合查询

## 5. 控制台设计

### 5.1 目标
- 提供可交互的调试平台，**验证向量数据库所有核心能力**
- 支持向量写入、搜索、过滤、索引构建、混合搜索、召回分析
- **召回效果验证**：支持 ANN vs BruteForce 对比，展示 recall@k、精度、延迟
- 可视化展示向量查询结果、距离分布、索引状态、搜索耗时、执行计划
- 理解向量数据库内部原理

### 5.2 控制台功能模块

| 模块 | 功能 | 验证的能力 |
|------|------|-----------|
| **Dashboard** | 概览：集合数、向量总数、索引数、服务状态 | 系统健康度 |
| **Schema Designer** | 可视化创建集合 + 类型化字段 + 标记 Indexable | Schema 管理、元数据倒排索引 |
| **Data Manager** | 文档浏览/新增/编辑/删除、批量 Upsert、导入导出 | CRUD、Batch 操作、Import/Export |
| **Search Lab** | 向量搜索、混合搜索（BM25+向量）、丰富过滤条件 | HNSW/IVF/BF/HybridSearch、7 种 Filter |
| **Recall Analyzer** | ANN vs BruteForce 对比：recall@k、延迟、候选数 | 索引质量评估、参数调优 |
| **Index Manager** | 构建/删除索引、查看状态、调整 HNSW/IVF 参数 | 索引生命周期管理 |
| **Query Explainer** | 执行计划：搜索路径、耗时、过滤条件、候选数 | 搜索过程可视化 |
| **API Playground** | 交互式 API 调用、curl 命令生成 | HTTP API |

### 5.3 Recall Analyzer 设计

召回分析是控制台的核心调试功能，帮助开发者理解 ANN 索引的精度：

1. **模式**：对同一查询同时执行 ANN 搜索（HNSW/IVF）和 BruteForce 精确搜索
2. **对比指标**：
   - `recall@k` = ANN 结果中命中的 BF top-k 文档数 / k
   - `precision@k` = ANN 结果中相关文档的比例
   - `延迟对比` = ANN 耗时 vs BF 耗时
   - `候选数对比` = ANN 扫描的文档数 vs BF 扫描数
3. **可视化**：双列表并排展示 ANN 和 BF 结果，命中项高亮标记
4. **参数调优**：实时调整 efSearch（HNSW）、nProbe（IVF），观察 recall 变化

### 5.4 技术实现
- 前端：纯 HTML + CSS + JavaScript（编译期嵌入 嵌入）
- 后端：调用现有 REST API + 新增 `/collections/{name}/recall` 端点
- 新增端点 `POST /collections/{name}/recall` 在一次请求中同时执行
  ANN 搜索和 BruteForce 搜索，返回对比数据

## 6. 技术栈

- 核心引擎：C++17（GCC 13+ / Clang 17+）
- 构建系统：CMake 3.25+，vcpkg 包管理
- 距离计算：Google Highway（可移植 SIMD，x86 AVX2/AVX-512 + ARM NEON）
- HTTP 服务：cpp-httplib（单头文件，零依赖）
- JSON：nlohmann/json（单头文件）
- 日志：spdlog
- CLI：CLI11
- 配置：yaml-cpp + 环境变量覆盖
- 测试：Google Test
- 控制台：编译期嵌入 HTML/CSS/JS SPA
- 数据持久化：JSON Lines WAL + CRC32 校验 + JSON 全量快照
- 索引算法：自实现 HNSW、IVF（K-means++）、BM25Okapi
- API：HTTP REST
- 部署：单静态二进制 ~5MB，Docker 多阶段构建

## 7. 学习目标与价值

该项目的价值不仅是做出一个向量数据库产品原型，
更在于通过“构建一个可调试的向量数据库控制台”，让开发者真正理解：
- 向量数据如何存储与索引
- 相似度搜索的本质与调优空间
- 向量数据库与结构化数据混合查询的设计
- 为什么向量数据库需要特定的索引与调度能力

## 8. 未来扩展方向

- 多节点分布式部署
- 语义搜索与向量搜索融合
- 自定义距离度量与向量运算插件
- 向量聚类、近邻可解释性分析
- 主流检索框架集成和生态适配
