# `.rasset` 统一资产容器

> 状态：**已实现**。容器读写、GUID、材质与网格资产、内置图元、64px 缩略图、场景 v3 的 GUID 引用、v4 灯光组件与 v5 材质 Slot 覆盖都已落地；仍缺纹理载荷、数据块压缩与调试导出工具。
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

六种网格都以原点为中心、Y 轴向上。Plane 沿用旧地面的 24×24 尺寸，位于 Y=0，法线向上；新建场景优先引用 Engine Sphere／Plane。Cube 边长 2；Sphere 半径 1；Cylinder 半径 1、高 2 且两端封闭；Cone 底半径 1、高 2 且底面封闭；Torus 主半径 0.7、管半径 0.3。Cube/Sphere/Plane 保留解析图元标记，其他三种沿用通用三角网格标记 `Imported`（不表示需要外部文件）。随库的这六个网格当前不含 UV；导入网格可在独立可选数据流中保存 UV0–UV3。可显式构建 CMake 的 `GenerateEngineMeshes` 目标，再运行 `GenerateEngineMeshes <输出目录>` 重建容器；普通构建和启动直接使用随库文件。

标准材质使用工程的共享 Shader；引擎材质只读不意味着禁用工程 Shader 编辑。

## 运行时几何缓存

`MeshAsset::geometryKey()` 提供几何版本、顶点／索引／UV 流存储地址和长度以及 section 存储信息，用于 GPU 网格、拾取加速、包围盒与增量持久化；它不写入 `.rasset`，也不改变资产 GUID。加载新资产或更换存储会使缓存失效。直接原位修改顶点、法线、索引、UV 或 section（包括保持长度的内容赋值）后，必须调用 `markGeometryModified()`；只修改 Actor Transform 或材质参数不修改几何版本。场景保存不访问几何载荷，也不缓存几何验证。

`localBounds()` 按版本缓存局部包围盒。`MeshQueryCache` 按相同版本构建局部空间 BVH，查询保留实际三角面、单双面和最近命中语义；移除／替换资产时清理，按 256 MiB 预算淘汰较旧网格（单个超预算网格保留到下一次淘汰或清理）。首次命中未缓存的大型网格仍有构建成本，后续查询复用，几何不做减面或近似替换。

## 增量网格保存

成功加载现代容器、完成导入发布或成功保存后，`AssetLibrary` 记录项目 Imported 网格的持久化基线。后续保存比较几何 key、Slot 元数据与默认材质 GUID、UV 名称、网格名称、图元类型、来源路径、目标路径和文件大小／修改时间；全部匹配时复用原文件，不打包几何、不重新生成缩略图、不写盘。修改、换目标、文件缺失／状态变化及旧 `.raasset` 迁移仍会保存。已修改但未被场景使用的项目网格也会正常落盘；引擎网格仍只读。

文件状态检查不是内容哈希：外部编辑若保持大小和修改时间相同，不保证被发现。调用方不得用 `rememberMeshPersisted` 将未实际发布的数据标为已保存。保存失败不建立新基线，修复原因后可重试。首次或实际变更的大网格保存仍为同步操作；容器写盘并非原子替换。

`saveMeshes` 在需要实际写入时先验证几何名称、三角形数量、索引范围、有限位置／法线和非零法线；失败不触碰原容器。`saveMaterial(id, folder, error)` 只保存一个项目材质，Engine 显式保存被拒绝；`saveMaterials` 用于批量保存。现代材质的 GUID、名称和 payload 与磁盘一致时跳过重写；保存一个材质不会保存场景或其他材质。

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

### 多材质分区与 UV 流

含新增属性的网格额外写入 `payload.meshAttributesVersion: 1`，未知值或非整数拒绝加载。原来没有属性的网格省略这些字段，保留原有 24 字节顶点布局和容器内容。新属性由当前引擎读取；不保证旧版引擎能正确解释新资产。

```json
{
  "meshAttributesVersion": 1,
  "materialSlots": [
    { "name": "Paint", "sourceKey": "obj:Paint", "defaultMaterialId": 0 },
    { "name": "Glass", "sourceKey": "obj:Glass", "defaultMaterialId": 0 }
  ],
  "sections": [
    { "firstIndex": 0, "indexCount": 6, "materialSlot": 0 },
    { "firstIndex": 6, "indexCount": 6, "materialSlot": 1 }
  ],
  "uvChannels": [
    { "channel": 0, "name": "UV0", "blob": { "offset": 336, "size": 96 } }
  ]
}
```

上例为附加字段示意；blob 偏移和大小由实际顶点／索引块决定。每个 UV blob 是与最终顶点等长的 `float2` 数组，通道号限定 0–3 且不能重复；单通道每顶点 8 字节，最多四套。未提供的通道不占用 CPU 坐标数组，渲染时为零并由 UV mask 标记缺省。坐标可超出 0–1，必须有限；名称保留源通道信息。名称、sections 与 UV blobs 全部保存在同一 `.rasset`，不生成外部 UV 文件或纹理图集。

`materialSlot` 为局部零基槽索引，`defaultMaterialId` 才是引擎材质 GUID，0 表示继承 Actor 的兼容回退材质。`sourceKey` 是重导入匹配信息，不是材质资产 GUID；源身份发生变化不会保证原绑定自动延续，重名 FBX 的歧义处理见 [导入规范](Importing.md#重新导入已有资产)。Slot 名称不能为空；有效匹配键（非空 `sourceKey`，否则使用名称）必须唯一，同名但具有不同来源键的 Slot 仍合法。

section 以索引元素计数，必须按 `firstIndex` 排序、从 0 开始连续无重叠地覆盖完整索引缓冲，`indexCount` 非零且为 3 的倍数，槽索引有效。缺少 sections／slots 的旧网格隐式只有一个覆盖全部面的 Slot；多个显式槽必须提供 section。越界、空洞、不完整覆盖、UV 长度不匹配、重复通道、非有限值或无效类型均导致事务式加载失败，保留原内存网格。保存前执行同样的新属性校验。

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

加载 v3/v4/v5/v6/v7 时从 `Content/Materials`、`Content/Meshes` 解析引用；缺失时报 `Scene references material N that is not loaded from Content/Materials.`。v1（内嵌材质快照 + 内嵌网格描述）和 v2（引用材质）仍可读取，用于打开旧工程。

含灯光 Actor 时写出 **v4**，不含灯光时仍写 v3。v4 Actor 的 `mesh` 和 `light` 必须二选一；`light.type` 为 `Directional/Sky/Rect/Spot/Point`，其余字段为 `color/intensity/range/innerConeAngle/outerConeAngle/width/height/castShadows`。颜色和能量非负、范围和尺寸大于零、内外半角满足 `0 ≤ inner ≤ outer < 90°`，全部数值必须有限。灯光不向 materials/meshes 清单添加引用；资产删除依赖扫描识别 v4。非法类型、数值或混合组件导致加载／保存失败，保留原状态。字段含义见 [Lighting](Lighting.md)。

有 Actor 材质 Slot 覆盖时写出 **v5**，否则根据灯光组件仍写 v3 或 v4。`mesh.materialId` 保留为旧统一回退材质；`mesh.materialOverrides` 保存每槽的 GUID（0 为继承），`mesh.materialOverrideKeys` 保存并行的来源键／名称以便重载时按槽身份重映射。覆盖优先于网格 `materialSlots[].defaultMaterialId`，最后回退到 `materialId`。旧 v1–v4 保持兼容；格式错误、无效材质引用或无法合法解析的覆盖导致失败。v5 的灯光 Actor 仍不允许同时带 mesh 数据。

新保存的场景清单仅包含 Actor 实际引用的网格和材质，隐藏 Actor 也计入，重复实例共用一个引用。不删除 Content 中未引用的资产。`World::save` 只验证合法／重复资产 ID、Engine／Project 归属和 Actor 引用／变换，不验证或保存几何与材质载荷。编辑器保存场景前用不加载预览的文件索引确认依赖已经发布（遵循项目材质、网格及引擎挂载目录），缺失时提示先保存资产；这不是磁盘载荷完整性校验。旧 Sphere/Plane/Cube 的 1/2/3 准确身份可从代码重建，豁免文件检查。场景 JSON 使用临时文件替换，资产容器与场景之间尚无整体事务。

场景还可包含可选的 `camera` 与 `lighting` 配置；它们是场景数据，不会触发资产重写。`camera` 包含 `yaw/pitch/distance/target/exposure`，表示显式存储的初始视角。`lighting` 包含 `directionalIntensity/environmentIntensity` 与 `areaLight`（`enabled/center/halfAxisU/halfAxisV/radiance`）；半轴为非零正交世界空间向量，叉积指向单面发光侧。数值必须有限，能量非负，视距／俯仰／曝光遵循编辑器范围；解析或保存校验失败保留原场景／已发布文件。缺少字段的旧场景不启用场景照明覆盖，也不强制覆盖当前相机。矩形发光面的渲染范围见 [GI 文档](GlobalIllumination.md#单矩形发光面示例)。

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
3. **保存**：资产容器的普通写入直接覆盖目标文件（中途崩溃可能留下半写文件，属于待改进项）；场景 JSON 使用临时文件替换。新资源导入只写新文件，使用临时文件与排他发布，失败清理；见 [统一导入规范](Importing.md)。
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

### Camera 场景扩展（v6 / v7）

Camera Actor 使用独立 camera 对象保存 projection、verticalFovDegrees、orthographicHeight、nearClip、farClip、aspectRatio，并复用 Actor transform。camera/light/mesh 组件互斥；defaultCameraActor 引用同场景的 Camera Actor。旧场景的 camera 开场书签保持原语义，新书签可含 lens 和 pose。相机无网格/材质依赖，资源清单与删除检查据此处理。非默认景深配置使用v7，在Camera或开场书签lens中保存完整depthOfField对象；关闭效果保留调参，旧v1–v6默认无景深。格式选择、有效范围和兼容说明见 [Camera 系统](Camera.md)。
