# Proximia

用 Go 实现的向量数据库引擎。核心算法自实现，零第三方依赖。

## 快速开始

```bash
go run github.com/wzhongyou/proximia/cmd/proximia@latest
```

打开 http://localhost:8080 进入控制台。

从源码运行：

```bash
git clone https://github.com/wzhongyou/proximia.git
cd proximia
go run ./cmd/proximia
```

## 定位

Proximia 不是 Milvus 或 Qdrant 的替代品。适用场景：

- **嵌入使用**：`import "github.com/wzhongyou/proximia/pkg/proximia"` 在进程内调用
- **边缘/私有部署**：单二进制，无需 Kubernetes、etcd、对象存储
- **CI/测试环境**：`go run` 秒级启动，无需 Docker
- **中小规模**：百万级以下向量，单节点够用
- **学习/调试**：5350 行代码，一天可读完

## 能力

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
| 运维 | 健康检查、API Key 认证、CORS、YAML/env/CLI 配置、优雅关闭 |
| 控制台 | 8 页面 SPA（Dashboard/Schema/Data/Search/Recall/Index/Explain/API） |
| 部署 | 单二进制，Docker 镜像 ~15MB |
| 依赖 | 仅 Go 标准库 |
