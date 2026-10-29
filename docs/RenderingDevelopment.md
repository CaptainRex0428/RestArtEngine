# 渲染开发要求与架构设计

本文件记录核心渲染开发的已确认要求、源码现状及待讨论方案。现有工程架构仍以 [Architecture.md](Architecture.md) 和 [README.md](../README.md) 为准；这里的拟议目录和接口不会自动成为已实现能力。

## 已确认范围

- 标准且职责明确的工程结构。
- Shader 热编译正常，成功更新画面、失败保留可用版本。
- 可调参数变化可实时观察，不以保存或重启作为普通调参的前提。
- 便于手写 Shader、管线、渲染特性，并支持后续平台与后端移植。
- Windows、Linux、macOS 均纳入首轮架构设计。
- 图形后端包括 DX11、DX12、Vulkan、OpenGL，支持在可用后端之间切换。
- 特性可以按工程需求保留、禁用或采用明确的降级实现。
- 诊断工具统一覆盖 Profile / Trace、引擎帧检视与 Viewport Debug；数据来源、实现阶段与验收见 [Diagnostics.md](Diagnostics.md)。

## 四条开发要求与验收

运行时静态网格缓存使用 `MeshAsset::geometryKey()`，不在每个阴影／GBuffer／描边 Pass 重扫全部几何字节。软件光追与 DXR 先比较可见 Actor、Transform、材质和几何版本，输入未变时复用已展开的数据；相机和光照更新继续通过场景常量及 GI 历史规则生效。几何原位编辑须调用 `markGeometryModified()`，契约见 [AssetFormat.md](AssetFormat.md)。DXR 当前仍采用世界空间展开数据，Actor 变换／材质编辑仍会触发重新展开；未将其描述为按 Mesh BLAS 和 TLAS 实例更新的实现。

静态 Mesh Asset Details 按资产、几何、默认材质、Shader 版本及预览资源有效性刷新，不再固定每 0.3 秒重绘同一静态模型。预览取景和视口聚焦复用局部包围盒，变换八个角点求保守世界范围。路径追踪采样数、反弹数和 GPU 同步策略保持项目设置，不用缓存优化隐式降低画质。

### 1. 工程结构

保持 `include/RestArtEngine/<模块>/` 与 `src/<模块>/` 的公共接口／实现分工，`app/` 负责启动与集成。Scene、Assets 不依赖图形 API 或编辑器；渲染参数及特性逻辑不依赖面板布局。引擎与工程资产仍按现有路径规则分离，`.raproject` 继续是纯数据工程。

每项新 Shader、Pass、管线或 Feature 都必须能定位以下内容：

| 内容 | 必须明确的责任 |
| --- | --- |
| 源码与入口 | 引擎实现还是工程可编辑源码，入口函数、include 搜索根与依赖。 |
| 参数与绑定 | 参数定义、默认值、范围、CPU/GPU 布局、资源语义与版本兼容策略。 |
| 资源与执行 | 输入／输出、创建销毁、尺寸依赖、资源读写与 Pass 顺序。 |
| 选择与禁用 | 管线如何组合，特性如何开关，不支持时如何处理。 |
| 验证 | 最小可观察场景、预期画面、相关平台／后端和实际执行结果。 |

小修改不强制拆文件；新职责不应继续无边界堆入 `SceneRenderer.cpp`、`Scene.hlsl` 或编辑器入口。需要新模块时同步接入 CMake／Premake 对应构建规则；不先铺设没有实现的目录树。

### 2. Shader 热编译

源码变更经当前编辑／重载入口编译，在运行中的场景生效。将受影响 Shader 阶段、反射契约、绑定和 GPU 管线对象作为一次候选更新，全部成功后再切换；任何失败都保留上一版可用画面和活动版本，诊断需能定位源码。GPU 仍在使用的旧对象必须按后端生命周期安全回收。

修改 CPU/HLSL 字段或绑定时同步维护契约校验，不绕过现有反射保护。维护 include 依赖，使重新编译使用最新相关源码；若采用异步编译，过期结果不能覆盖较新的请求。文件监听、异步编译和缓存均需实际实现后才能宣称支持。

验收至少覆盖相关成功路径、语法或契约错误保留画面、错误修复后恢复更新；涉及 include、保存／重载或新 Pass 时补充对应路径。C++ 源码按构建流程处理，此要求不代表已有 C++ 热重载。

### 3. 参数实时反馈

硬件光追允许以画质优先，放宽逐帧完成最终质量的要求；普通参数变化仍须自动触发新画面计算，无需 Apply／保存或重启。当前路径采样算法提供渐进与实时两种更新模式：渐进累积完整画面，到目标停止；实时持续更新有界 HDR 历史，并经空间滤波将间接光交给 Forward／Deferred。间接漫反射与镜面贡献分别开关。场景／材质／相机／光照和 Shader 变化使历史失效，曝光仅重新显示。噪点不能作为是否使用硬件光追的判断标准；GI 接口、预算和验收见 [GlobalIllumination](GlobalIllumination.md)。

光照、材质、相机、曝光及新特性公开的普通数值参数，应在下一次可呈现的场景帧生效。拖动滑块时持续更新视口，无需点 Save、重启引擎或重新编译 Shader。通过运行时参数缓冲更新；确需重建资源或编译静态变体的选项要明确其状态和生效条件，保留最后可用画面。

实时预览与持久化分开。当前材质编辑器将有效草稿作为每帧只读覆盖交给渲染器，按材质 ID 同时用于光栅、解析光追与三角形光追，不修改已提交资产。Apply 提交内存中的资产状态，Save 写当前工程磁盘，Discard 恢复最近已提交参数；切材质弹窗中的 Cancel 保留原草稿和预览。切换资产、共享材质、多视图和后端重建时，不能把预览误当成已保存数据。

验收需检查界面操作到实际 GPU 画面的整条链：在有可见影响的测试场景持续调参，并验证恢复原值；涉及草稿时验证 Apply／Save／Cancel，涉及资源重建时验证 resize、切管线或切后端后的值。仅 CPU 数值变化不算通过。

### 4. 可扩展与可移植

将平台窗口／输入、图形 API、Shader 编译与反射、渲染管线与特性、编辑器预览分开。跨平台公共接口使用自有数据类型和资源描述；原生句柄、图形 API 对象及平台数学／编译依赖收敛到明确的适配边界。旧接口泄漏按迁移步骤收敛，不能据此继续扩大依赖。

手写特性声明所需能力与资源，再由后端实现或显式降级；共享逻辑不散落操作系统与 API 条件分支。资源状态、同步和提交契约要能容纳显式 API，不能简单复制 DX11 即时上下文模型。DX12 的资源同步责任与 DX11 有实质差异，设计时须明确归属。[Microsoft：DX11 到 DX12 的重要变化](https://learn.microsoft.com/en-us/windows/win32/direct3d12/important-changes-from-directx-11-to-directx-12)。

移植性验收区分接口可编译、Shader 可编译、能启动、实际画面验证；分别记录 OS、CPU 架构、设备与后端。架构已考虑某个平台，不代表该平台已经运行通过。

## 当前源码基线与缺口

以下列出当前实现与限制；不能仅据接口设计外推平台支持：

| 项目 | 源码现状 | 后续缺口 |
| --- | --- | --- |
| 场景渲染 | `SceneRenderer` 实际使用 D3D11，支持 Forward／Deferred。 | 与后端资源、命令和呈现接口尚未统一。 |
| 后端接口 | `IRenderBackend` 包含多 API 类型与生命周期占位；独立 DX12／DXR 1.1 通道通过共享图像接入 DX11 场景。 | 尚无完整 DX12、Vulkan、OpenGL 场景后端切换；枚举不等于可用实现。 |
| 硬件光追 | 项目配置总开关及独立反射／阴影开关；实际 DXR RayQuery、BLAS／TLAS、共享材质与实时草稿。 | 当前仅 Windows 同适配器 DX11／DX12 组合；能力与限制见 [HardwareRayTracing](HardwareRayTracing.md)。 |
| 全局光照 | 共用算法／模式／贡献请求与状态契约；DXR 路径采样支持渐进累积和实时间接光，Forward／Deferred 消费共享 HDR，漫反射与镜面贡献独立可配。 | 当前仅 PathTracing 算法及 Disabled；实时模式有空间滤波，无运动重投影，其他算法和后端尚未实现。 |
| Shader 热编译 | 候选编译 2 个 VS、6 个 PS 和可用的硬件计算管线，失败保留活动版本。光栅专属材质允许在硬件关闭时更新并明确禁用不兼容硬件管线。 | 文件监听、异步编译与编译缓存尚未实现。 |
| 选中描边 | 独立遮罩与合成通道，深度区分可见实线和遮挡虚线，前方物体描边可选。 | 最终遮挡形态、多选及缩放需定向画面验证；无选中时跳过 pass 和 mask 分配，但初始化仍创建 Shader 和状态。 |
| 工具预览 | 材质球、网格和场景复用离屏渲染；材质窗口、资产详情、场景缩略图各持有独立纹理和读回入口。 | 工具取景距离上限 80，不保证任意尺度场景完整入镜。 |
| 实时参数 | 光栅及硬件光追的光照／曝光、Actor 变换与材质有效草稿按帧更新，Apply／Save 保持提交与持久化边界。 | Vulkan／OpenGL 接入时须维护相同语义。 |
| 可移植性 | CMake 非 Windows 会拒绝配置；原生窗口、DX11、DirectXMath 和 ImGui DX11 适配存在耦合。 | Linux／macOS、共享渲染接口及编辑器 GPU 适配需要迁移。 |

现有验证入口是 `--editor-self-test` 与 `scripts/Validate-Render.ps1`。后者默认寻找 Premake 输出；验证 CMake 构建时显式传入 `-Executable`，避免测到旧二进制。脚本可通过 `-Project` 指定隔离测试工程，避免启动加载在真实工程生成派生资产。

### 离屏工具预览

`offscreenTarget=true` 将绘制定向场景目标；仅设置 `editorViewport=false` 会输出到交换链。工具预览借用场景目标角落后复制到独立 SRV，主场景随后重绘。当前用途包括 64px 自测、128px 场景缩略图、160px 材质窗口和 190px Asset Details。

`renderMaterialPreview` 将临时球和独立预览资产库绑定到传入材质的实际 ID，不修改工程资产。`PreviewSlot` 将 `MaterialEditor`、`AssetDetails`、`SceneThumbnail` 分开存储；`previewTexture(slot)` 和 `readPreviewPixels(slot, ...)` 获取对应图像，旧 `materialPreview*` 接口固定访问材质窗口。另一槽的渲染或尺寸变化不会替换本槽 SRV；同槽成功改变尺寸后，旧借用指针失效。新资源创建或渲染失败保留上一份图像。

场景取景按世界包围盒计算距离（1.5–80），工具渲染采用相同范围，主场景仍为 2–30。预览尺寸限制在可用场景目标范围内；工具远裁剪面随距离扩展，但仍不能保证任意大型场景完整入镜。场景 sidecar 保存入口与兼容边界见 [AssetFormat.md](AssetFormat.md)。

### 材质实时预览验收

参数预览测试使用隔离工程，验证拖动期间画面逐帧变化、已提交资产与磁盘不变、Shader revision 不变，以及 Discard 恢复和 Apply 前后画面一致。还需比较 Forward、Deferred、解析光追和三角形光追的预览结果与等价提交结果，检查保存／重载及切换材质时的草稿确认。使用 `--editor-self-test` 与 `scripts/Validate-Render.ps1` 获取当前构建的结果，其他平台／后端需分别验证。

### 材质深度测试与面剔除

所有材质具有独立的 `Cull`（Off／Front／Back）、`ZTest` 与 `ZWrite`。新材质默认 Cull Back、ZTest Less、ZWrite On；Cull Off 对应双面，Cull Front 只保留背面并翻转着色法线。旧 `twoSided` 调用与资产仍兼容：旧 `.ramat`／场景快照缺字段按双面读取，现代 `.rasset` 缺字段按单面读取。实时预览、提交、丢弃和保存沿用已有材质流程。剔除覆盖光栅、阴影、解析／三角形光追、DXR 与 GI，并处理镜像缩放；拾取遵循有效预览材质的面规则。GPU 材质 flags 保留 Grid=1、Two-sided=2，新增 Cull Front=4，常量和三角形布局保持不变。

ZTest 支持 Never、Less、Equal、LessEqual、Greater、NotEqual、GreaterEqual、Always 和 Disabled。ZWrite 独立控制写深度；Disabled 同时关闭深度测试与深度写入，Always 则允许通过测试并按 ZWrite 写入。状态逐材质绑定，Forward 与 Deferred 的几何 Pass 一致，阴影贴图固定使用 Less／写深度。深度比较与面剔除的底层语义分别见 [Direct3D 深度状态](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-depth-stencil) 和 [Cull 模式](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_cull_mode)。

当前 DXR 主可见性按最近命中确定。可见场景材质使用 Less／LessEqual 且 ZWrite On 时允许硬件光追／GI；其他深度状态保留用户请求、明确报告实际不可用，并继续光栅显示。恢复兼容状态会重新启用硬件并重置 GI 历史，防止间接图与光栅接收面不一致。深度自定义不是透明排序或自定义光照模型的实现。

验收覆盖平面正反面、镜像缩放、背面法线、各深度比较的重叠几何、Forward／Deferred、解析／三角形反射与阴影、硬件兼容状态恢复，以及 UI 实时预览、Apply／Discard、拾取、持久化和旧文件兼容。材质属性尚未提供通用撤销。定向 GPU 入口为 `--material-render-state-self-test`。

### 视锥剔除与千万三角形基准

`SceneSettings::frustumCulling` 默认开启。每个网格按几何版本缓存局部包围盒；主视图和每个阴影级联分别用自己的裁剪矩阵进行保守包围盒测试，在 GPU 上传／绘制前跳过完全位于视锥外的 Actor。无效包围盒保守保留，镜像和非均匀缩放通过完整对象变换处理。相机视锥结果不用于移除 DXR 场景物体，因为屏幕外物体仍可能提供阴影、反射和间接光。当前未实现遮挡剔除。

规模验收目标为视锥内至少 10,000,000 个三角形、硬件光追与实时 GI 同时开启，帧率以实际分辨率和采样预算报告。`--render-scale-test --capture-dir <输出目录>` 生成独立的 2500×2000 网格单元和两个彩色物体，在 1920×1080 下运行；地形包含一千万个不同的非退化三角形，不用重叠副本计数。基准校验主光栅 GPU 输入／裁剪计数、实际 DXR 场景计数与持续 dispatch、有效实时 GI、像素和图形错误，导出 JSON、BMP 与 CPU Trace。硬件不可用时该规模验收失败，不以软件回退作为通过。

报告分别记录首次准备、暖机、稳态同步帧时间和 DXGI 进程 local／nonlocal 内存使用。同步帧时间包含 CPU 提交至 GPU 完成，排除 Present／垂直同步和编辑器 UI；不等于独立 GPU 执行时间，也不承诺任意场景、材质和采样设置的固定帧率。当前 DXR 仍使用全场世界空间 BLAS，静态帧通过场景输入缓存复用，几何或实例变换变化的重建成本需单独考虑。

## 平台与图形后端矩阵

这是设计范围，不是支持完成表；实际可用性还取决于构建、驱动、设备及功能等级。

| 操作系统 | DX11 | DX12 | Vulkan | OpenGL |
| --- | --- | --- | --- | --- |
| Windows | 已有实际场景路径，后续纳入统一后端。 | 已有独立 DXR 通道；完整场景后端仍为目标。 | 目标后端。 | 目标后端。 |
| Linux | 不列为原生后端目标。 | 不列为原生后端目标。 | 目标后端。 | 目标后端。 |
| macOS | 不列为原生后端目标。 | 不列为原生后端目标。 | 纳入设计；候选为 Vulkan Portability／MoltenVK，路线待定。 | 纳入兼容性评估；范围与最低版本待定。 |

macOS 的 Vulkan 设计需要考虑可移植实现的能力差异。[Khronos Portability Initiative](https://docs.vulkan.org/guide/latest/portability_initiative.html)说明了该范围；[MoltenVK](https://github.com/KhronosGroup/MoltenVK)在 Metal 上提供 Vulkan 的子集。macOS OpenGL 已自 10.14 起弃用，不能据此承诺与其他平台同等的长期能力。[Apple OpenGL 文档](https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/opengl_intro/opengl_intro.html)。是否另建原生 Metal 后端需要单独讨论，当前未实现。

## 拟议职责与目录（待讨论）

下表定义设计落点，名称和文件拆分尚未定稿；按实际重构逐步引入，尚未全部按此拆分目录。

| 职责 | 拟议位置 | 边界 |
| --- | --- | --- |
| 平台适配 | `include/RestArtEngine/Platform/`、`src/Platform/` | 窗口、surface、输入、文件监听及平台生命周期；窗口库选型未定。 |
| RHI 契约 | `include/RestArtEngine/Renderer/RHI/` | 自有资源／管线描述、命令、资源读写／同步、能力查询、呈现契约。 |
| API 实现 | `src/Renderer/Backends/{DX11,DX12,Vulkan,OpenGL}/` | 设备、资源、命令提交、同步、交换链／context 与原生句柄。 |
| Shader 系统 | `include/RestArtEngine/Renderer/Shader/`、`src/Renderer/Shader/` | 源码与 include 依赖、编译变体、统一反射、绑定映射、诊断、候选发布。 |
| 基础 Pass | `src/Renderer/Passes/`、`Shaders/Private/Passes/` | 声明输入输出并完成一个渲染阶段，不持有编辑器状态。 |
| 管线组合 | `src/Renderer/Pipelines/` | 组合 Forward／Deferred 等 Pass，声明执行与资源依赖。 |
| 可选特性 | `src/Renderer/Features/<Feature>/`、`Shaders/Private/Features/<Feature>/` | 参数、能力与依赖、开关及可替换实现。 |
| 工程手写 Shader | 工程 `shaderRoot` 下；现有入口为 `Materials/Standard.hlsli` | 通过有版本的入口与资源契约接入；新增工程入口需实现解析和验证后开放。 |
| 编辑器适配 | 现有 `Editor/`、`ImGui/` | 参数操作与预览会话；GPU 适配随后端变化，面板不直接拥有 API 资源。 |

手写 C++ Pass／管线仍属于引擎模块；若未来允许工程插件提供 C++，需另行设计构建与加载契约，不改变现有纯数据工程定义。先用明确资源依赖与执行顺序的组合实现最小场景；是否引入 Render Graph、数据驱动管线或插件注册，依据实际特性需求讨论。

Shader 作者语言与后端编译目标分开设计。当前 DX11 使用 HLSL/SM5，硬件光追使用 DXC/cs_6_5；Vulkan、OpenGL 的编译产物、反射和绑定映射仍需分别实现验证。不能承诺任意 HLSL 自动跨全部后端；计算 Shader 不支持的像素导数材质需按硬件模式明确处理，见 [硬件光追契约](HardwareRayTracing.md#shader-与数据契约)。跨 API 作者语言与原生 GLSL 支持仍需设计。

## 后端切换与特性策略

后端选择先过滤 OS、已构建模块及设备可用性，再检查管线与必需特性。不把当前 `Auto` 或后端枚举视为已实现的探测和切换流程。目标是在可用实现之间切换；启动时选择与运行中重建设备的实施阶段需单独确定，不能静默把“切换”降为重新编译引擎。

运行中切换的设计必须覆盖：停止旧提交并等待安全边界、保存 CPU 场景与用户请求参数、释放或隔离旧设备关联资源、创建目标后端、重建 Shader／管线／资源、重新连接场景视口与 ImGui GPU 适配。失败时保持或恢复可工作的旧后端；若原生窗口限制要求先释放旧资源，则需要可验证的旧后端重建路径。场景纹理、加载界面与 UI 缓存不能继续引用旧设备对象。

特性策略按三层计算：

| 层 | 定义 |
| --- | --- |
| 构建可用性 | 当前发行版本是否包含该后端或特性；裁剪不能留下未链接引用。 |
| 用户／工程意图 | 想使用的后端、管线、特性与参数；禁用不自动删除实现。 |
| 实际生效状态 | 由设备能力、构建内容和 Pass 依赖决定，记录选用实现或禁用原因。 |

每项可选特性声明所需能力、前置资源／Pass、默认状态及缺失行为。允许“使用通用实现”“使用后端特有实现”“明确降级”“禁用”，具体选择随需求确定；必需能力缺失时拒绝该管线配置并解释原因。禁用生产某资源的特性时，也要替换或禁用消费者，保证基础场景仍有完整输出。

显示用户请求值和实际状态的差别；因设备不支持而禁用不能覆盖用户原偏好，切回支持的后端后可重新计算。对比后端时先比基础场景与共享参数，在声明差异的特性外使用合适的图像容差，不强求跨 API 位级相同。现有 Forward／Deferred 的一致性契约仍按 `Architecture.md` 维护。

## 尚未确定的设计

已确认的是三类 OS、四类 API 和可选特性机制；以下仍需设计：

1. 各平台最低 OS／GPU／API 版本，macOS CPU 架构范围，以及 OpenGL 需要保留哪些特性。
2. macOS 的 Vulkan Portability 路线、窗口／surface 层和是否需要原生 Metal。
3. Shader 作者语言、各后端工具链、资源绑定／反射契约及可用语言子集。
4. 首批公共 RHI 接口与最小 Pass／管线组合，以及后端启动选择和运行中切换的交付顺序。
5. 首批特性的保留／禁用／降级矩阵；材质实时预览在后续多视图与设备重建中的一致性。

这些讨论不阻止维护当前 DX11 功能；每次新增能力应朝以上边界收敛，并用实际运行结果更新现状。
