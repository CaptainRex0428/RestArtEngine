# DevLog01 之后的开发变化与 DevLog02 交接

核对日期：2026-10-26。此页是开发事实与证据索引，不是分享正文。逐提交审查结合当前源码、领域文档和已有日志完成；本轮没有重建或重跑引擎，没有改用户 Sandbox。写作状态及文章文件由本机 `docs/ZhihuSeriesLedger.md` 管理。

## 范围与版本

- 用户指定起点：`DevLog01` → `6236ce32df8584ccaed66f5d112096db177f1cc5`。
- 本轮冻结终点：`4193f55`，包含窗口测试修正；最后一个运行时功能提交为 `36f4a42`。之后的文档整理不算新增引擎功能。
- 主核对区间：`DevLog01..4193f55`，共 **20 个提交**，累计涉及 71 个文件。逐项记录见下表，不用提交标题代替代码事实。
- 引擎版本仍为 `0.2.0`，`Engine.raeengine` 的 `engineHash` 仍为 `dev-0.2.0`，不能因篇号变化编造新版本。
- 另两条任务线的 6 个原始提交是终点的祖先；本聊天的 5 项交付整合到 `36f4a42`，原始历史有本机备份。三条任务线均包含在此范围内。
- 当前 Sandbox 的材质 1–4、场景修改和新材质 5 是未提交用户内容，不作为本期引擎功能或干净基准场景。

### 第 1 期正文与标签的差额

现存第 1 期正文头部仍是 `8d80547 → cb8fb25`，而 `DevLog01` 标签落在 `6236ce3`。本轮按用户指定标签审计，不改旧正文、不移动标签、不假定已公开发布；正式写作需按实际已发布正文确认衔接。以下 3 个提交介于正文终点与标签之间，单列为待决定是否补述的素材，不混入上面的 20 个提交计数：

| 提交 | 变化 | 现状与证据 |
| --- | --- | --- |
| `6ed32a3` | 为知乎正文／配图／临时目录增加忽略规则。 | 后续 `8d64456` 明确规范、底稿和 DevLog skill 也为本机文件；不写成运行时功能。 |
| `6a9de59` | 新增 `RayTracing_Simple` 康奈尔盒与镜面球示例，补充两个示例工程文档。 | 见 [工程架构](Architecture.md)；本轮发现其 5 个独立 `.ramat` 含无效 JSON，详见“待办与不能宣称的能力”。 |
| `6236ce3` | 场景可选 `background: black`；复用常量缓冲已有分量控制背景，并支持保存／读取。 | [World.cpp](../src/Scene/World.cpp)、[Scene.hlsl](../Shaders/Private/Scene.hlsl)、[文件与持久化](Architecture.md#文件与持久化)。不是新增后处理或 GI。 |

## 逐提交核对

证据编号 E1–E8 指向后文日期明确的运行记录。表中“保留”指当前实现存在；历史中间方案被替换时明确注明。

| 提交／日期 | 实际改动、优化及最终状态 | 事实入口／证据 |
| --- | --- | --- |
| `8d64456` / 10-21 | 统一 Project Manager 名称；生成脚本从自身位置定位仓库并传递 Premake 退出码、移除 pause；调整演示实例与窗口日志兼容；写作文件改为本机忽略。不是新增 agent 框架。 | [GenerateWIN.bat](../scripts/GenerateWIN.bat)、[工程管理](ProjectManagement.md)、[分发](EngineDistribution.md)、`.gitignore`。生成脚本未在本轮重跑。 |
| `c1d6674` / 10-21 | 初版悬浮开关、几何／位掩码持久化、Content 页签重排；配置 Premake 调试程序／工作目录；撤回上一提交额外演示实例。开关和独立 panels.ini 方案后来被替换。 | [RestArtEngine.lua](../RestArtEngine.lua)、[编辑器指南](EditorGuide.md#rider-编译按钮)。不将演示数据增删算新增引擎能力。 |
| `7e13e25` / 10-22 | 去掉悬浮开关，采用标题拖动与边缘缩放，临时实现手写吸附、视口重排与遮挡输入处理。手写吸附已由官方 Docking 替换。 | [Godot 研究记录](GodotGuiStudy.md)、后续 `b64fcdf`；这是演进过程，不是两套现存布局系统。 |
| `b64fcdf` / 10-22 | 接入官方 ImGui docking 分支及 EditorDockManager；场景纹理合成按可见 Image 矩形统一投影、拾取、Gizmo；隐藏标签不回退覆盖全屏；窄面板控件重排和布局恢复。 | [布局与合成](Architecture.md#编辑器布局与视口合成)、[ImGui 版本](vendor/imgui.md)、E1。Godot 是研究来源，未整体移植其 GUI。 |
| `d6cdbf5` / 10-23 | 引入 UndoStack、值快照、redo 截断和 64 步上限；Actor 新建／复制／删除及单次 Gizmo 拖拽入栈；加载场景清空旧历史。 | [UndoStack.cpp](../src/Editor/UndoStack.cpp)、[事务规范](UndoTransactions.md)、E1。不是全属性自动撤销。 |
| `2384636` / 10-23 | 命令注册表统一稳定 ID、分组、快捷键、enabled 判断、菜单和按钮动作；减少入口判断分叉。 | [EditorCommands.cpp](../src/Editor/EditorCommands.cpp)、[命令注册表](UndoTransactions.md#命令注册表)、E1。部分应用级快捷键仍在主循环。 |
| `cb3e0a1` / 10-23 | 同一 Actor／同一变换字段在 700ms 内连续提交合并，保留最早 undo 和最新 redo，减少历史噪声。 | [事务规范](UndoTransactions.md)、E1；旧标题中的 Details 变换控件如今位于 Scene。 |
| `b02936e` / 10-23 | 主 Actor＋多选集合；Ctrl／Shift 切换成员；整组 Gizmo 与批量删除各一条事务，撤销恢复实例与顺序。 | [多选](UndoTransactions.md#多选)、E1。Shift 不是范围选择，Ctrl+D 多选复制仍逐个入栈。 |
| `fdee08c` / 10-23 | 工具栏改为两行，高度从字体／padding／spacing 计算，减少工具栏覆盖 Dock 内容。 | [窗口外框与工具栏](EditorGuide.md#窗口外框与工具栏)、E1；最终又接入正确菜单栏语义和自定义标题栏 WorkPos。 |
| `bed8459` / 10-23 | F11 无边框覆盖当前显示器，隐藏编辑面板；恢复窗口位置、样式与原 UI 状态；F11／Esc／F1 返回。 | [MinimalApp.cpp](../app/MinimalApp.cpp)、[编辑器指南](EditorGuide.md)、E6。不是独占显示模式。 |
| `d62960b` / 10-23 | 抽出 EditorTheme；Black／Dark Blue 共用几何样式、即时切换、工作目录偏好保存和无效值回退。 | [EditorTheme.cpp](../src/ImGui/EditorTheme.cpp)、[主题切换](EditorGuide.md#主题切换)、E1。没有新增用户字体／DPI 设置。 |
| `48797b6` / 10-23 | 纠正主题“未提交／待验证”的中间文档状态并整理引用。 | [GodotGuiStudy](GodotGuiStudy.md)、[Architecture](Architecture.md)；纯状态维护，不另计功能。 |
| `c94095d` / 10-26 | 建立渲染角色、技能、工程／热编译／反馈／可移植性标准，记录三平台四后端设计目标。 | [渲染开发](RenderingDevelopment.md)、`.codex/agents/restarengine_rendering.toml`。设计目标不等于已实现后端。 |
| `1d9b8aa` / 10-26 | 材质草稿通过只读覆盖进入渲染，拖动中即见变化；保留 Apply／Save／Discard，光栅与 ray 消费者采用一致预览。 | [材质编辑](EditorGuide.md#material-参数编辑)、[SceneRenderer.cpp](../src/Renderer/SceneRenderer.cpp)、E1／E3。预览不等于写入资产或磁盘。 |
| `3c6de25` / 10-26 | 逐材质 Two-sided 统一面剔除、背面法线、镜像、光追、阴影与拾取，加入格式校验和兼容。 | [渲染开发](RenderingDevelopment.md)、[AssetSerialization.h](../src/Assets/AssetSerialization.h)、E1／E3。新建默认单面，旧文件缺字段默认双面。 |
| `39416f0` / 10-26 | GUI agent、skill、视觉与交互设计流程、外部参考和验收规范。 | [GUI 开发](GuiDevelopment.md)、[Agent 协作](AgentDevelopment.md)。角色配置与运行时功能分别陈述。 |
| `c95860c` / 10-26 | ImGui 适配层加入显式启用的原生 platform 工具窗、设备资源、消息转发、焦点与生命周期处理。 | [ImGuiLayer.cpp](../src/ImGui/ImGuiLayer.cpp)、[ImGui 依赖](vendor/imgui.md)、E1。默认仍可保留单窗口模式。 |
| `5dfe4b5` / 10-26 | 材质／Shader 移入可离开主窗的系统窗口；主窗最小化仍可编辑，关闭保留草稿；Scene／Details 页签及旧布局迁移。 | [GUI 开发](GuiDevelopment.md)、[编辑器指南](EditorGuide.md)、E1。Details 后续按实例语义纠正。 |
| `36f4a42` / 10-26 | 整合图片 Logo 标题栏、Alt 单／组复制原子事务、实例 Details、配置式资源 GC、用户 Capture 选址及模态外框修复、加载页工作区适配、协作流程；修复 ImGui 归档导入库警告并补齐 CMake 命令源文件；保留前述材质与原生窗口功能。 | [Architecture](Architecture.md)、[GarbageCollection](GarbageCollection.md)、[DevelopmentStatus](DevelopmentStatus.md)、E1–E8；细分行为见下一节。 |
| `4193f55` / 10-26 | 窗口测试将按下／抬起统一排队，避免同步输入重入原生最大化过程的风险；保留全部断言；记录三条任务线完整性。 | [Test-WindowChrome.ps1](../scripts/Test-WindowChrome.ps1)、E5／E6。只修测试注入，不改变引擎窗口代码；具体重入消息顺序未做 trace。 |

## 最终变化按系统归类

| 系统 | 之前的限制 → 当前变化 | 优化的性质与边界 |
| --- | --- | --- |
| 工作区与窗口 | 手工划分面板／拖离后黑区 → 原生 Dock 树、纹理视口重排、六个主窗面板、独立材质窗。标题重叠 → 样式计算的菜单／工具行和工作区裁剪；外框统一图片 Logo 与主题。 | 布局、输入和维护优化；六个主窗面板不等于全部支持跨屏原生窗口。系统吸附弹窗、逐屏字体 DPI 未完成验收。 |
| 编辑行为 | 缺少撤销和批量操作 → 命令注册、事务、700ms 数值合并、多选／组变换／删除、Alt＋Gizmo 单／组复制一次撤销。 | 降低误操作恢复成本；不涵盖名称、可见性、资产引用、材质与 Shader 草稿的通用属性撤销。 |
| 实例与材质 | Details 看 Mesh 资产 → 看主选中 Actor 身份、当前材质与其网格信息；参数需 Apply 才见效 → 连续草稿预览，确认／持久化仍显式。 | 信息目标纠正和反馈延迟改善；共享材质仍共享，不是实例独立材质副本。Shader 仍需显式 Apply 编译。 |
| 渲染一致性 | 缺少逐材质面规则 → Two-sided 在光栅、ray、阴影、拾取和保存之间保持一致。 | 正确性与可控性；没有新增 DXR、完整 GI、多光源或其他已可用后端，没有 FPS／帧耗时提速基准。 |
| 资源与截图 | 缓存缺少统一闲置策略 → 可配置周期的派生资源 GC；用户截图自动落默认目录 → 原生 Save As、自选位置、取消不写盘。 | 回收闲置 GPU mesh／ray 缓冲、字体 CPU 像素和 ImGui 临时缓冲；加载图在 Ready 后及时释放。不是扫描任意 C++ 对象，不删除活资产／草稿／撤销历史。 |
| 临时文件保护 | 临时输出缺少明确归属 → 只有显式 `--capture-temp` 完成登记的截图才能按保留时间回收。 | 未登记、被用户改过、正在写入的文件保留；永久 Capture／显式 capture-dir／测试证据不参与回收。Windows 句柄／链接保护不外推到其他 OS。 |
| 工程与验证 | ImGui StaticLib 合并系统导入库产生 LNK4006 → 最终程序负责系统链接、CMake INTERFACE 传递依赖；补齐 EditorCommands 构建入口。 | 修复依赖归属，没有屏蔽告警。加载页采用 viewport WorkPos／WorkSize 与完整 UV；Shader UI 比较适配 CRLF，API／磁盘断言仍逐字节。 |
| 协作与文档 | 分散任务依赖聊天 → core 协调、context 恢复、delivery 验收，配合 GUI／rendering 专项和领域事实源。 | 明确文件写入者、接口、测试和完成状态；本轮实际分派了三份只读提交审计，不将文件存在说成所有客户端都原生加载。 |

细节补充：实时材质输入增加有效数值钳制与稳定状态行，减少拖动时布局跳动；双面支持同时修正 Sphere／Plane 绕序，并在现有 GPU 参数布局中打包 Grid／Two-sided 标记。Alt 复制直到首次有效变换才创建副本，手势内只复制一次，Gizmo 外 Alt 拖动仍为相机操作。视口底部提示已移除。GC 默认检查／内存闲置／临时保留时间分别为 60／60／3600 秒，每轮临时目录最多检查 512 项，并非严格耗时预算。截图新增借用 `FILE*` 的受保护写入，像素分配移到 Map 前以避免异常留下映射资源；按用户指令删除旧 Capture 图片是一次性清理，不是 GC 自动识别能力。

## 证据索引与写作等级

本次新增的是逐提交、文档和文件格式核对。下表的运行记录发生在 **2026-10-26 集成／补验阶段**，本轮已核对其存在及结果，没有重跑。下一写作会话按技能采用 A＝当轮实际执行观察、B＝实现／既有证据已核对但当轮未运行、C＝计划；不能自动把历史 A 当作新会话 A。可先用本表选择需要重现的少量场景。

| 编号 | 记录与来源 | 支持的结论／边界 |
| --- | --- | --- |
| E1 | `artifacts/linear-integration/editor/stdout.txt`、`editor/captures/`；完整 `--editor-self-test`，工程来自已提交 Sandbox 的隔离副本。 | 主题／菜单／输入／实例 Details、原生材质窗生命周期、预览／Two-sided、事务／Alt 复制、Dock 重排／迁移、Shader 失败回滚通过；D3D11 错误 0。 |
| E2 | `artifacts/linear-integration/gc/stdout.txt`、`gc/captures/`；`--gc-self-test`。 | 活缓存保留、闲置 GPU 释放、重建画面一致、ray 消费者、字体／UI 缓冲、设置保存重载及手动回收通过。 |
| E3 | `artifacts/linear-integration/render/stdout.txt`、`render/captures/validation-results.json`；14 帧 `--self-test` 后 `Validate-Render.ps1 -SkipCapture`。 | 前向／延迟、光追效果、Unlit 光照独立性、resize、法线与 UI；16 项带 Passed 的图像比较通过，D3D11 错误 0。不是性能基准。 |
| E4 | `build/gui-material-vs2022/Testing/Temporary/LastTest.log`；CTest Debug 2/2。 | GC CPU 文件保护／调度及 LoadingScreen artwork／fallback／完整 UV／标题裁剪／三种尺寸／正负桌面原点。负坐标模拟不是跨屏 DPI 交互验收。 |
| E5 | `artifacts/linear-integration/native-capture-confirmed/results.txt` 与 PNG。 | 实际 F9／鼠标 Capture、中文选址、覆盖拒绝与确认、取消保旧、主窗模态前后标题像素、场景不变和干净退出通过。截图含桌面路径／个人文件夹时，发布前必须检查处理。 |
| E6 | `artifacts/linear-integration/native-chrome-final/results.txt`。 | 编辑器／Project Manager 命中、最小化／最大化／还原／双击标题、F11／Esc、F9、脏关闭保护及严格退出码检查通过。首轮失败与隔离修正验证保留，未删失败证据。 |
| E7 | [DevelopmentStatus](DevelopmentStatus.md) 中的构建、静态库警告和加载页记录；`artifacts/build-link-validation/`、`artifacts/loading-screen/loading-stage-1.png` 至 `loading-stage-3.png`。 | 两套 Debug 构建通过；早期依赖修复还做过 Premake Debug／Release Rebuild。加载 15%／65%／100% GPU 图来自临时诊断，最终已移除自动导出。不能说本次又重建 Release／Dist。 |
| E8 | `artifacts/linear-integration/all-task-commits.json`、提交祖先／diff核对；[AgentDevelopment](AgentDevelopment.md) 与专项记录。 | 六个原始提交和整合功能完整；配置解析与文档引用验证。不是所有平台功能验收。 |

历史 2026-10-24 的跨进程临时截图回收、junction／硬链接／写入保护等进一步证据见 [GC 文档](GarbageCollection.md) 与 [开发状态](DevelopmentStatus.md)。symlink 的 3 项历史用例因权限跳过，不能归入全部通过。具体释放量只表示对应场景的逻辑资源统计，不等于工作集或物理显存立即下降。

本机 artifacts 和 build 不随仓库分发；缺失时按上述脚本与源码用隔离工程重新生成。当前范围没有可发表的“渲染提升 N%”或“AI 节省 N 小时”测量。

## 待办与不能宣称的能力

1. 第 1 期正文终点和标签的 3 个提交差额、实际公开链接及发布日期仍待写作阶段确认。本轮仅准备第 2 期，不创建正文、不修改历史正文、不打发布 tag。
2. 标签前 `RayTracing_Simple/Content/Materials/1.ramat` 至 `5.ramat` 的 roughness 数值后有多余引号；本轮严格 JSON 解析五项均失败。场景内嵌材质与独立材质文件是不同入口，不能因场景可读就声称“Reload material assets”可用；此问题不属于主核对区间，本轮只登记、不修改工程素材。
3. 旧 `scripts/Test-Window.ps1` 对空 ExitCode 容忍，不能单凭其旧 PASS 行声称严格 exit 0；最终 E6 使用严格退出码检查。后续可单独完善旧脚本。
4. 参数 UI 自动化重点覆盖 Roughness／Metallic／Two-sided；Base color／Grid 和切材质确认各按钮没有各自完整注入证据。多选、输入组合并非穷尽测试。
5. Visual Studio 式面板显隐／图钉／命名布局、层级、贴图／UV／切线、节点材质、DXR、其他图形后端仍不是本期已完成能力。场景背景色与解析反射不能写成完整间接光照。

## 下一写作会话的接续

先读本页和 [开发状态](DevelopmentStatus.md)，再读本机 `docs/ZhihuSeries.md`、`docs/ZhihuSeriesLedger.md` 与 DevLog skill。正式正文仍写到本机配置指定的外部目录；写作规范、底稿和 DevLog skill 当前由 `.gitignore` 保持本机归属，此页与领域文档负责跨机器开发事实交接。

推荐主线是“从可运行的编辑器到可重排、可撤销、可实时调材质的工作区”。正文可选择五个技术模块：工作区与窗口、编辑事务、实例与材质／双面渲染、资源与 Capture、构建与验证；开头另交代本阶段真实的 agent／skill 协作与返工。不要照抄逐提交表，不重新讲第 1 期已有的全部渲染算法。最终日期、标题、选图和当轮运行等级由写作会话确定。
