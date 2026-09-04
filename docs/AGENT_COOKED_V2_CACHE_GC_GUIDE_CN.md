# PrismRender Stage 17-H：Cooked Asset v2 与缓存 GC 学习指南

## 1. 阶段目标

Stage 17-F 建立了 `.prismmesh/.prismtex/.prismmat`，但 v1 只是：

```text
16 字节 Header
  + 顺序序列化 Payload
```

它无法回答：

- 文件是否在磁盘、传输或写入过程中损坏。
- Payload 原始大小和实际存储大小分别是多少。
- 是否使用压缩以及如何解压。
- 容器版本和内部数据 Schema 版本是否可以独立演进。
- 旧内容哈希产生的源缓存和 Cooked 资产何时可以清理。

Stage 17-H 完成：

1. Cooked Asset v2 容器。
2. Payload FNV-1a 64 位校验。
3. 可选 RLE 压缩与无收益时自动回退。
4. v1 Cooked 文件兼容读取。
5. 写后立即重新读取并验证。
6. 内容哈希级 Asset Cache GC。
7. dry-run、年龄保护、容量预算和结构化 GC 报告。
8. Harness `asset.cache.gc` 命令。

## 2. 文件变更

### 2.1 主要修改

- `src/Asset/CookedAssetIO.h`
- `src/Asset/CookedAssetIO.cpp`
- `src/Asset/AssetCache.h`
- `src/Asset/AssetCache.cpp`
- `src/Asset/AssetDatabase.cpp`
- `src/Asset/AssetRuntimeLoader.cpp`
- `src/Automation/HarnessTools.h`
- `src/Automation/HarnessTools.cpp`
- `tests/EngineHarnessTests.cpp`
- `docs/LEARNING_GUIDE_CN.md`

### 2.2 新增

- `docs/AGENT_COOKED_V2_CACHE_GC_GUIDE_CN.md`
- `examples/harness/cooked_v2_cache_gc.jsonl`

## 3. Cooked v2 总体数据流

```text
MeshAsset / TextureAsset / MaterialAsset
  -> 业务 Payload 序列化
  -> 计算 FNV-1a 64 Payload Checksum
  -> 尝试 RLE 压缩
  -> 压缩后更小：保存 RLE
  -> 压缩无收益：保存原始 Payload
  -> 写入 .tmp
  -> 重新读取、解压、检查长度和 Checksum
  -> 原子替换最终 .prismmesh/.prismtex/.prismmat
  -> Asset Manifest 写入 v2 元数据
```

读取路径：

```text
读取 Magic + Container Version
  -> version 1：按旧 Payload 读取
  -> version 2：读取大小、Codec、Schema 和 Checksum
      -> 解压
      -> 校验 Payload 长度
      -> 校验 Payload Checksum
  -> 解析 Mesh/Texture/Material Payload
  -> 创建运行时 RHI 资源
```

## 4. Cooked v2 文件布局

v2 Header 固定为 56 字节：

```cpp
struct CookedHeaderV2
{
    char magic[8];                  // PRCOOKED
    uint32_t version;               // Container Version = 2
    uint32_t kind;                  // Mesh / Texture / Material
    uint32_t headerBytes;           // 56
    uint32_t payloadVersion;        // 当前为 1
    uint32_t compression;           // None / RunLength
    uint32_t flags;                 // 当前必须为 0
    uint64_t uncompressedBytes;
    uint64_t storedBytes;
    uint64_t payloadChecksum;
};
```

### 4.1 Container Version

描述文件容器如何组织：

- Header 布局。
- 压缩字段。
- 长度字段。
- 校验和规则。

### 4.2 Payload Version

描述 Mesh、Texture 或 Material 内部字段顺序。

Container 和 Payload 分开版本化后，未来可以：

- 保持 v2 容器不变，只升级 Mesh Payload。
- 增加新的压缩 Codec，但保持业务数据不变。
- 编写明确的 Payload 迁移器。

## 5. Payload 校验

Checksum 对解压后的原始 Payload 计算：

```text
payloadChecksum = FNV1a64(uncompressedPayload)
```

读取顺序必须是：

1. 检查 `storedBytes` 和 `uncompressedBytes` 安全上限。
2. 读取完整存储数据。
3. 解压。
4. 检查解压后的长度。
5. 计算 Checksum。
6. Checksum 一致后才解析业务字段。

这样损坏数据不会进入：

- `MeshVertex` 数组分配。
- Texture GPU 上传。
- Material 依赖解析。

`CookedAssetIO::Inspect` 也执行完整解压和校验，可用于 Harness 状态查询，而不创建运行时资产。

## 6. RLE 压缩

当前 v2 使用轻量级字节 RLE：

- 控制字节最高位为 `0`：后续是 1 到 128 字节 Literal。
- 控制字节最高位为 `1`：后续一个字节重复 3 到 130 次。

压缩器只有在：

```text
compressedBytes < originalBytes
```

时才选择 RLE，否则保存原始 Payload。

优点：

- 无第三方依赖。
- 实现小，便于学习和审计。
- 对纯色纹理、Fallback Texture、重复标志和零填充数据有效。

局限：

- 不适合作为最终大型纹理和几何压缩方案。
- 对高熵 RGBA 数据收益有限。
- 不替代 BCn/ASTC、Mesh Optimization 或 Zstandard。

未来可在 `compression` 字段增加 Zstandard，但必须保留 None 与 RLE 的读取兼容。

## 7. v1 向后兼容

`CookedAssetIO::IsSupportedVersion` 当前接受：

- v1：Legacy 顺序 Payload。
- v2：带长度、压缩和校验的容器。

运行时 `ResolveCookedPath` 不再要求 Manifest 版本必须等于最新版，只要版本属于支持集合即可。

v1 没有原始 Checksum，因此：

```text
checksumVerified = false
```

这不代表 v1 一定损坏，只表示它无法提供 v2 级完整性证明。

新导入和重导入始终写 v2。旧项目可以逐个执行 `asset.reimport`，不需要一次性删除全部缓存。

## 8. 写后验证

写入过程使用：

```text
target.prismtex.tmp
  -> 写容器
  -> 重新打开
  -> 解压和 Checksum 校验
  -> 成功后替换 target.prismtex
```

如果 `.tmp` 校验失败：

- 删除临时文件。
- 保留原有正式文件。
- 导入命令返回失败。
- Manifest 不应把损坏文件声明为有效资产。

`AssetDatabase::SetCookedMetadata` 还会进行第二次 `Inspect`，并把以下信息写入 Manifest：

- `version`
- `payloadVersion`
- `compression`
- `uncompressedBytes`
- `storedBytes`
- `payloadChecksum`
- `checksumVerified`

## 9. Asset Cache GC 模型

PrismRender 有两个内容寻址根目录：

```text
automation/cache/assets/<contentHash>
automation/cache/cooked/<contentHash>
```

GC 把同一 `contentHash` 的两个目录视为一个清理单元。

这样不会出现：

- 删除了源 Bundle，但遗留 Cooked 文件。
- 删除了 Cooked 文件，但遗留源依赖副本。

## 10. 引用扫描

当前引用集合来自 Asset Manifest 中所有导入 Scene 的 `contentHash`：

```text
Asset Manifest
  -> GetImportedScenes()
  -> referencedContentHashes
```

GC 规则：

- 引用中的 Hash 永不删除。
- 未引用 Hash 是候选。
- 非法目录名不会进入删除候选。
- 删除目标必须是缓存根目录的直接子目录。

执行删除前再次验证：

```text
target.parent_path == expectedCacheRoot
```

避免路径拼接错误造成越界删除。

## 11. 访问时间

每次成功 Build 或 Resolve Bundle 时写：

```text
automation/cache/assets/<contentHash>/access.json
```

格式：

```json
{
  "format": "PrismAssetCacheAccess",
  "version": 1,
  "lastAccessUnixMilliseconds": 1784190000000
}
```

访问时间写入失败不会使原本有效的缓存失效。旧缓存没有 `access.json` 时被视为未知且较旧，便于清理历史遗留内容。

## 12. GC 策略

参数：

```json
{
  "dryRun": true,
  "minimumUnusedAgeSeconds": 604800,
  "maximumBytes": 21474836480
}
```

### 12.1 第一阶段：年龄清理

选择：

```text
未引用 && 未使用时间 >= minimumUnusedAgeSeconds
```

### 12.2 第二阶段：容量收敛

如果第一阶段后仍超过 `maximumBytes`：

- 按最后访问时间从旧到新排序。
- 继续选择未引用条目。
- 引用中的条目仍不删除。

如果仅引用中的缓存就超过预算：

```text
budgetSatisfied = false
```

GC 不会为了满足容量强行破坏当前 Manifest。

## 13. dry-run 与真实执行

新命令：

```json
{
  "requestId": "cache-gc-preview",
  "command": "asset.cache.gc",
  "arguments": {
    "dryRun": true,
    "minimumUnusedAgeSeconds": 604800,
    "maximumBytes": 21474836480
  }
}
```

返回：

- `bytesBefore`
- `bytesAfter`
- `reclaimableBytes`
- `reclaimedBytes`
- `referencedEntryCount`
- `selectedEntryCount`
- `removedEntryCount`
- 每个 Hash 的选择原因

建议 Agent 操作顺序：

1. 调用 `asset.cache.status`。
2. 调用 `asset.cache.gc` 且 `dryRun=true`。
3. 检查所有 `selected` 条目均为 `referenced=false`。
4. 检查 `reclaimableBytes`。
5. 使用相同参数改为 `dryRun=false`。
6. 再次调用 `asset.cache.status`。

## 14. Harness 能力

`engine.describe` 新增：

- `assetCacheGarbageCollection`
- `assetCacheGcDryRun`
- `cookedAssetV2`
- `cookedAssetCompression`
- `cookedAssetChecksums`

`asset.cache.status` 对每个 Cooked 资产新增：

- Container/Payload Version。
- Compression。
- Uncompressed/Stored Bytes。
- Payload Checksum。
- Checksum 是否验证。
- 结构化错误。

## 15. 自动测试

`EngineHarnessTests` 新增验证：

1. 新导入文件是 v2。
2. v2 Checksum 验证成功。
3. v1 Mesh 仍可读取。
4. 重复纹理选择 RLE。
5. Literal/Run 处于 128 字节边界时完整往返。
6. 篡改最后一个字节后读取和 Inspect 都失败。
7. GC dry-run 只选择未引用 Hash。
8. dry-run 不删除文件。
9. 真实 GC 删除源 Bundle 和 Cooked 目录。
10. 引用 Hash 保持存在。
11. Harness 暴露 GC 和 v2 能力。

## 16. 验证结果

2026-07-17 Debug 构建验证：

- 完整编译成功。
- CTest 7/7 通过。
- `StartupScene.gltf` 重导入成功。
- 实际 Cooked 资产 7/7 通过 v2 校验。
- 实际 7 个资产均选择 RLE。
- D3D12 从 v2 资产完成 640x360 Tonemap 截图。
- Vulkan 从同一批 v2 资产完成 640x360 Tonemap 截图。
- GC dry-run 正确识别当前内容 Hash 为引用中，未选择删除。

验证命令：

```powershell
cmake --build build-windows-ci --config Debug --parallel 8
ctest --test-dir build-windows-ci -C Debug --output-on-failure
```

示例：

```powershell
Get-Content examples/harness/cooked_v2_cache_gc.jsonl |
  .\build-windows-ci\Debug\PrismHarness.exe
```

## 17. 阅读顺序

1. `src/Asset/CookedAssetIO.h`
2. `src/Asset/CookedAssetIO.cpp`
3. `src/Asset/AssetDatabase.cpp`
4. `src/Asset/AssetRuntimeLoader.cpp`
5. `src/Asset/AssetCache.h`
6. `src/Asset/AssetCache.cpp`
7. `src/Automation/HarnessTools.cpp`
8. `tests/EngineHarnessTests.cpp`

先理解容器，再理解导入/运行时，最后看 GC 和 Agent 命令。

## 18. 下一阶段

建议继续：

1. Stage 17-I 已完成离线 Minidump 符号化工具和 Build/PDB 身份匹配。
2. Stage 17-J 已完成 RDG Pass Culling。
3. Stage 17-J 已完成 Transient Resource 生命周期与别名槽规划。
4. Stage 17-K 已完成真实 Alias Memory、Aliasing Barrier 和 Compute Queue 提交探针。
5. 实现真正的多队列 RDG Pass 执行、Subresource Tracking 和 MCP Agent Adapter。

后续实现顺序见 `docs/AGENT_RDG_NATIVE_RESOURCE_QUEUE_GUIDE_CN.md`。
