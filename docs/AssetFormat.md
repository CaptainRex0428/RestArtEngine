# `.rasset` 统一资产容器

> 状态：**设计提案，代码尚未实现**。当前提交只落地了 `AssetRegistry`、Content 文件夹树与场景切换；文件格式按本文实现前仍需评审。
>
> 扩展名 `.rasset`（原 `.raasset` 更名，旧文件仍可只读加载）。
>
> 已定：**GUID 作身份**（64 位随机，文件里以 16 位十六进制显示，内存仍是 `AssetId = uint64`）；**材质并入 `.rasset`**，旧 `.ramat` 只读兼容。
>
> 已实现缩略图：保存资产时用 CPU 光栅化生成 64×64 rgba8 预览（网格取最大两个轴向正交投影，材质为着色球），作为 `thumbnail` 数据块写入容器清单；Content 面板按需读取并直接绘制，缺缩略图时回退线框图标。内置图元（Sphere / Plane / Cube）在工程首次打开时生成到 `Content/Meshes/Builtin/*.rasset`（生成物、已 gitignore；存在文件时文件优先）；容器读写库 `src/Assets/AssetFile.cpp`；材质与网格都以 `.rasset` 持久化（材质参数在清单、网格几何在数据块），`AssetRegistry` 从清单读类型/GUID/名称；场景格式升到 **v3**，只保存材质与网格的 GUID 引用，加载时从 `Content/Materials`、`Content/Meshes` 解析。旧 v1/v2 场景仍可读。

## 目标

- **自包含**：几何、材质参数、缩略图与元数据放在同一个文件里，可以整体复制、移动、提交版本库，不依赖外部 OBJ 与场景内嵌副本。
- **单一真相**：资产只存一份，场景只引用资产与路径，不再把材质/网格几何抄进 `.rascene`。
- **可演进**：头部带版本号，清单是 JSON，未知字段可忽略，读不了的版本明确报错。
- **可快速浏览**：Content 浏览器只读文件头与清单，不解码大数据块；缩略图可选。

## 容器布局

全部整数小端。

| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 4 | magic，ASCII `RAAS` |
| 4 | 2 | `version`，当前为 `1` |
| 6 | 2 | `flags`：bit0 = 含缩略图，其余保留为 0 |
| 8 | 8 | `manifestSize`（字节） |
| 16 | `manifestSize` | 清单 JSON（UTF-8，无 BOM） |
| 16 + `manifestSize` | 剩余 | 数据块区，按清单里的 `offset` / `size` 引用 |

数据块 `offset` 相对**数据块区起点**，`size` 为字节数；读取器必须校验 `offset + size` 不越界，否则拒绝加载而不是崩溃。

## 清单（manifest）

```json
{
  "format": "RAEngine.Asset",
  "version": 1,
  "type": "staticMesh",
  "id": 16,
  "name": "Sphere",
  "source": { "tool": "RestArtEngine", "created": "2026-10-27T12:00:00Z" },
  "thumbnail": { "offset": 0, "size": 4096, "format": "png" },
  "payload": {}
}
```

- `type`：`staticMesh` / `material` / `scene` / `texture`（后续扩展用新值，读取器遇到未知类型只展示、不加载）。
- `id`：稳定标识，**策略待定**（GUID / 内容哈希 / 项目内自增），本阶段先允许为 0。
- `name`：显示名，可重命名而不用改文件名。

### `staticMesh` payload

```json
{
  "vertexCount": 24,
  "indexCount": 36,
  "vertexStride": 24,
  "indexFormat": "u32",
  "bounds": { "min": [-1, -1, -1], "max": [1, 1, 1] },
  "blobs": {
    "vertices": { "offset": 4096, "size": 576 },
    "indices": { "offset": 4672, "size": 144 }
  }
}
```

- 顶点布局沿用 `MeshVertex`：`position[3] + normal[3]`，步长 24 字节。
- 内置图元（Sphere / Plane / Cube）也走同一格式，由引擎生成后缓存，不再有"保留 ID 1–3"的特殊分支。

### `material` payload

材质作为一个独立的 `.rasset` 文件（例如 `Content/Materials/Copper.rasset`），场景只引用它，不再把参数抄进 `.rascene`。编辑器里的材质草稿、Apply/Discard 语义不变，落到同一个容器格式。

```json
{
  "baseColor": [0.85, 0.28, 0.08],
  "roughness": 0.25,
  "metallic": 0.65,
  "grid": false,
  "twoSided": false,
  "textures": { "baseColor": "textures/copper.rasset" }
}
```

### `scene` payload

迁移现有 `.rascene`：`actors` 保留，`materials` / `meshes` 数组改为引用列表：

```json
{
  "actors": [ { "id": 1, "name": "Sphere", "mesh": "meshes/sphere.rasset", "material": "materials/copper.rasset" } ],
  "background": "black"
}
```

相对路径以场景文件所在目录为基准，和现在的 OBJ `sourcePath` 规则一致；移动场景时重算（`moveContentEntry` 已实现同样的重定位逻辑）。

### `texture` payload（后续）

`width` / `height` / `format`（`rgba8`、`bc7`…）/ `mipCount`，数据块按 mip 顺序排列。

## 读写策略

1. **浏览**：`AssetRegistry` 读头部与清单，得到类型、名称、ID、缩略图偏移；不解码几何。
2. **加载**：按清单读取数据块，校验边界与长度，失败返回明确错误；场景加载失败不得破坏当前场景（现有 `World::load` 已是这种事务式实现）。
3. **保存**：先写临时文件，`fflush` 后原子替换，避免半写文件；清单先序列化，再写数据块，最后回填偏移。

## 迁移步骤

1. ✅ `AssetRegistry` + Content 文件夹树 + 场景切换（本次提交）。
2. 实现 `.rasset` 读写库与单元测试：magic/版本、清单解析、越界块、损坏文件。
3. ✅ OBJ 导入生成 `.rasset`（GUID），场景引用资产；保留旧场景 `.obj` 读取兼容。
4. 材质从 `.ramat` 迁入 `.rasset`：读取旧 `.ramat` 后转存为新容器，材质编辑器改为保存 `.rasset`；旧文件只读兼容一个版本后移除。
5. 删除场景内嵌的 `materials` / `meshes` 几何，只留引用。

## 未决问题

- 网格仍内嵌在场景（内置描述 + OBJ 相对路径），迁移为 `.rasset` 网格资产是下一步。
- 缩略图生成时机（保存时渲染 vs 编辑器后台）。
- 是否压缩数据块（deflate）以及 4 字节对齐要求。
- 二进制与 JSON 混合的调试体验：是否需要 `--dump-asset` 工具输出清单。
