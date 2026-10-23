# RestArtEngine 目录与基础架构

本工程面向技术美术的材质与渲染实验。当前编辑器通过 DX11 绘制 Actor 场景，使用 Dear ImGui 提供编辑界面。场景数据、资产数据、编辑器和渲染器分层组织，便于继续扩展与替换界面。

> 命名约定：产品与所有工程文件使用全称 **RestArtEngine**（`RestArtEngine.exe`、`RestArtEngine.sln`、`RestArtEngine.vcxproj`、`RestArtEngine.rc`、`RestArtEngineProject.json`、`include/RestArtEngine/`）。只有文件扩展名与格式标识保留 `RA` 缩写：`.raproject`、`.rascene`、`.ramat`、`.raeengine`、JSON `format`（`RAEngine.*`）、命名空间 `rae`、注册表目录 `%LOCALAPPDATA%\RAEngine`。

## 目录约定

| 目录 | 职责与放置规则 |
| --- | --- |
| `include/RestArtEngine/Core/`、`src/Core/` | 引擎生命周期等公共基础设施。 |
| `include/RestArtEngine/Project/`、`src/Project/` | `.raproject` 工程描述解析、引擎与工程资源路径解析验证。 |
| `include/RestArtEngine/Scene/`、`src/Scene/` | `World`、`Actor`、`Transform`、`StaticMeshComponent` 与场景序列化；不依赖 ImGui 或 DX11。 |
| `include/RestArtEngine/Assets/`、`src/Assets/` | `AssetLibrary`、CPU 网格、材质参数、OBJ 导入与资产文件读写。 |
| `include/RestArtEngine/Renderer/`、`src/Renderer/` | 渲染接口、GPU 资源、前向与延迟渲染、Shader 编译与 GPU 管线替换。 |
| `include/RestArtEngine/Editor/`、`src/Editor/` | 编辑器会话、选中对象、参数和源码草稿、Outliner / Details / Content 面板；`EditorDockManager` 管理面板身份、默认停靠布局和恢复入口；`ViewportInteraction` 负责视口拾取与变换手柄，通过公共接口修改场景和渲染器。 |
| `include/RestArtEngine/ImGui/`、`src/ImGui/` | ImGui 上下文和 Win32 / DX11 适配器；当前工作区新增 `EditorTheme` 配色与本机偏好读写，尚未提交验证。 |
| `Shaders/Private/Scene.hlsl` | 渲染器维护的几何、前向、G-buffer、延迟光照与背景着色代码。修改时必须保持 CPU / HLSL 数据布局一致。 |
| `Shaders/Materials/Standard.hlsli` | 引擎提供的默认材质源码模板，编辑器不会把工程修改写入此模板。 |
| `Projects/Sandbox/Sandbox.raproject` | 默认 Sandbox 工程描述，声明引擎、工程资产、工程 Shader 与默认场景的位置。 |
| `RestArtEngine.exe` | 独立工程管理器和编辑器程序；无 `--project` 时进入 Project Manager。CMake 目标名为 `RestArtEngine`。 |
| `Engine.raeengine` | 引擎安装身份（版本和发布 / 源码 hash），用于工程绑定和工程管理器兼容检查。 |
| `Projects/Sandbox/Sandbox.raproject`、`Content/`、`Shaders/` | 默认示例工程（纯数据）：`.raproject` 声明引擎链接与内容路径，通过 `RestArtEngine.exe` 打开；不包含也不生成 `.sln`。 |
| `Projects/Sandbox/Content/Scenes/` | Sandbox 工程的场景资产；默认场景为 `Default.rascene`。 |
| `Projects/Sandbox/Content/Materials/` | Sandbox 工程的材质参数资产。参数资产与 HLSL 源码分开管理。 |
| `Projects/Sandbox/Content/Meshes/` | Sandbox 工程的 OBJ 源文件；编辑器导入时复制至当前工程的 Meshes 目录。 |
| `Projects/Sandbox/Shaders/Materials/Standard.hlsli` | Sandbox 工程可编辑的共享材质 Shader，编辑器 Apply / Save 的源码入口。 |
| `Projects/RayTracing_Simple/` | 光追测试示例（纯数据）：康奈尔盒 + 两个镜面球，用于观察 `--rt` 解析光追的反射与阴影差异；与 Sandbox 一样通过 `RestArtEngine.exe` 打开，不生成自己的 `.sln`。 |
| `app/MinimalApp.cpp` | 引擎可执行入口：Win32 窗口、消息循环、Project Manager、编辑器启动、启动加载界面、命令行解析。 |
| `app/EditorSmoke.cpp` | 引擎 GPU 自测：通过 ImGui 上下文钩子注入真实鼠标/键盘，验证拾取、gizmo、材质与 Shader 流程（`--editor-self-test`）。 |
| `scripts/` | 引擎验证脚本：`Validate-Render.ps1` 跑渲染自测并比对截图，`Test-Window.ps1` 验证窗口生命周期。 |
| `vendor/` | 随仓库提供的第三方代码和许可证，如 Dear ImGui、ImGuizmo、nlohmann/json。Dear ImGui Docking 版本与来源记录在 `docs/vendor/ImGui.md`；ImGuizmo 用于编辑器变换手柄，记录在 `docs/vendor/ImGuizmo.md`。 |
| `docs/` | 架构、文件格式与编辑工作流文档。 |
| `artifacts/`、`screenshots/` | 验证日志与截图，属于输出，不应作为资产源目录。 |
| `build*/` | 编译生成物，不在这些目录维护源码、材质或 Shader。 |

对外接口放在引擎根目录的 `include/RestArtEngine/<模块>/`，实现放在对应的 `src/<模块>/`。新增可复用引擎功能不继续堆入样例入口。引擎根目录的 `Shaders/Private` 保存公共渲染管线，`Shaders/Materials` 提供默认模板；用户内容属于具体工程，各工程拥有自己的 `Content/` 和 `Shaders/`。编辑器导入、保存与重新加载都解析到当前工程，不把用户资产写入引擎根目录。

## 工程描述与路径

示例工程位于 `Projects/`（`Sandbox` 通用演示、`RayTracing_Simple` 光追测试）。`.raproject` 中的路径相对描述文件所在目录解析：

```text
RestArtEngine/                          引擎仓库根目录
├─ include/RestArtEngine/、src/         引擎与编辑器源码
├─ Shaders/                        引擎公共管线与默认模板
└─ Projects/
   ├─ Sandbox/                     通用演示工程
   │  ├─ Sandbox.raproject
   │  ├─ Content/                  该工程的资产根目录
   │  │  ├─ Scenes/Default.rascene
   │  │  ├─ Materials/*.ramat
   │  │  └─ Meshes/*.obj
   │  └─ Shaders/Materials/Standard.hlsli
   └─ RayTracing_Simple/           光追测试工程（康奈尔盒 + 镜面球）
      ├─ RayTracing_Simple.raproject
      ├─ Content/
      │  ├─ Scenes/Default.rascene
      │  └─ Materials/*.ramat
      └─ Shaders/Materials/Standard.hlsli
```

`Projects/` 是示例工程的存放约定，并非强制位置。独立工程也可以放在仓库外，用 `.raproject` 的 `engineRoot` 引用这个引擎；工程 Content 不归引擎管理，也不要求根目录存在 `Content/`。

| 字段 | Sandbox 的值 | 含义 |
| --- | --- | --- |
| `engineRoot` | `../..` | 指向引擎根目录的缓存 / 回退路径，提供公共渲染管线源码；直接 `--project` 打开时以此定位引擎。 |
| `engineAssociation` | `0.2.0` | 命名的引擎版本关联（UE 式），由启动器通过引擎注册表解析到安装路径；切版本即改此关联。 |
| `engineVersion` | `0.2.0` | 工程绑定引擎的身份版本，与 `engineHash` 一起用于兼容校验。 |
| `engineHash` | `dev-0.2.0` | 工程绑定引擎的稳定构建标识。 |
| `contentRoot` | `Content` | 当前工程的场景、材质、网格资产目录。 |
| `shaderRoot` | `Shaders` | 当前工程的可编辑材质 Shader 目录。 |
| `defaultScene` | `Content/Scenes/Default.rascene` | 当前工程默认加载和保存的场景。 |

工程描述使用 JSON，`format` 为 `RAEngine.Project`、`version` 为 `3`，并包含非空 `name`。`--project` 接受工程目录或 `.raproject` 文件；传入目录时其中必须恰好存在一个工程描述文件。Content、Shader 和默认场景路径必须位于当前工程内，解析时检查相对路径与符号链接，拒绝越出工程目录的配置。解析失败保留先前的工程路径数据。版本 `1` / `2` 仍可读取：`1` 视为没有引擎绑定的旧工程，`2` 为无 `engineAssociation` 的绑定工程。

引擎根目录包含 `Engine.raeengine`：

```json
{
  "format": "RAEngine.Engine",
  "version": 1,
  "name": "RestArtEngine",
  "engineVersion": "0.2.0",
  "engineHash": "dev-0.2.0"
}
```

工程的 `engineVersion` 和 `engineHash` 必须同时存在，并与所选引擎身份完全匹配。工程管理器可以用 `checkEngineCompatibility` 判断匹配、版本不一致、hash 不一致或引擎身份文件缺失；不匹配时可选择「使用引用的引擎」「更新到当前引擎」或「选择引擎…」（浏览目录或从已发现引擎列表选择）。`updateProjectEngineBinding` 会原子更新 `engineAssociation`、`engineRoot`、版本和 hash，其中 `engineAssociation` 默认取引擎的版本号。没有绑定的 v1 工程报告为 `LegacyProject`，便于首次打开时升级而不破坏旧 Sandbox 或外部工程。

引擎版本通过**引擎注册表**解析：注册表是每用户一份的 `engines.json`（Windows 位于 `%LOCALAPPDATA%\RAEngine\engines.json`），记录 `association → 引擎目录` 的映射。工程管理器启动时会把当前运行的引擎登记进去；`.raproject` 的 `engineAssociation` 通过它解析到安装路径，`engineRoot` 则作为缓存 / 回退，保证 `--project` 直接打开时无需注册表也能定位引擎。打开工程时若 `engineRoot` 失效，工程管理器会按 `engineAssociation` 自动恢复并重写 `engineRoot`；恢复失败（无匹配引擎、或工程 manifest 不可写）时提示无法打开。

新工程可以复用同一个引擎目录，拥有独立的 Content 和材质 Shader；引擎模板与工程源码是两个文件，保存一个工程的 Shader 不会修改其他工程或默认模板。移动工程后，应检查 `engineRoot` 是否仍能指向引擎。

引擎可独立启动。没有 `--project` 时，`RestArtEngine.exe` 先显示 Project Manager：可以打开已有 `.raproject`、从 `Projects/` 中选择案例，或把 `Projects/Sandbox/` 复制到指定父目录创建新工程。新工程拥有自己的 `Content/`、材质 Shader 和场景文件，manifest 会写入当前引擎的 `engineAssociation`、`engineVersion`、`engineHash` 与相对 `engineRoot` 链接。工程是纯数据，不生成自己的 `.sln`；编译引擎只打开仓库根目录的 `RestArtEngine.sln`。工程管理器在进入编辑器前检查身份；版本或 hash 不一致时，可以更新绑定到当前引擎、使用 manifest 引用的引擎、浏览选择其它引擎版本，或关闭。`contentRoot`、`shaderRoot` 目录和 `defaultScene` 文件必须已经存在；默认材质源码位于 `shaderRoot/Materials/Standard.hlsli`。复制时保留 Content 与 Shaders 的完整内容，不只复制描述文件。

`Engine.raeengine` 是引擎安装的发布身份文件。`engineHash` 由发行或构建流程写入，必须是稳定的构建标识；它不依赖进程随机数，也不把工程资产复制进引擎。这样可以把引擎目录独立发布，工程只保存链接和身份信息，未来可单独开源或安装多份引擎并在工程管理器中切换。

## 数据关系

```text
SceneEditor
  ├─ World
  │   └─ Actor：ID、名称、可见性、Transform
  │       └─ StaticMeshComponent：meshId、materialId
  ├─ AssetLibrary
  │   ├─ MeshAsset：顶点、法线、索引、来源
  │   └─ MaterialAsset：Base Color、Roughness、Metallic
  └─ SceneRenderer
      ├─ 按 Actor 变换与资产引用绘制网格
      └─ 编译 HLSL，成功后替换活动 GPU Shader
```

`World` 用稳定 ID 标识 Actor；复制 Actor 分配新 ID，删除其他 Actor 不改变现有 ID。编辑器应保存 ID，在使用时重新查找对象，避免持有可能因 vector 扩容失效的指针。Actor 通过资产 ID 引用 Mesh 和 Material，同一资产可以由多个 Actor 共享。修改共享 Material 会影响所有引用它的 Actor；需要独立参数时创建另一个 Material 并重新分配。

当前每个 Actor 有一个 `StaticMeshComponent`，没有通用组件注册、反射或 ECS 调度系统。`Transform` 保存位置、以度为单位的 XYZ 欧拉旋转和缩放。场景采用 Y 轴向上；这与 UE 常见的 Z 轴向上约定不同，不能直接套用 UE 坐标。

`ViewportInteraction` 位于编辑器模块，负责将视口点击映射到网格选择，并通过 ImGuizmo 编辑选中 Actor 的 Transform。手柄提供移动、旋转、缩放与步进吸附；移动和旋转支持世界 / 局部坐标，缩放始终沿局部轴，避免产生当前 Transform 无法表示的剪切。变换同步到 Details，保存时沿用场景序列化。手柄是编辑器工具，不是新的运行时组件类型，Scene / Assets 无需依赖 ImGuizmo。

Actor 删除统一通过 World 的稳定 ID 完成。Outliner 按钮、右键菜单与场景上下文中的 Delete 快捷键都只移除 Actor 实例，保留共享资产。文本输入会屏蔽场景快捷键，避免编辑名称、材质或 Shader 时误操作。多选通过 Outliner 的 Ctrl/Shift 点击与视口 Ctrl/Shift 点击扩展，批量删除与批量变换各记一条撤销；仍无组件父子层级（见 [UndoTransactions.md](UndoTransactions.md)）。

内置 Mesh 包含 Sphere、Plane 和 Cube，OBJ 用于导入其他静态网格。当前顶点布局是 Position / Normal，OBJ 可以使用正索引或负索引、多边形会三角化，没有法线时生成法线。不包含 UV、切线、贴图、MTL 材质分组、骨骼或动画导入；复杂非凸多边形建议在 DCC 中预先三角化。

## 编辑器布局与视口合成

编辑器使用 Dear ImGui `v1.91.9b-docking`。`EditorDockManager` 集中维护五个面板的稳定身份和默认位置，创建工具栏下方的 DockSpace，并处理恢复默认布局。`SceneEditor` 负责面板内容及编辑器会话，业务面板不再计算屏幕边缘距离或手工吸附。实时分割树、标签分组、悬浮位置及其持久化交给 ImGui；内部 `DockBuilder` 调用仅用于创建和恢复默认布局。

DockSpace 根标识为 `RestArtEngine.DockSpace.v1`。旧版自由窗口配置没有此根节点，首次使用会建立默认停靠树；已有根节点时保留用户布局。`Layout > Restore default layout` 显式重建默认树。布局使用现有 `RestArtEngine.imgui.ini`，没有第二套布局序列化器；它属于本机编辑器设置，不写入工程场景文件。当前只启用主窗口内 Docking，没有启用 ImGui 的操作系统多视口窗口功能。

场景在编辑器模式下渲染到 DX11 纹理，通过 Scene Viewport 的 ImGui 图像绘制合成到窗口。`ViewportInteraction` 根据本帧实际图像内容区域设置 `SceneSettings` 的屏幕矩形；投影宽高比、鼠标拾取及 Gizmo 使用同一矩形。这样停靠、分割和浮动无需保留旧位置的渲染区域，图像也按窗口的层级参与遮挡。渲染器提供纹理与尺寸接口，不依赖 Dock 节点或编辑器面板。无 UI 渲染路径继续输出到交换链。

Godot 编辑器的职责划分和后续扩展方向见 [GodotGuiStudy.md](GodotGuiStudy.md)。本轮使用 ImGui 实现停靠体系，没有将 Godot 的 GUI 运行时引入引擎。

工具栏由 `SceneEditor` 绘制为两行，高度按 `GetFrameHeight()`、窗口内边距与行距计算，并传入 `EditorDockManager::beginFrame`，不再使用固定 90px 高度。F11 全屏由 `MinimalApp` 管理 Win32 窗口样式、位置及 UI 显示状态，进入时隐藏面板，退出时恢复；它不改变 Docking 的主窗口边界，也不启用多窗口渲染。

## 编辑命令、事务与主题

`EditorCommandRegistry` 位于 Editor 层，集中登记命令 ID、分组、文案、快捷键、回调与可用条件。已接入撤销 / 重做、Actor 新建 / 复制 / 删除、场景保存和布局恢复命令；菜单和相关按钮通过命令执行。渲染切换、F11 等应用级按键仍由 `MinimalApp` 处理，并非所有 UI 动作都已迁入注册表。`SceneEditor` 在文本输入或 Gizmo 使用期间跳过命令快捷键派发。

`UndoStack` 保存撤销 / 重做回调及历史游标，默认上限 64 步，新编辑会截断重做分支。回调捕获稳定 ID 和状态值拷贝，不持有 Actor 容器内指针。Gizmo 拖动按开始 / 结束聚合，Details 变换按 Actor 与字段在 700ms 窗口内合并；多选删除和整组变换各记一条，多选复制仍逐个记录。材质 / Shader 草稿及其他未接入的属性编辑不自动获得撤销能力。详细边界见 [UndoTransactions.md](UndoTransactions.md)。

截至 2026-10-23，工作区的 `EditorTheme` 已集中 Black / Dark Blue 配色及共用几何样式，通过 `UI > Theme` 切换；`MinimalApp` 在创建 ImGui 上下文前读取偏好，`ImGuiLayer` 初始化时应用，字体仍由后者加载。`RestArtEngine.theme.ini` 与交互式布局 ini 都相对进程工作目录解析，主题文件不存 Dock 树、不写入工程资产。主题代码尚未提交，本轮仅核对源码，未验证其构建与运行效果。

## 文件与持久化

场景文件 `.rascene` 使用可读 JSON，文件头 `format` 为 `RAEngine.Scene`，`version` 为 `1`。它保存 Actor、变换、网格 / 材质引用、材质参数快照及网格元数据。可选字段 `background`（`"sky"` 默认 / `"black"`）选择程序化天空或纯黑背景；纯黑时渲染器把标志写入 `SceneConstants.cameraPosition.w`（该分量在着色器中原本未使用），由 `Background()` 直接返回黑色，常量缓冲布局不变。导入网格的 `sourcePath` 相对场景文件记录，加载时重建 CPU 网格；因此场景文件不是把所有 OBJ 数据打包进去的二进制包。移动项目时应同时保留工程描述、工程 `Content/` 与 `Shaders/` 的相对结构。

独立材质存储为工程 `Content/Materials/{id}.ramat`，同样为 JSON，`format` 为 `RAEngine.Material`，`version` 为 `1`。材质文件包含 ID、名称、Base Color、Roughness 与 Metallic。加载场景使用场景内的材质快照；使用 `Reload material assets` 可显式读入工程独立材质文件。`Save material assets` 同时保存当前场景快照，使下次启动恢复保存的外观。

加载先构造并验证候选 World 和 AssetLibrary，全部成功后再替换当前场景。JSON 损坏或引用的 OBJ 无法加载时，返回诊断并保留当前 World 和 AssetLibrary。场景文件与独立材质文件各自持久化；需要版本管理的成果包括工程描述、场景、材质参数、OBJ 与工程 Shader 源文件。

## 与 UE 概念的对应

| 本工程 | UE 中接近的使用概念 | 当前边界 |
| --- | --- | --- |
| `World` / Actor 列表 | World / Level、World Outliner | 单场景，无 Level Streaming、父子层级或蓝图。 |
| Actor 的 Transform 与 `StaticMeshComponent` | Actor / Static Mesh Component | 一个静态网格组件，无任意组件栈。 |
| 视口变换手柄与网格拾取 | 视口选择及移动 / 旋转 / 缩放工具 | 支持多选，Gizmo 以主 Actor 为枢轴对整组应用同一世界增量；编辑事务覆盖批量增删与变换；使用 Y 轴向上的引擎坐标。 |
| `MeshAsset` | Static Mesh 资产 | 内置几何与 OBJ，无 FBX / Nanite / LOD 工具链。 |
| `MaterialAsset` 参数编辑 | Material Instance 的参数调节 | 共享标准 Shader，尚无节点图、材质继承或纹理采样参数。 |
| Shader 编辑器 Apply | 修改后编译并更新预览 | 编译共享 HLSL，当前为 DX11 Shader Model 5。 |
| Save | 将编辑成果保存到磁盘 | Apply 与 Save 分离，不存在自动 Cook 或派生数据缓存。 |

这里借鉴 UE 的对象与编辑流程，文件格式和代码实现均为 RestArtEngine 自己的接口。

## 渲染边界与扩展位置

前向和延迟两条路径共享材质表达。延迟路径写入三个 G-buffer：Albedo / Roughness、World Normal / Metallic、World Position / Coverage，再进行光照。Lit、Unlit 和调试视图均从该基础上工作。

光照深度图支持 2x2 PCF、单点硬比较、3x3 PCF、PCSS 软阴影和两级 CSM。PCSS 在现有 D3D11 深度图上先搜索 blocker，再按估计半影半径执行比较采样；CSM 将两个相机距离切片渲染到 Texture2DArray，并在延迟光照中按距离选择切片。GTAO 使用 Deferred G-buffer 的世界位置/法线做屏幕空间环境遮蔽，Deferred Contact Shadow 沿光线方向在位置缓冲中做短距离步进；前向路径对这两种模式使用普通深度阴影回退。这些模式共享同一套资源管理和切换入口。

当前真实场景渲染只接入 DX11，OpenGL、DX12、Vulkan、Null 的 RHI 仍为骨架。后续支持其他 API 时，在 `Renderer` 中落地资源和命令接口，并保持 Scene / Assets 不依赖具体图形 API。

光追开关当前使用 GPU Shader 中的解析求交：内置 Sphere / Plane / Cube 使用精确解析路径，OBJ、未来 MeshAsset 以及超过 32 个内置对象的部分使用 `StructuredBuffer<RayTriangle>` 三角形路径。每帧按 Actor 变换上传全部可见网格三角形，不设置引擎级三角形预算；仅在数量无法用 D3D11 结构化缓冲区的字节数或元素数表示时拒绝分配，避免静默丢弃场景几何。没有 DXR、硬件 RT 管线、通用网格 BVH 或路径追踪。节点材质编辑、纹理资产、加速结构和其他后端是后续扩展。

## 开发指引

### 目录归属

| 归属 | 目录 / 文件 | 规则 |
| --- | --- | --- |
| 引擎 | `include/`、`src/`、`app/`、`Shaders/`、`vendor/`、`scripts/`、`docs/` | 提交到引擎仓库；引擎功能不依赖任何具体工程。 |
| 引擎资源 | `assets/`（logo、启动加载背景） | 随引擎发布；运行时路径以引擎根目录为准。 |
| 工程 | `Projects/<name>/`（`.raproject` + `Content/` + `Shaders/`） | 纯数据，可独立提交 / 分发；编辑器不会写回引擎目录。 |
| 输出 | `build*/`、`artifacts/`、`screenshots/` | 不提交，已由 `.gitignore` 排除。 |

### 常见改动落点

| 想改什么 | 先看这里 |
| --- | --- |
| 窗口、消息循环、命令行、启动与加载流程、Project Manager | `app/MinimalApp.cpp` |
| 引擎 GPU 自测 | `app/EditorSmoke.cpp` |
| 场景 / Actor / 场景序列化 | `include/RestArtEngine/Scene/World.h`、`src/Scene/World.cpp` |
| 网格导入、材质资产与资产文件格式 | `src/Assets/AssetLibrary.cpp`、`src/Assets/AssetSerialization.h` |
| 工程格式、引擎身份、引擎注册表 | `src/Project/Project.cpp` |
| DX11 资源、G-buffer、阴影、光照 | `src/Renderer/SceneRenderer.cpp` + `Shaders/Private/Scene.hlsl` |
| 材质开放函数 | `Shaders/Materials/Standard.hlsli` 的 `EvaluateMaterial()` |
| 编辑器面板、草稿 / Apply / Save | `src/Editor/SceneEditor.cpp` |
| 编辑命令、快捷键与撤销历史 | `src/Editor/EditorCommands.cpp`、`src/Editor/UndoStack.cpp`；动作接入在 `SceneEditor.cpp` |
| 编辑器配色与主题偏好（当前工作区） | `src/ImGui/EditorTheme.cpp`、`src/ImGui/ImGuiLayer.cpp` |
| 面板身份、默认 Dock 树、布局恢复 | `src/Editor/EditorDockManager.cpp` |
| 视口拾取、变换手柄、相机 | `src/Editor/ViewportInteraction.cpp` |
| 启动加载界面与背景图 | `src/Editor/LoadingScreen.cpp`、`assets/load/load.png` |
| 新的公开接口 | `include/RestArtEngine/<模块>/`，实现放 `src/<模块>/` |

`app/` 是引擎的可执行入口（不是示例工程），新增可复用功能应放进对应模块而不是堆进入口文件。

### 必须守住的约定

1. **常量缓冲布局**：`SceneConstants`(144B) / `ObjectConstants`(160B) / `RayConstants`(5136B) / `ShadowConstants`(160B) 由 `static_assert` 与运行时 `D3DReflect` 双向锁定，改字段必须同时改 C++ 与 HLSL。
2. **资源绑定**：`t0..t4`、`s0/s1` 的语义固定，`validateShader()` 会拒绝不兼容的工程 Shader。
3. **前向 / 延迟一致性**：两条路径共享 `ShadeSurface()`，`--editor-self-test` 会比对两者图像差异。
4. **场景加载事务性**：先在临时 `World` + `AssetLibrary` 中完成解析与引用校验，成功后才提交；失败保留当前数据。
5. **工程资产不写回引擎**：编辑器所有保存路径都解析到当前工程的 `contentRoot` / `shaderRoot`。

### 提交前验证

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-debug
.\build\cmake-vs2022\Debug\RestArtEngine.exe --editor-self-test --capture-dir artifacts\editor-validation
.\scripts\Validate-Render.ps1
```

`--editor-self-test` 与 `Validate-Render.ps1` 需要 D3D11 设备（硬件设备不可用时回退 WARP，速度较慢）。

