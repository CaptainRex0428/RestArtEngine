# RestArtEngine 目录与基础架构

本工程面向技术美术的材质与渲染实验。当前编辑器通过 DX11 绘制 Actor 场景，使用 Dear ImGui 提供编辑界面。场景数据、资产数据、编辑器和渲染器分层组织，便于继续扩展与替换界面。

> 命名约定：产品与所有工程文件使用全称 **RestArtEngine**（`RestArtEngine.exe`、`RestArtEngine.sln`、`RestArtEngine.vcxproj`、`RestArtEngine.rc`、`RestArtEngineProject.json`、`include/RestArtEngine/`）。只有文件扩展名与格式标识保留 `RA` 缩写：`.raproject`、`.rascene`、`.ramat`、`.raeengine`、JSON `format`（`RAEngine.*`）、命名空间 `rae`、注册表目录 `%LOCALAPPDATA%\RAEngine`。

## 目录约定

资源回收入口见 [GarbageCollection.md](GarbageCollection.md)：Core 负责可配置调度和受管临时截图，Renderer／ImGui 各自回收闲置缓存；实际调用位于 Project Manager／编辑器的帧末。场景、CPU 资产、草稿和撤销历史继续按其原有生命周期管理。用户 Capture／F9 由应用层打开原生保存对话框，Renderer 只接收选定路径；取消不导出，用户图片不登记到 GC。批量／测试截图通过显式输出参数单独路由，不恢复默认项目截图目录。

| 目录 | 职责与放置规则 |
| --- | --- |
| `include/RestArtEngine/Core/`、`src/Core/` | 引擎生命周期等公共基础设施。 |
| `include/RestArtEngine/Project/`、`src/Project/` | `.raproject` 工程描述解析、引擎与工程资源路径解析验证。 |
| `include/RestArtEngine/Scene/`、`src/Scene/` | `World`、`Actor`、`Transform`、`StaticMeshComponent` 与场景序列化；不依赖 ImGui 或 DX11。 |
| `include/RestArtEngine/Assets/`、`src/Assets/` | `AssetLibrary`、CPU 网格、材质参数、OBJ 导入与资产文件读写。 |
| `include/RestArtEngine/Renderer/`、`src/Renderer/` | 渲染接口、GPU 资源、前向与延迟渲染、Shader 编译与 GPU 管线替换。 |
| `include/RestArtEngine/Editor/`、`src/Editor/` | 编辑器会话、选中对象、参数和源码草稿、Outliner / Scene / Details / Content 面板；`EditorDockManager` 管理面板身份、默认停靠布局和恢复入口；`ViewportInteraction` 负责视口拾取与变换手柄，通过公共接口修改场景和渲染器。 |
| `include/RestArtEngine/ImGui/`、`src/ImGui/` | ImGui 上下文和 Win32 / DX11 适配器；含 `EditorTheme` 双主题配色与本机偏好读写。 |
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

材质参数草稿通过 `SceneEditor::materialPreview()` 提供每帧只读覆盖，渲染器按资产 ID 同时用于前向／延迟绘制与光追材质。颜色、粗糙度、金属度及 Grid 变化可在拖动过程中预览，不修改 `AssetLibrary` 的已提交材质，也不重新编译 Shader。Apply 提交参数，Discard draft 恢复已提交值；场景保存仍只序列化已提交资产，Save All 则先提交草稿。渲染器只借用该覆盖到本次 `render()` 返回，不持有编辑器对象或草稿指针。

`MaterialAsset::twoSided` 是统一的逐材质表面选项，新材质默认单面。材质与场景快照共用序列化，旧文件缺字段时按双面读取，保持历史可见性。GPU 材质 `properties.w` 使用数值整数标记（bit 0 Grid、bit 1 Two-sided），进入 `EvaluateMaterial()` 前解码 Grid；常量缓冲和 `RayTriangle` 字节布局不变。该选项由渲染器统一处理，不要求工程手写材质 Shader 自行实现剔除。

视口拾取同时读取有效材质及预览覆盖，按镜像补偿后的三角形朝向过滤单面背面，避免不可见背面挡住后方 Actor 的选择；双面预览的背面可以正常拾取。

内置 Mesh 的三角形绕序与顶点法线统一朝外。DX11 按单／双面及镜像变换预建四种光栅状态，镜像时翻转正面定义；Forward 和 G-buffer 根据 `SV_IsFrontFace` 翻转双面背面的着色法线。全屏绘制恢复无剔除状态，防止继承上一对象状态。[D3D11 光栅状态定义](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_rasterizer_desc)提供了相应 API 语义。

解析及三角形光追使用相同的单／双面语义，双面命中法线朝向入射侧；镜像三角形修正其几何法线方向。阴影贴图从光源朝场景绘制，阴影可见性射线则从表面朝光源追踪，因此单面阴影采用相反的面筛选；Sphere／Cube 对这类射线选取朝光的远交点，避免封闭对象漏影。

当前每个 Actor 有一个 `StaticMeshComponent`，没有通用组件注册、反射或 ECS 调度系统。`Transform` 保存位置、以度为单位的 XYZ 欧拉旋转和缩放。场景采用 Y 轴向上；这与 UE 常见的 Z 轴向上约定不同，不能直接套用 UE 坐标。

`ViewportInteraction` 位于编辑器模块，负责将视口点击映射到网格选择，并通过 ImGuizmo 编辑选中 Actor 的 Transform。手柄提供移动、旋转、缩放与步进吸附；移动和旋转支持世界 / 局部坐标，缩放始终沿局部轴，避免产生当前 Transform 无法表示的剪切。变换同步到 Scene 面板，保存时沿用场景序列化。手柄是编辑器工具，不是新的运行时组件类型，Scene / Assets 无需依赖 ImGuizmo。

Actor 删除统一通过 World 的稳定 ID 完成。Outliner 按钮、右键菜单与场景上下文中的 Delete 快捷键都只移除 Actor 实例，保留共享资产。文本输入会屏蔽场景快捷键，避免编辑名称、材质或 Shader 时误操作。多选通过 Outliner 的 Ctrl/Shift 点击与视口 Ctrl/Shift 点击扩展，批量删除与批量变换各记一条撤销；仍无组件父子层级（见 [UndoTransactions.md](UndoTransactions.md)）。

内置 Mesh 包含 Sphere、Plane 和 Cube，OBJ 用于导入其他静态网格。当前顶点布局是 Position / Normal，OBJ 可以使用正索引或负索引、多边形会三角化，没有法线时生成法线。不包含 UV、切线、贴图、MTL 材质分组、骨骼或动画导入；复杂非凸多边形建议在 DCC 中预先三角化。

## 编辑器布局与视口合成

编辑器使用 Dear ImGui `v1.91.9b-docking`。`EditorDockManager` 集中维护六个面板的稳定身份和默认位置，创建工具栏下方的 DockSpace，并处理恢复默认布局。`SceneEditor` 负责面板内容及编辑器会话，业务面板不再计算屏幕边缘距离或手工吸附。实时分割树、标签分组、悬浮位置及其持久化交给 ImGui；内部 `DockBuilder` 调用限于默认布局创建／恢复及旧布局迁移。

DockSpace 根标识为 `RestArtEngine.DockSpace.v1`。旧版自由窗口配置没有此根节点，首次使用会建立默认停靠树；已有根节点时保留用户布局。新增 Scene 时，仅在缺少 Scene 设置的旧布局中把它加入原 Details 所在节点；旧 Details 为自由浮窗时按原位置尺寸创建双页签浮动组。迁移不重建其他节点，后续已有 Scene 配置即尊重用户安排。`Layout > Restore default layout` 显式重建默认树。布局使用现有 `RestArtEngine.imgui.ini`，没有第二套布局序列化器；它属于本机编辑器设置，不写入工程场景文件。六个工作区面板显式留在主 viewport，保持主窗口内 Docking；材质编辑器使用单独的系统 viewport。

默认左上为 Outliner，左下的 Scene / Details 共用同一个 dock 节点，底部 Content 的 Meshes 保留资源列表与导入／创建入口。Scene 编辑主选中 Actor 的属性；Details 以同一 Actor ID 实时解析实例身份、可见性、当前 Mesh 几何与来源，以及该实例唯一的 Material 引用，不从 Content 的 `selectedMesh_` 取目标，不遍历同 Mesh 实例汇总材质。多选明确主实例，无选中或已删除实例显示空状态，资源失效显示缺失提示；不新增 Mesh 材质绑定。场景／Outliner 选择事件请求显示 Details，Content 选择／导入只更新资源浏览状态；焦点请求按事件消费一次，手动切页后不逐帧抢回。两个实例查看页均归入 Actor 输入作用域，Scene / Details 保留既有窗口身份和原生 Docking 布局。

场景在编辑器模式下渲染到 DX11 纹理，通过 Scene Viewport 的 ImGui 图像绘制合成到窗口。`ViewportInteraction` 根据本帧实际图像内容区域设置 `SceneSettings` 的主客户区／纹理局部矩形；图像和 Gizmo 使用对应的桌面矩形，拾取鼠标减去主 viewport 原点。两者通过同帧原点转换，移动主窗口后仍使用相同图像区域和投影宽高比。渲染器提供纹理与尺寸接口，不依赖 Dock 节点或编辑器面板。无 UI 渲染路径继续输出到交换链。

材质参数和共享 Shader 从 Content 内部标签迁至按需打开的系统材质窗口，由 `SceneEditor::drawMaterialWindow` 绘制同一份编辑会话。Scene 的 Actor 材质入口和 Details 中主选中 Actor 实例的当前材质引用统一打开／聚焦该窗口。`ImGuiWindowClass` 使用 `NoAutoMerge`、无父 viewport 和禁止停靠，保持独立 HWND、系统标题栏与任务栏；窗口关闭保留会话草稿和预览，应用退出继续使用全局未保存确认。主窗口最小化或隐藏主 UI 时可单独提交材质界面，`EditorDockManager::keepAlive` 保留原 Dock 树。

`ImGuiLayer` 显式选择是否启用官方 Win32/DX11 多 viewport；Project Manager 和原图形／GC 自测保留单窗口模式。各系统窗口的 swapchain、RTV、缩放和呈现由后端管理，帧末先提交主 DrawData，再更新／呈现平台窗口；最小化主窗口时可跳过主 DrawData。适配层包装次窗口创建及原生过程，使跨帧消息使用正确 context，并把 WM_CLOSE 延迟为 ImGui 关闭请求。销毁平台窗口先于关闭后端和释放设备引用，不改动 vendor。主应用级快捷键限制在主 HWND 焦点，材质窗口的焦点、弹窗和文本编辑屏蔽场景命令。此实现不等于已接通其他平台／图形后端或逐显示器字体缩放。

GUI 职责、交互和验收约定见 [GuiDevelopment.md](GuiDevelopment.md)。停靠体系使用 ImGui，没有将 Godot 的 GUI 运行时引入引擎。

工具栏由 `SceneEditor` 绘制为两行：窗口带 `ImGuiWindowFlags_MenuBar`，第一行通过 `BeginMenuBar / EndMenuBar` 绘制 Layout / Edit / UI 和场景操作，第二行在正常内容区绘制渲染控件。高度为两倍 `GetFrameHeight()` 加上下窗口内边距与边框，并传入 `EditorDockManager::beginFrame`。不得用普通内容区的 `BeginMenu + SameLine` 模拟菜单栏：`SameLine` 不改变菜单的纵向布局模式，菜单项会占用剩余宽度，挤出后续控件；`EditorDockManager::drawLayoutMenu` 也必须在菜单栏内调用。

`WindowChrome` 位于 ImGui / Win32 适配层，由每个窗口的 `WindowState` 持有，`ImGuiLayer` 借用并在帧开始时绘制。它通过 `WM_NCCALCSIZE` 去掉系统标题栏绘制，保留 `WS_OVERLAPPEDWINDOW`；标题、按钮与分隔线使用 ImGui 主题，DWM 边框色同步当前主题。`WM_NCHITTEST` 区分拖动区、系统菜单、八向缩放和三个标题按钮，按钮发送原生系统命令，关闭沿用 `WM_CLOSE` → 编辑器未保存提示。主窗口样式、系统菜单和 `HTMAXBUTTON` 的保留遵循微软的[自定义窗口框架](https://learn.microsoft.com/en-us/windows/win32/dwm/customframe)与[吸附布局接入](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/ui/apply-snap-layout-menu)说明。

标题栏左上角复用 `assets/icons/RAEngine.png`：通过 `RestArtEngine.rc` 嵌入程序，由 `ImGuiLayer` 解码并持有 DX11 纹理，在关闭 UI 时释放，不依赖启动目录；图片区域保留原生系统菜单命中。

自绘窗口的 `WM_NCACTIVATE` 在非最小化时直接返回 `TRUE`，允许激活状态切换并由客户端继续负责标题绘制；最小化时保留默认处理，遵循微软的 [WM_NCACTIVATE 返回值与最小化约定](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-ncactivate)。调用 `DefWindowProc(..., -1)` 仍会在模态保存框打开时覆盖出经典浅色系统标题栏；仅拦截 `WM_NCPAINT` 不足以避免该路径。该处理位于共用 `WindowChrome`，保留 `WM_ACTIVATE`、系统模态 owner 禁用／恢复和原生保存框归属，不改窗口样式或对话框主题。

标题栏作为 viewport 顶部保留区域，`WindowChrome` 同帧更新 `WorkPos / WorkSize`，工具栏、DockSpace 和 Project Manager 从工作区开始布局；场景拾取与 Gizmo 继续使用图像本帧屏幕矩形。最小窗口为 480×360 逻辑像素以允许常见吸附分区，窄窗口中的面板内容仍可能需要滚动。当前应用仍使用系统 DPI 感知，标题按钮与边缘命中按窗口 DPI 计算；未将此功能宣称为全编辑器跨屏 DPI 缩放支持。

启动 `LoadingScreen::draw` 同样在 chrome 布局之后读取主 viewport 的 `WorkPos / WorkSize`，显式向该 viewport 的 background draw list 提交，并将背景、遮罩、文字、百分比和进度条统一偏移到工作区，再裁剪到工作区边界。调用者只传进度与文案，避免把 `IO.DisplaySize` 的本地尺寸与多 viewport 的桌面绘制坐标混用。图片继续使用完整 UV 拉伸铺满，图片缺失时的纯色背景采用同一布局；不覆盖标题栏，不引入裁切图片的填充策略。`LoadingScreenTests`（CMake `BUILD_TESTING=ON`）覆盖图片解码与无图回退、完整 UV、标题栏排除、三种尺寸及正负桌面坐标下的几何一致性；实际跨屏 DPI 不由这些坐标测试证明。

F11 全屏由 `MinimalApp` 管理 Win32 窗口样式、位置及 UI 显示状态，进入时隐藏标题栏和面板，退出时恢复；切换窗口样式之前同步 chrome 状态，保证 `WM_NCCALCSIZE` 和最大化工作区处理不会裁剪全屏尺寸。普通最大化适配显示器工作区，F11 覆盖整个显示器；两者均不启用多窗口渲染。

## 编辑命令、事务与主题

`EditorCommandRegistry` 位于 Editor 层，集中登记命令 ID、分组、文案、快捷键、回调与可用条件。已接入撤销 / 重做、Actor 新建 / 复制 / 删除、场景保存和布局恢复命令；菜单和相关按钮通过命令执行。渲染切换、F11 等应用级按键仍由 `MinimalApp` 处理，并非所有 UI 动作都已迁入注册表。`SceneEditor` 在文本输入或 Gizmo 使用期间跳过命令快捷键派发。

`UndoStack` 保存撤销 / 重做回调及历史游标，默认上限 64 步，新编辑会截断重做分支。回调捕获稳定 ID 和状态值拷贝，不持有 Actor 容器内指针。Gizmo 拖动按开始 / 结束聚合，Scene 面板变换按 Actor 与字段在 700ms 窗口内合并；多选删除和整组变换各记一条。Ctrl+D 多选复制仍逐个记录；Alt＋拖动由 `ViewportInteraction` 判定手势并回调 `SceneEditor` 复制选中集合，结束时将创建副本与最终变换记成一条事务，同时保存原选择和副本选择。材质 / Shader 草稿及其他未接入的属性编辑不自动获得撤销能力。详细边界见 [UndoTransactions.md](UndoTransactions.md)。

`EditorTheme` 集中 Black / Dark Blue 配色及共用几何样式，通过 `UI > Theme` 切换；`MinimalApp` 在创建 ImGui 上下文前读取偏好，`ImGuiLayer` 初始化时应用，字体仍由后者加载。`RestArtEngine.theme.ini` 与交互式布局 ini 都相对进程工作目录解析，主题文件不存 Dock 树、不写入工程资产。

## 文件与持久化

场景文件 `.rascene` 使用可读 JSON，文件头 `format` 为 `RAEngine.Scene`，`version` 为 `1`。它保存 Actor、变换、网格 / 材质引用、材质参数快照及网格元数据。可选字段 `background`（`"sky"` 默认 / `"black"`）选择程序化天空或纯黑背景；纯黑时渲染器把标志写入 `SceneConstants.cameraPosition.w`（该分量在着色器中原本未使用），由 `Background()` 直接返回黑色，常量缓冲布局不变。导入网格的 `sourcePath` 相对场景文件记录，加载时重建 CPU 网格；因此场景文件不是把所有 OBJ 数据打包进去的二进制包。移动项目时应同时保留工程描述、工程 `Content/` 与 `Shaders/` 的相对结构。

独立材质存储为工程 `Content/Materials/{id}.ramat`，同样为 JSON，`format` 为 `RAEngine.Material`，`version` 为 `1`。材质文件包含 ID、名称、Base Color、Roughness、Metallic、Grid 与 Two-sided。新建材质默认单面；旧文件缺少 `twoSided` 时按双面读取，存在该字段但不是布尔值时拒绝加载。加载场景使用场景内的材质快照；使用 `Reload material assets` 可显式读入工程独立材质文件。`Save material assets` 同时保存当前场景快照，使下次启动恢复保存的外观。

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

### 编辑器选中描边

选中描边是**编辑器可视化**，不属于运行时场景渲染。它由一个独立通道 `SelectionOutlinePass`（`include/RestArtEngine/Renderer/SelectionOutline.h`、`src/Renderer/SelectionOutline.cpp`）承担，位置在场景着色全部完成之后、编辑器合成场景纹理之前：

1. 场景几何照常写入颜色与深度（前向或延迟路径）。
2. 遮罩通道只重绘选中 Actor 的轮廓到一张场景尺寸的遮罩纹理；深度只读比较（`LESS_EQUAL`、不写深度），因此被其他物体遮挡的部分不会外溢描边。
3. 合成通道用屏幕空间膨胀把遮罩扩成描边，用 `SV_Position` 换算的纹理坐标采样并与场景颜色 alpha 混合；不按视口局部 UV 采样，保证停靠面板任意尺寸时描边仍贴合轮廓。

开关与颜色来自 `SceneSettings` 的编辑器字段：`outlineActors` / `outlineActorCount`（借用编辑器当前选中列表，只对本次调用有效）、`outlineColor`（默认取主题强调色）、`outlineThickness`（像素半径）。运行时 `render(settings, error)` 不填这些字段，遮罩纹理也只在编辑器第一次有选中时创建，因此独立运行不承担该通道的开销。描边颜色不写入场景资产，也不参与撤销或保存。

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
| 编辑器配色与主题偏好 | `src/ImGui/EditorTheme.cpp`、`src/ImGui/ImGuiLayer.cpp` |
| 自定义标题栏、窗口按钮、原生命中测试 | `src/ImGui/WindowChrome.cpp`；窗口状态与 F11 集成在 `app/MinimalApp.cpp` |
| 面板身份、默认 Dock 树、布局恢复 | `src/Editor/EditorDockManager.cpp` |
| 视口拾取、变换手柄、相机 | `src/Editor/ViewportInteraction.cpp` |
| 启动加载界面与背景图 | `src/Editor/LoadingScreen.cpp`、`assets/load/load.png` |
| 新的公开接口 | `include/RestArtEngine/<模块>/`，实现放 `src/<模块>/` |

`app/` 是引擎的可执行入口（不是示例工程），新增可复用功能应放进对应模块而不是堆进入口文件。

### 构建依赖与警告

构建配置修改应落到 `RestArtEngine.lua`、`vendor/imgui.lua` 或 `CMakeLists.txt`，再重新生成工程；`settings/*.vcxproj` 与 solution 属于生成物，不手工修补。

静态库只归档自身对象文件；系统 DLL 导入库在最终可执行文件／动态库链接阶段解析。Premake 的 `StaticLib` 不设置这些系统 `links`，否则 MSVC librarian 会把导入库合并进归档；引擎的系统依赖在 `RestArtEngine.lua` 声明。CMake 为导出的 DearImGui 目标保留 `INTERFACE` 链接需求，供消费者使用；引擎自身直接使用的系统依赖仍显式声明。

新增警告先查原因与影响，不以全局降级警告等级、`/IGNORE:4006` 或 `/FORCE:MULTIPLE` 代替修复。第三方代码确需局部豁免时限定到具体文件／告警编号并解释原因。微软的 [LNK4006 文档](https://learn.microsoft.com/en-us/cpp/error-messages/tool-errors/linker-tools-warning-lnk4006?view=msvc-170) 明确指出合并导入库可能导致重复符号；CMake 链接接口语义见 [target_link_libraries](https://cmake.org/cmake/help/latest/command/target_link_libraries.html)。

涉及归档／链接依赖的改动，应重新生成并执行实际 Rebuild，检查最终程序也能完成链接，不能仅凭增量构建“无任务”判断警告已修复。验证时注明构建配置与覆盖范围。

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

窗口外框回归：`powershell -ExecutionPolicy Bypass -File scripts/Test-WindowChrome.ps1`（可通过 `-Executable` 指定构建）。脚本复制 Sandbox 到独立输出目录，隔离布局、主题与场景修改，仅控制自己启动的测试进程；验证场景编辑器与 Project Manager 的标题 / 边角 / 系统菜单命中、最小化、最大化工作区、还原、双击标题以及干净关闭的明确零退出码。场景编辑器另测 F11 / Esc、缩放、实际 F9 截图，以及脏场景点击关闭后保留进程和场景文件。脏关闭用例最后只终止测试进程，不保存测试编辑。


Capture 外框回归使用 `scripts/Test-Capture.ps1`：从桌面合成画面读取测试窗口，保存弹框前／弹框中／取消后的 PNG 并检查标题背景像素；仅使用 DX11 截图无法检测系统覆绘。覆盖 Black / Dark Blue、F9、点击 Capture、嵌套覆盖确认、owner 禁用与取消恢复，以及 Unicode 保存／覆盖语义。

编辑器自测检查 Layout / Edit / UI 菜单可达、弹出位置、标题栏／工具栏／DockSpace 边界、主题和字体／样式缩放。系统吸附弹窗、跨显示器拖动与 DPI 变化需另做实际交互验收，不能由命中值或字体缩放测试推断通过。
