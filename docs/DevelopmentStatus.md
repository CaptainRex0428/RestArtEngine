# 开发状态与事实入口

此页供主 agent 和专项 agent 恢复上下文。它是索引与近期交接记录，不替代领域设计文档，也不代表所有已列功能都在当前工作区重新验证。使用方式见 [AgentDevelopment.md](AgentDevelopment.md)。

DevLog02 开发交接已于 2026-10-26 整理到 [DevLog01 之后的变化审计](DevelopmentChanges-DevLog02.md)：逐项核查 `DevLog01..4193f55` 的 20 个提交，映射最终功能、优化、领域文档和运行证据，并单列第 1 期正文终点与标签之间的 3 个差额提交。本轮只核对文档／源码／既有日志，不新增引擎运行结论；本机写作底稿同步到第 2 期准备状态，尚未写正文或记录发布。

审计表的 20 项提交覆盖与顺序、84 个实际本地文档引用、差异空白及新增内容的本机路径检查通过；文档示例里的日期占位图片链接单独排除。独立只读复核未发现提交遗漏或把计划当实现的问题，Sandbox 原文件哈希保持不变。本机核验记录为 `artifacts/devlog02-audit/checks.json`。

审计新发现：标签前 `RayTracing_Simple` 的 5 个独立材质文件含无效 JSON（roughness 数值后多余引号），本轮解析失败已登记，未改工程素材；旧 `Test-Window.ps1` 仍容忍空退出码，严格退出验证应引用最终 `Test-WindowChrome.ps1` 结果。详见上述审计的待办节，不能把这些未处理项写成已完成。

## 需求、进度与标准到哪里查

| 范围 | 权威入口 | 使用时关注 |
| --- | --- | --- |
| 产品范围、已有功能与限制 | [README](../README.md) | “特性”“当前边界”；不要把方向当成已实现功能。 |
| 模块归属、依赖与开发约定 | [Architecture](Architecture.md) | 数据流、CPU/GPU 契约、编辑器合成和验证记录。 |
| 渲染需求、跨平台目标与缺口 | [RenderingDevelopment](RenderingDevelopment.md) | 已确认范围、源码基线、待讨论设计；DX11 实现与其他 API 骨架分开。 |
| GUI 设计规范、工程基线与验收 | [GuiDevelopment](GuiDevelopment.md)、[GodotGuiStudy](GodotGuiStudy.md) | 前者是 GUI 专项设计/开发入口，后者保留研究和阶段快照；后续窗口和工具栏进展见 Architecture。 |
| 编辑器操作与事务边界 | [EditorGuide](EditorGuide.md)、[UndoTransactions](UndoTransactions.md) | 快捷键、保存语义、多选、Alt 复制、撤销覆盖。 |
| 工程和分发 | [ProjectManagement](ProjectManagement.md)、[EngineDistribution](EngineDistribution.md) | 工程资产边界、版本关联、构建与发布流程。 |
| 阴影功能 | [ShadowMethods](ShadowMethods.md) | 实现差异、资源与验证矩阵。 |
| 资源 GC 与临时截图 | [GarbageCollection](GarbageCollection.md) | 定时设置、内存缓存边界、临时截图登记与清理、平台限制。 |
| agent 分工与技能流程 | [AgentDevelopment](AgentDevelopment.md) | 角色路由、任务契约、文件归属、完成条件。 |

## 当前交接基线（2026-10-23）

本次协作体系整理时 HEAD 为 `48797b6`，工作区存在引擎、文档、Sandbox 内容及 agent 配置改动。它们不都由本轮产生；任务启动时重新检查 Git 状态，不将此 SHA 当作全部工作区改动的提交证明。

| 项目 | 已知事实与证据 | 剩余边界 |
| --- | --- | --- |
| 菜单、Alt 拖动复制、自定义窗口栏 | Architecture／UndoTransactions 记录了 2026-10-23 的运行验证；本机 `artifacts/window-chrome-editor/stdout.txt` 可见相关 PASS 与 D3D11 错误 0。 | 历史验证不能自动覆盖后续改动；系统吸附弹窗、跨屏交互仍未据此验证。 |
| 图片 Logo、移除视口底部提示 | 工作区已有对应改动；Logo 的本机运行记录见 `artifacts/window-chrome-logo/stdout.txt`，8 帧且 D3D11 错误 0；提示移除已在此前任务构建通过。 | 本轮是协作配置工作，不重复宣称进行了引擎运行测试。 |
| 核心渲染演进 | RenderingDevelopment 区分实际 DX11 路径与三平台／四后端的设计目标；材质预览任务的最新实现与验证见下一行。 | 真实多后端场景路径仍按领域文档缺口推进，后端枚举不代表可用实现。 |
| 材质参数实时预览 | 材质预览任务已在 Windows/DX11 完成：只读草稿覆盖统一用于光栅与光追，保留 Apply／Save／Discard 边界。独立 MSVC Debug 构建、隔离 Sandbox 编辑器自测、渲染回归通过，D3D11 错误 0；证据见 `artifacts/material-preview-editor/stdout.txt`、`artifacts/material-preview-render-validation.txt`，范围见 RenderingDevelopment。 | UI 自动化实际拖动 Roughness／Metallic；Base color／Grid 和切材质确认分支已审查、未单独注入交互测试；其他平台／后端未运行。本次改动未提交。 |
| 材质 Two-sided | 2026-10-23 在 Windows/DX11 完成逐材质双面开关，含实时预览、面剔除／法线、镜像、反射、阴影、拾取及新旧文件持久化。独立 MSVC Debug 构建、完整编辑器自测和渲染回归通过，D3D11 错误 0；证据见 `artifacts/two-sided-editor/stdout.txt`、`artifacts/two-sided-render-validation.txt`，具体对照范围见 RenderingDevelopment。 | 新材质默认单面，旧缺字段默认双面；其他平台／后端未运行，未新增材质属性通用撤销。本次改动未提交。 |
| agent 协作体系 | core 角色、context／core／delivery 技能已建立，rendering 已接入。两个 TOML 解析、四个开发 skill 校验、41 个本地文档链接检查通过；只读演练实际分派了一个渲染调查子 agent，完成跨模块任务契约与回报。 | 本轮验证了普通子 agent 读取角色／技能的兼容分派；客户端原生命名角色加载未验证。演练没有实现或运行材质预览功能。 |

本机 `artifacts/` 不随仓库分发；不存在时按历史文档记录处理，需要本轮证据则重跑相关验证。详细需求不复制到 skill，未明确排期的缺口不自行提升为当前任务。

## GUI 专项配置与验证（2026-10-23）

按本聊天的 GUI 设计用途新增 [gui 角色](../.codex/agents/restarengine_gui.toml)、[gui skill](../.agents/skills/restarengine-gui/SKILL.md) 与 [GUI 规范](GuiDevelopment.md)，接入主引擎路由和交付流程。规范记录当前工程入口、Black / Dark Blue 风格基线、外部标准适用范围，以及任务/状态设计、布局原型、视觉规则、实现与验收流程；未在本轮修改运行时 GUI。

三份角色 TOML 和 GUI UI 元数据解析通过；本轮涉及的 core／delivery／gui 三个技能通过格式校验，相关本地 Markdown 引用检查通过。独立普通子 agent 读取新技能后，完成了“窄面板材质编辑器先给方案”的只读演练，交付布局、状态与验收条件，保留预览/Apply/Save/Discard 语义，没有越界实现。另一次只读复核未发现工程事实或职责冲突。本机交接记录见 `artifacts/agent-tasks/gui-agent-setup.md`。

这些结果验证了文件配置、规范与显式读取技能的兼容用法；客户端原生 `restarengine_gui` 角色加载未验证。本轮未重建或运行引擎，也不新增跨屏 DPI、完整键盘导航或可访问性达标结论；没有执行提交。

## 资源 GC（2026-10-24）

已接入 Core 调度器、Renderer／ImGui 缓存回收和受管临时截图清理。Project Manager、编辑器帧末及最小化循环使用同一调度；`UI → Garbage collection...` 支持自动开关、检查间隔、内存闲置时间、截图保留时间、保存设置与手动请求。默认分别为 60 秒、60 秒和 1 小时。内存范围、配置格式和永久／临时截图边界见 [GarbageCollection](GarbageCollection.md)。

本轮 Windows/DX11 验证使用独立 MSVC Debug 构建 `build/gc-vs2022` 与隔离 Sandbox：

- Core CPU 回归通过（CTest 1/1）：时间调度、配置校验／替换／失败保旧、跨会话过期文件、修改／未知文件保留、junction／硬链接／锁定文件保护、写入期间目录及文件锁、登记碰撞不覆盖。最终日志为 `build/gc-vs2022/Testing/Temporary/LastTest.log`。目录、截图和登记文件的 3 项 symlink 创建测试因本机权限跳过，不将其记作运行通过。
- GC GPU／UI 自测通过：活缓存保留、闲置 GPU 资源释放、恢复场景画面一致、不同 ray 消费者、字体 CPU 像素／闲置 UI 缓冲、设置草稿／保存／重载、关闭自动后手动请求、Black／Dark Blue 及窄面板滚动。最终证据为 `artifacts/gc-validation/final-gpu.txt` 与 `gpu-final/` 截图。
- 完整编辑器自测与渲染回归通过，D3D11 验证错误为 0；证据为 `artifacts/gc-validation/editor-stdout.txt` 与 `render/validation-results.json`。Premake 工程重新生成后包含新增源文件。
- 实际 CLI 跨进程回归通过：一个进程使用 `--capture-temp` 生成并登记截图，后续进程回收过期文件，同时保留同目录未登记哨兵与永久输出。该场景首轮释放 41,947,604 字节 CPU 缓存（约 40 MiB）；活 GPU 缓存保留。证据为 `artifacts/gc-validation/final-temp-create.txt`、`final-auto-collect.txt`，本机复现脚本为 `verify-temp-lifecycle.ps1`。最后的目录锁／原子发布修正后已重跑 CPU、GC GPU／UI 与此 CLI 回归；没有重复不受影响的完整编辑器／渲染套件。

这属于派生资源／缓存回收，不扫描任意 C++ 对象，不自动删场景、CPU 资产或撤销历史；现有截图及自测证据不回收。跨平台文件保护和其他图形后端未验证，本轮改动未提交。

## 用户 Capture 保存位置（2026-10-24）

按用户要求，工具栏 Capture 与 F9 改为系统“另存为”对话框：由用户选址，默认系统“图片”目录，同次会话记住目录，BMP 扩展名和覆盖确认明确；取消不落盘，保存失败提示且保留编辑器。F9 不随按住自动重复。`--capture-dir` 仅用于批量／自测，手动操作不能通过该参数绕过选框。单独 `--frames N` 不自动导出；自测缺省输出迁到系统临时目录的 `RestArtEngine/Validation`，不再自动生成项目 `screenshots`。

已按本次明确删除指令清除原 `bin/windows-x86_64/Debug/screenshots` 下 3 张用户 Capture 图片（合计 15,552,162 字节）；这不是 GC 自动识别或删除旧截图的能力。工程素材、Logo 和独立历史自测证据保留。

MSVC Debug 构建通过。新增 `scripts/Test-Capture.ps1` 实测 F9 弹框／取消、中文选址保存有效 BMP、覆盖拒绝保旧／确认覆盖、实际鼠标点击 Capture、场景文件不变、干净退出；D3D11 错误 0。测试默认使用系统 TEMP 下的独立工程与图片目录，保存框截图已检查。额外实际运行验证 `--frames` 无输出、显式 `--capture-dir` 输出、`--capture-temp` 登记输出均通过。结果摘要位于本机 `artifacts/capture-dialog/results.txt`。`Test-Window` 与 `Test-WindowChrome` 已同步原生保存框操作并通过解析检查，本轮不重复完整窗口套件。使用说明见 [EditorGuide](EditorGuide.md#capture-截图)，未执行提交。

## 系统独立材质编辑器（2026-10-26）

按用户明确选择，材质参数和工程共享 Shader 编辑已迁至可移出主窗口的系统窗口。Details 的 `Edit assigned material`、Mesh 信息中的实际材质引用以及材质库入口打开／复用该窗口；底部 Content 仅保留 Meshes。工具窗关闭保留会话草稿和预览，主程序退出继续全局未保存确认。主窗口最小化时工具窗可独立编辑；五个工作区面板的 Dock 树和主窗口内工作方式保留。行为与边界见 [EditorGuide](EditorGuide.md)，工程契约见 [Architecture](Architecture.md)。

本轮使用独立 MSVC Debug 构建 `build/gui-material-vs2022` 和隔离 Sandbox，在 Windows/DX11 完成：

- 完整编辑器自测通过，D3D11 debug layer 开启、错误 0。新增验证真实材质入口、独立无 owner HWND、移到主窗外及缩放、重复打开复用、最小化恢复、原生 WM_CHAR、主窗最小化时编辑、两窗最小化后的关闭处理、草稿保留重开、Mesh 多材质真实引用、Shader 草稿关重开和退出无残留窗口；保留参数连续反馈、Two-sided、Shader 编译回滚、拾取、Gizmo、Alt 复制、事务、布局及文本输入隔离覆盖。最终证据为 `artifacts/native-material-validation/pass4/editor-stdout.txt`。
- 渲染回归 14 帧及 `Validate-Render.ps1 -SkipCapture` 图像检查通过，覆盖前向／延迟、光追、Unlit、法线、resize 和 UI；D3D11 错误 0。证据为 `artifacts/native-material-validation/render/stdout.txt` 与 `render/validation-results.json`。
- GC GPU／UI 自测通过，D3D11 错误 0；证据为 `artifacts/native-material-validation/gc/stdout.txt`。窗口外框脚本通过编辑器／Project Manager 命中、最大化／还原、最小化、F11／Esc、实际 F9 另存与脏关闭保留，证据为 `artifacts/native-material-validation/chrome-focus/results.txt`。窗口脚本现显式激活自己启动的测试窗口再注入按键，符合新的原生焦点作用域。
- 材质参数、Shader 与双主题图像从实际原生窗口的 GPU 渲染目标在 Present 前读回，避免桌面遮挡；已人工检查 `pass4/editor/material_native_black.png`、`material_native_blue.png` 和 `shader_native.png`。截图与本机任务记录不提交。

首轮运行发现后端字符串 context 属性在本机不可用，已用整数属性／进程内绑定保证原生输入和关闭，并实际验证该回退；还修复全最小化时待处理关闭不能推进帧的问题。Apply 图像等价检查发现仅 1 像素的 1/255 量化差异，测试仍严格核对全部材质、场景／相机／视口状态，仅该图像比较允许最多 4 像素各一档差异，其余反馈和回滚断言保留。

本机只有一块显示器，跨显示器拖动和逐显示器字体 DPI 未实测；其他平台／图形后端未运行。未修改工程资产格式、运行中的用户工程或用户进程，未执行提交。本机交接为 `artifacts/agent-tasks/native-material-editor.md`。

## Scene / Mesh Details 页签（2026-10-26）

本节为当时实现与验收记录；用户随后纠正 Details 必须以具体场景实例为目标，当前行为以本页“Details 场景实例修正”和领域文档为准。

按用户确认，将原 Actor 属性页改为 Scene，把 Mesh 信息移入 Details；两页默认在左侧 Outliner 下方同组切换，底部 Content 仅保留 Meshes 资源列表及导入／创建入口。选 Actor 显示 Scene，点 Mesh 显示 Details；事件只排队一次，手动切页后不会持续抢回。Details 展示几何信息及真实场景材质引用，材质按钮继续打开独立系统窗口。当前操作与迁移契约见 [EditorGuide](EditorGuide.md)、[Architecture](Architecture.md) 和 [GuiDevelopment](GuiDevelopment.md)。

保留 `RestArtEngine.DockSpace.v1` 和旧面板身份，旧五面板布局仅补入 Scene；旧 Details 自由浮动时按原位置尺寸建立双页签组，已有 Scene 设置继续尊重用户安排，不重置其他区域。

本轮使用独立 MSVC Debug 构建 `build/gui-material-vs2022` 与隔离 Sandbox，在 Windows/DX11 完成验证：

- 完整编辑器自测通过，D3D11 debug layer 开启、错误 0；最终日志为 `artifacts/mesh-details-validation/pass3/editor-stdout.txt`。新增真实 Mesh 点击后的动态几何信息与单处展示、真实 Actor 点击后的 Scene 字段、手动 Details 激活后连续三帧保持；原生材质入口、草稿与连续反馈、Shader 回滚、拾取、Gizmo、Alt 复制、事务及文本输入隔离继续通过。
- 隔离 ImGui context 覆盖旧停靠 Details、旧自由浮动 Details 和已有独立浮动 Scene 三类布局，各自保存后再次创建 context 读回；未关联面板的停靠／初始几何保留，Scene 无重复创建。原生 Dock 拖动、合并、分割、恢复默认及视口重排仍通过。
- 已检查本次实际 GPU 主窗口截图：`pass3/editor/inspector_details_black.png`、`inspector_details_blue.png`、`inspector_scene_black.png`、`inspector_scene_blue.png`，条件为 1440×900、默认字体／样式缩放、隔离 Sandbox 自测场景。另有原工具栏 980／1440 宽及 150% UI 缩放检查；它不等同于全部属性内容或跨屏 DPI 验收。
- 当前说明已同步 README、EditorGuide、Architecture、GuiDevelopment、UndoTransactions 与 ImGui 依赖说明；53 个本地文档链接和差异空白检查通过。

首轮切页测试补足必要的悬停帧后，分别断言自动切页、手动激活和保持，避免把未激活误报为回跳；一次后段截图写入失败在独立输出目录重跑通过，保留失败证据并增加路径／系统错误码诊断，没有降低功能断言或增加写入重试。其他平台／后端及逐显示器字体 DPI 未在本轮验证，未执行提交。本机交接见 `artifacts/agent-tasks/mesh-details-scene-tabs.md`。

## 静态库链接警告（2026-10-26）

已复现并修复 DearImGui 的 3 条 `LNK4006 __NULL_IMPORT_DESCRIPTOR`：Premake `StaticLib` 的系统 `links` 被写入 librarian 的 `AdditionalDependencies`，合并了 d3d11／dxgi／d3dcompiler／dwmapi 导入库。现从 `vendor/imgui.lua` 移除该归档输入，由最终 `RestArtEngine` 目标解析系统依赖；CMake 将 DearImGui 的系统需求明确为 `INTERFACE`，保留导出目标消费者的链接需求。没有添加告警屏蔽或改 vendor 源码。

已重新生成 Rider／Visual Studio 工程，核对 Debug／Release／Dist 的 DearImGui 归档配置均无系统导入库。Premake 全 solution Debug、Release Rebuild 通过，各 0 警告／0 错误；CMake `build/gui-material-vs2022` Debug 的 RestArtEngine 与依赖 clean build 通过，无警告。修复前／后日志位于本机 `artifacts/build-link-validation/`。Dist 未实际重建；本轮仅调整构建依赖，未重跑 GPU／编辑交互测试，未提交。构建源配置、最终链接归属与告警处理规范已写入 [Architecture](Architecture.md#构建依赖与警告)。

## Details 场景实例修正（2026-10-26）

按用户纠正，Details 已从 Content 网格资产检查改为主选中 Actor 实例检查：显示名称、Actor ID、可见性、当前材质以及该实例网格的几何／来源。材质入口位于网格统计之前，默认面板高度下可直接操作。两个 Actor 共享 Mesh 时仍分别读取各自的 Material；不再汇总所有使用同 Mesh 的实例。Content 浏览／导入不改变 Details 目标或抢页签；Outliner／视口选择一次性激活 Details，手动 Scene 页保持稳定。多选标识主实例，空选／删除清空信息，撤销恢复后重新解析；Details 纳入 Actor 输入作用域。Scene 编辑控件、共享材质编辑语义、存档格式及原生窗口／Dock 布局保持原契约。

CMake `build/gui-material-vs2022` Debug 与 Premake Debug 构建通过。隔离 Sandbox 的完整 `--editor-self-test` 通过，D3D11 验证错误为 0；新增运行覆盖同 Mesh 不同材质的双向切换、Content 选择隔离、实例材质入口、Outliner／视口选择、空选、删除／撤销恢复、多选主实例及手动页签稳定。既有原生材质窗口、实时参数、Shader 回滚、Alt 复制、变换、输入隔离与布局迁移回归继续通过。最终日志为 `artifacts/instance-details-validation/pass4/stdout.txt`，双主题截图为 `pass4/editor/inspector_instance_details_black.png`／`inspector_instance_details_blue.png`，已检查实例身份和当前材质可见。

初次断言误将 Outliner 的 Ground 实例名当作另一材质；校准为锁定版 ImGui 实际按钮日志 `[ label ]` 后，仍保留材质入口存在及另一实例材质不存在的双向断言。原生 backend context 属性回退日志与既有实现一致。其他后端、跨屏 DPI 未运行；本轮没有重跑无关渲染／GC 套件，未提交。续接记录见本机 `artifacts/agent-tasks/instance-details.md`。

## Capture 弹窗期间外框变白修复（2026-10-26）

已复现并修复用户报告：Capture 原生保存框打开时，主窗口被经典浅色系统标题栏与边框覆盖。消息级像素观察将覆绘定位到 `WM_NCACTIVATE` 默认处理；`WindowChrome` 对非最小化窗口直接返回 `TRUE`，最小化仍走默认处理，保持焦点切换和模态禁用／恢复。未修改 Capture 选址、文件保存或主窗口样式；临时诊断代码已移除。平台处理约定见 [Architecture](Architecture.md)。

CMake `build/gui-material-vs2022` Debug 和 Premake solution Debug 构建通过，无编译警告。新增桌面画面回归在修复前失败（标题背景 RGB 从 `31,31,32` 变为 `201,215,229`），最终构建的 Black／Dark Blue 均通过并已检查实际 PNG：F9、鼠标 Capture、嵌套覆盖确认期间自绘标题栏与外框保持主题，取消恢复 owner 可用；Unicode BMP 保存、拒绝／确认覆盖、场景不变和零退出码继续通过，D3D11 验证错误 0。Black 使用 CMake 构建，Dark Blue 使用 `bin/windows-x86_64/Debug/RestArtEngine.exe`。证据分别为 `artifacts/capture-chrome/black/`、`darkblue/` 的 `results.txt`、`owner-before.png`、`owner-modal-f9.png`、`owner-modal-toolbar.png`、`owner-modal-overwrite.png` 和 `owner-after.png`。

`Test-WindowChrome.ps1` 同轮通过编辑器／Project Manager 的标题与边角命中、最小化、最大化工作区、还原、双击、干净关闭，以及编辑器 F11／Esc／缩放／脏关闭保护；结果位于 `artifacts/capture-chrome/window-regression/results.txt`。只操作隔离测试工程与本次测试进程；本轮未重复无关渲染／GC 自测，未验证跨屏和系统吸附弹窗，未提交。

## 启动加载页裁切与空缺修复（2026-10-26）

用户截图中的左侧 Logo／名称／状态文字被裁掉、右侧与底部留空，源于启用 ImGui 多 viewport 后仍把加载页绘制原点写死为 `(0,0)`，并以整个客户区尺寸绘制。`LoadingScreen` 已统一使用主 viewport 本帧 `WorkPos / WorkSize`：完整背景和进度层对齐标题栏下方工作区，明确归属主 viewport 并裁剪到该工作区；调用接口不再接受容易混淆坐标系的 display width／height。保留现有完整图片拉伸与相对比例设计。

CMake 与 Premake Debug 构建通过。新增 `LoadingScreenTests` 通过，覆盖实际 WIC 解码／WARP 纹理创建、缺图回退、完整 UV、标题栏排除、1440×900／980×700／480×360，以及正坐标／负坐标平移后文字与进度几何一致；允许 ImGui 字形像素对齐造成的 1 像素差异。实际启动流程的 15%／65%／100% GPU 帧通过临时诊断导出，已检查背景、Logo、完整名称、状态与进度条，图片位于 `artifacts/loading-screen/loading-stage-1.png` 至 `loading-stage-3.png`；诊断导出代码已移除并重建最终程序，不增加普通启动的自动截图行为。隔离 Sandbox 启动、进入编辑器及退出成功，D3D11 验证错误 0。测试仅模拟负桌面坐标，未声称完成跨屏 DPI 交互验收；未提交。

## 主线集成与提交状态（2026-10-26）

将 `codex/chat-editor-fixes` 的五项交付压缩整合到 `master` 的 `5dfe4b5` 之后，保留主线已有的独立系统材质窗口、Scene／Details 页签、实时材质预览和 Two-sided。整合覆盖自定义图片标题栏、Alt 复制、实例 Details、GC、Capture 选址与弹窗外框修复、加载页工作区适配、构建依赖以及协作技能／文档；Sandbox 用户场景和材质不纳入提交。上文的“未提交”是对应阶段的历史状态，以本次主线集成为准。

集成前逐项比较两条提交线与工作区，补回 Shader UI 自测的 CRLF 归一化：ImGui 输入过滤 CR，UI 回读比较采用可输入文本，API／磁盘原样恢复断言继续逐字节检查。依赖文档同步 Details 的选中 Actor 实例语义。

本次最终代码在 Windows/DX11 的验证：CMake Debug 与 Premake／MSBuild Debug 构建通过；CTest 的 GC 与加载页测试 2/2 通过；从已提交 Sandbox 创建隔离副本，完整编辑器自测、GC GPU／UI 自测、14 帧渲染自测及图像比较均通过，D3D11 验证错误 0。覆盖独立材质窗口、实时预览、Two-sided、实例 Details、Alt 单／多选复制与撤销、Shader 回滚、停靠重排及布局恢复。本机证据为 `artifacts/linear-integration/{editor,gc,render}/`。本轮不重复原生 Capture 保存框的桌面像素测试，其实现与此前通过版本一致，历史证据见上文；不新增跨平台／跨屏验收结论。原分支历史和集成前工作文件已在本机备份。

## 三条任务线完整性复核（2026-10-26）

在整合提交 `36f4a42` 上复核全部相关任务提交，以下六个原始提交均为当前 `master` 的祖先，已提交功能无需再次 cherry-pick 或另造重复提交：

| 任务 | 提交 | 保留内容 |
| --- | --- | --- |
| 渲染规范 | `c94095d` | 渲染角色、技能、工程结构、热编译、实时反馈及跨平台设计边界。 |
| 材质实时预览 | `1d9b8aa` | 只读草稿预览、连续滑块反馈及 Apply／Save／Discard 契约。 |
| 双面材质 | `3c6de25` | 逐材质 Two-sided、光栅／光追／阴影／拾取、持久化与旧文件兼容。 |
| GUI 规范 | `39416f0` | GUI 角色、技能、主题与设计／验收流程。 |
| 原生工具窗基础 | `c95860c` | 独立平台窗口、消息／焦点处理、最小化及销毁生命周期。 |
| 材质窗口与面板 | `5dfe4b5` | 独立材质／Shader 窗口、草稿保留、Scene／Details 页签与旧布局迁移。 |

分别完成渲染和 GUI 的只读专项复核：关键 HLSL、资产结构与序列化和双面材质提交一致；独立材质窗口及材质／Shader 控件核心实现和 GUI 提交一致；后续 GC 与实例 Details 改动保留两侧契约。Details 改为选中 Actor 实例属于后续需求修正。Visual Studio 式显隐／图钉／命名布局仍是设计提案，三平台／四后端仍以 RenderingDevelopment 中的已实现边界为准。

复用上节同一运行时代码的两套 Debug 构建、CTest、完整编辑器／GC／渲染回归证据；新增真实桌面补测 `Test-Capture.ps1`，覆盖 F9、鼠标 Capture、中文选址、取消、覆盖拒绝／确认、模态期间标题像素及关闭恢复，全部通过，D3D11 错误 0，已检查弹窗期间和关闭后的实际截图。证据为 `artifacts/linear-integration/native-capture-confirmed/`。

窗口补测首轮在 Project Manager 双击最大化后的还原步骤失败。脚本原本以同步 `SendMessage` 发送按下、异步 `PostMessage` 发送抬起；`IsZoomed` 观察到状态变化时，原生最大化处理仍可能未退出，同步按下存在重入风险。现将两边输入统一排队，不改引擎运行时、不放宽断言或添加重试。隔离脚本和最终 `Test-WindowChrome.ps1` 均通过编辑器／Project Manager 最大化、还原、最小化、双击标题、命中区域、F11／Esc、实际 F9 保存及脏关闭保护。失败及两次成功证据分别保留在 `native-chrome-confirmed/`、`native-chrome-queued-probe/`、`native-chrome-final/`；未记录消息级追踪，不将重入顺序推断写成已观测的运行时故障。

提交归属核对记录为本机 `artifacts/linear-integration/all-task-commits.json`。Sandbox 原始文件哈希保持不变，仍不纳入本次提交。上述运行范围为 Windows/DX11；跨屏 DPI、系统吸附弹窗及未实现后端不新增通过结论。

## 维护方式

协调者在阶段完成或任务交接时更新本页，领域执行者回报证据并更新获准范围内的领域文档。记录日期、实现／验证／提交状态、证据入口和下一步；结果变化时修正当前条目，历史验证保持日期与范围，不以旧结果覆盖新失败。

进行中的复杂任务使用 `artifacts/agent-tasks/` 里的本机记录；共享页保留稳定里程碑与明确的未完成事项。新会话先读本页，再按任务选读领域文档与源码，不依赖私人聊天或旧会话才能工作。
