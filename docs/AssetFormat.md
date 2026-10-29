# `.rasset` 统一资产容器

> 状态：**已实现**。容器读写、GUID、材质与网格资产、内置图元、64px 缩略图、场景 v3 的 GUID 引用都已落地；仍缺纹理载荷、数据块压缩与调试导出工具。
>
> 扩展名 `.rasset`（由 `.raasset` 更名；旧 `.raasset` 仍可只读加载，保存只写新后缀）。
>
> 已定：**GUID 作身份**（64 位随机，文件里以 16 位十六进制显示，内存仍是 `AssetId = uint64`）；**材质并入 `.rasset`**；旧 `.ramat` 只读兼容，且保存新容器时**不删除**旧文件。
>
> 缩略图：保存资产时用 CPU 光栅化生成 64×64 RGBA8（网格取最大两个轴向正交投影，材质为着色球），作为 `thumbnail` 数据块写入清单；Content 面板直接读取绘制，缺失时回退线框图标。场景缩略图不写进场景 JSON：通过带 renderer 的保存路径（如 `Save All`）用 GPU 渲染 128px 写到同目录 `<scene>.rthumb`（magic `RTHB`，已 gitignore），缺失时回退 Actor 布局的 CPU 示意图。内置网格随引擎存放在 `Content/Meshes/*.rasset`；编辑器启动不再生成项目 `Meshes/Builtin`，旧文件和旧 ID 仍可读取。容器读写库在 `src/Assets/AssetFile.cpp`。

## 目标

- **自包含**：几何、材质参数、缩略图与元数据放在同一个文件里，可以整体复制、移动、提交版本库，不依赖外部 OBJ 与场景内嵌副本。
- **单一真相**：资产只存一份，场景只引用资产 ID，不再把材质/网格几何抄进 `.rascene`。
- **可演进**：头部带版本号，清单是 JSON，未知字段可忽略，读不了的版本明确报错。
- **可快速浏览**：Content 浏览器只读文件头与清单，不解码大数据块；缩略图可选。

## 引擎资产与项目资产

引擎目录的 `Content/Materials/DefaultMaterial.rasset` 提供中性灰默认材质，固定 ID 为 `0010000000000001`。`0010000000000000`～`0010000000000fff` 保留给引擎资产；新项目材质与导入网格生成随机 ID 并避开此区间。历史 Copper/Ground 的 ID 1/2 和已有项目 GUID 保持兼容，项目文件占用保留区间时报告冲突，不能覆盖引擎资产。

`AssetOrigin` 是挂载器赋予的运行时归属，不从项目文件接受。项目和引擎材质共同进入 AssetLibrary；项目重载保留引擎项，项目保存跳过引擎项。`createMaterial` 在引擎默认材质已挂载时继承其参数并生成项目 ID；纯资产库未挂载引擎时保留原有参数默认值。场景 v3 继续保存数值 ID 引用，只有实际被 Actor 使用的引擎材质才写入引用清单；找不到引用仍然报错。v1 内嵌材质加载保留已挂载的引擎资产，v2/v3 引用加载同样可解析它们。

Content 浏览器保留原有项目相对路径；引擎项使用 `engine://Materials/DefaultMaterial.rasset` 形式的浏览器键。这是来源标识，不是磁盘路径，也不替代场景资产 ID。只读限制覆盖编辑器参数、文件操作与保存入口；`Copy to Project` 生成新项目 GUID 和独立 `.rasset`，共享参数与资产身份分开处理。C++ 资产结构仍可由调用方直接修改，这不是文件系统权限隔离。

引擎还分发 `Content/Meshes/{Cube,Sphere,Cylinder,Cone,Torus,Plane}.rasset`，包含实际几何和 64px 缩略图。固定 ID 分别为 `0010000000000101`～`0010000000000106`，与旧项目 Sphere/Plane/Cube 的 1/2/3 独立。旧场景（包括尚未迁移的 Sandbox 引用）仍兼容原 ID；新建场景使用引擎网格，启动不再自动生成项目 Builtin 文件。保留区间适用于引擎材质和网格。

`loadEngineMeshes` 挂载这六个只读资产，缺失、损坏或重复 ID 会整体失败；项目网格不得占用引擎保留 ID。项目保存和旧图元生成均跳过引擎网格，v3 场景只记录实际使用的引擎网格和旧兼容图元 ID；v1/v2 加载保留已挂载的引擎网格。容器内几何自包含，`sourcePath` 仅表示来源，保存／加载不依赖源 OBJ／FBX 存在。

六种网格都以原点为中心、Y 轴向上。Plane 沿用旧地面的 24×24 尺寸，位于 Y=0，法线向上；新建场景优先引用 Engine Sphere／Plane。Cube 边长 2；Sphere 半径 1；Cylinder 半径 1、高 2 且两端封闭；Cone 底半径 1、高 2 且底面封闭；Torus 主半径 0.7、管半径 0.3。Cube/Sphere/Plane 保留解析图元标记，其他三种沿用通用三角网格标记 `Imported`（不表示需要外部文件）。当前顶点格式尚无 UV。可显式构建 CMake 的 `GenerateEngineMeshes` 目标，再运行 `GenerateEngineMeshes <输出目录>` 重建容器；普通构建和启动直接使用随库文件。

标准材质使用工程的共享 Shader；引擎材质只读不意味着禁用工程 Shader 编辑。

## 运行时几何缓存

`MeshAsset::geometryKey()` 提供几何版本、顶点／索引存储地址和长度，用于 GPU 网格、拾取加速与包围盒缓存；它不写入 `.rasset`，也不改变资产 GUID。加载新资产或更换存储会使缓存失效。直接原位修改顶点、法线或索引（包括保持长度的内容赋值）后，必须调用 `markGeometryModified()`；只修改 Actor Transform 或材质参数不修改几何版本。

`localBounds()` 按版本缓存局部包围盒。`MeshQueryCache` 按相同版本构建局部空间 BVH，查询保留实际三角面、单双面和最近命中语义；移除／替换资产时清理，按 256 MiB 预算淘汰较旧网格（单个超预算网格保留到下一次淘汰或清理）。首次命中未缓存的大型网格仍有构建成本，后续查询复用，几何不做减面或近似替换。

## 容器布局

全部整数小端。

| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 4 | magic，ASCII `RAAS` |
| 4 | 2 | `version`，当前为 `1` |
| 6 | 2 | `flags`：保留位，当前写出 `0`；是否有缩略图以清单 `thumbnail` 字段为准 |
| 8 | 8 | `manifestSize`（字节） |
| 16 | `manifestSize` | 清单 JSON（UTF-8，无 BOM） |
| 16 + `manifestSize` | 剩余 | 数据块区，按清单里的 `offset` / `size` 引用 |

数据块 `offset` 相对**数据块区起点**，`size` 为字节数；读取器校验 `offset + size` 不越界，否则拒绝加载而不是崩溃。

## 清单（manifest）

```json
{
  "format": "RAEngine.Asset",
  "version": 1,
  "type": "staticMesh",
  "id": "3f2a9c7d51b84e60",
  "name": "Sphere",
  "thumbnail": { "offset": 0, "size": 16384, "width": 64, "height": 64, "format": "rgba8" },
  "payload": {}
}
```

- `type`：`staticMesh` / `material`。场景仍是 `.rascene` JSON，不装进容器；`texture` 是后续类型。
- `id`：GUID，取 64 位随机数；文件里写成 16 位十六进制，可与文件名无关。
- `name`：显示名，可重命名而不用改文件名。
- `thumbnail`：RGBA8 原始像素块，`offset`/`size` 相对数据块区；没有缩略图时整个字段省略。
- `payload`：类型相关的描述数据，见下。

### `staticMesh` payload

```json
{
  "primitive": "sphere",
  "vertexCount": 6305,
  "indexCount": 12096,
  "vertexStride": 24,
  "indexFormat": "u32",
  "bounds": { "min": [-1, -1, -1], "max": [1, 1, 1] },
  "blobs": {
    "vertices": { "offset": 0, "size": 151320 },
    "indices": { "offset": 151320, "size": 48384 }
  }
}
```

- 顶点布局沿用 `MeshVertex`：`position[3] + normal[3]`，步长 24 字节；索引为 `u32`。
- `primitive` 标记内置图元类型（`sphere` / `plane` / `cube`，OBJ／FBX 导入为空或省略），加载时据此走解析光追或三角形路径。
- OBJ／FBX 导入的网格额外带 `sourcePath`（当前保留导入时的来源路径，可能是绝对路径；仅作来源信息，不再依赖源文件加载，分享容器前应检查该元数据）。

### `material` payload

```json
{
  "baseColor": [0.85, 0.28, 0.08],
  "roughness": 0.25,
  "metallic": 0.65,
  "grid": false,
  "twoSided": false,
  "cullMode": "back",
  "depthTest": "less",
  "depthWrite": true
}
```

材质是独立资产（如 `Content/Materials/Copper.rasset`），场景只引用 GUID。旧 `.ramat` 文件保留在磁盘上可继续读取；材质编辑器保存时写 `.rasset`，不删除旧文件。`textures` 尚未实现。

`cullMode` 为 `off`／`front`／`back`，`twoSided` 作为兼容字段与 `off` 对应，同时提供且相互矛盾时拒绝加载。缺少 `cullMode` 时按原 `twoSided` 解析；现代容器缺少两字段时为 Back，旧 `.ramat`／场景快照为 Off。`depthTest` 使用 `never`、`less`、`equal`、`lessEqual`、`greater`、`notEqual`、`greaterEqual`、`always`、`disabled` 字符串，缺省为 `less`；`depthWrite` 缺省为 `true`。未知值或错误类型拒绝加载，保留原内存资产。这些字段为后端无关的材质状态，GPU 原生枚举与缓存不写入资产。

## 场景与场景缩略图

场景仍是可读 JSON `.rascene`，**v3** 只引用材质与网格的 GUID（JSON 里保存数值 ID，容器清单里才写成 16 位十六进制）：

```json
{
  "format": "RAEngine.Scene",
  "version": 3,
  "actors": [
    { "id": 1, "name": "Sphere", "mesh": { "meshId": 1, "materialId": 3 } }
  ],
  "materials": [ { "id": 3 } ],
  "meshes": [ { "id": 1 } ]
}
```

加载 v3 时从 `Content/Materials`、`Content/Meshes` 解析引用；缺失时报 `Scene references material N that is not loaded from Content/Materials.`。v1（内嵌材质快照 + 内嵌网格描述）和 v2（引用材质）仍可读取，用于打开旧工程。

场景缩略图是例外：它不写进 `.rascene`，而是通过带 renderer 的保存路径由 GPU 渲染 128×128 写到同目录 `<scene>.rthumb`：

| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 4 | magic，ASCII `RTHB` |
| 4 | 4 | `version`，当前为 `1` |
| 8 | 4 | `width` |
| 12 | 4 | `height` |
| 16 | 剩余 | RGBA8 像素 |

Content 浏览器优先读该文件；不存在或读取失败时回退到按 Actor 位置生成的 CPU 布局图。

## 读写策略

1. **浏览**：`AssetRegistry` 读头部与清单，得到类型、名称、GUID、大小与缩略图；不解码几何。
2. **加载**：按清单读取数据块，校验边界与长度，失败返回明确错误；场景加载失败不破坏当前场景。
3. **保存**：普通保存直接覆盖写目标文件（中途崩溃可能留下半写文件，属于待改进项）。新资源导入只写新文件，使用临时文件与排他发布，失败清理；见 [统一导入规范](Importing.md)。
4. **移动**：Content 内拖拽移动资产时更新其相对路径；移动旧场景时仅重写 `meshes[].sourcePath` 相对引用（`moveContentEntry`）；v3 的资产 GUID 不随文件位置改变，`.rthumb` 随文件移动；目标冲突会拒绝操作。目录整体移动不支持。

## 迁移步骤

1. ✅ `AssetRegistry` + Content 文件夹树 + 场景切换。
2. ✅ `.rasset` 读写库与测试：magic/版本、清单解析、越界块、损坏文件、旧 `.raasset` 可读。
3. ✅ OBJ／FBX 通过统一入口导入生成 `.rasset`（GUID），场景引用资产；旧场景 `.obj` 读取兼容。
4. ✅ 材质从 `.ramat` 迁入 `.rasset`；旧文件保留只读兼容，保存时不再删除。
5. ✅ 场景 v3 删除内嵌材质/几何，只留 GUID 引用；v1/v2 仍可读。
6. ⬜ 纹理资产（`texture` payload、采样参数与 Content 识别）。

## 当前使用限制

- 网格从整个项目 Content 递归加载，导入到当前项目目录；材质仍从 `Materials` 目录树加载。保存按 GUID 查找已有文件路径，保留子目录与改名结果；新资产同名时生成不冲突的文件名。材质仍不能移出对应类型根。
- `initialize` 和 Content 的 `openScene` 均先加载材质和网格再解析场景；加载错误明确返回。普通 `Load Scene` 保持使用当前内存资产的行为。
- 普通 `Save Scene`／Ctrl+S 调用无 renderer 的保存重载，当前不会生成或刷新 GPU sidecar。旧 sidecar 可能过时；sidecar 缺失或读取失败时回退 CPU 示意图。
- `.rasset` 与旧 `.ramat`／`.raasset` 同类型同 GUID 时优先现代容器；浏览器默认隐藏已替代旧文件，可在 View 中显示。旧文件仍保留，保存不保证两份同步更新；无替代文件的旧资产仍显示。

## 未决问题

- 数据块是否压缩（deflate）、4 字节对齐要求，以及大网格的流式加载。
- 网格/材质缩略图目前是 CPU 光栅化；是否改成 GPU 渲染并缓存（场景已走 GPU sidecar）。
- 保存的原子性：临时文件＋rename，避免断电或崩溃留下半写资产。
- 是否需要 `--dump-asset` 工具输出清单与块表，方便排查损坏文件。

场景还可包含可选的 `camera` 与 `lighting` 配置；它们是场景级配置。`camera` 包含 `yaw/pitch/distance/target/exposure`，表示显式存储的初始视角。`lighting` 包含 `directionalIntensity/environmentIntensity` 与 `areaLight`（`enabled/center/halfAxisU/halfAxisV/radiance`）；半轴为非零正交世界空间向量，叉积指向单面发光侧。数值必须有限，能量非负，视距／俯仰／曝光遵循编辑器范围；解析或保存校验失败保留原场景／已发布文件。缺少字段的旧场景不启用场景照明覆盖，也不强制覆盖当前相机。矩形发光面的渲染范围见 [GI 文档](GlobalIllumination.md#单矩形发光面示例)。
