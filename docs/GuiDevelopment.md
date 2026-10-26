# GUI 设计与开发规范

本页是 RestArtEngine GUI 的设计、实现与验收入口，覆盖 Project Manager、启动加载页和编辑器工作区。用户任务与最新明确要求决定本轮范围；下述默认风格延续现有实现，可以随具体设计任务演进。创建 GUI agent / skill 不代表界面已重设计，也不代表下述质量目标已全部实现。

产品和模块边界见 [README](../README.md)、[Architecture](Architecture.md)，操作行为见 [EditorGuide](EditorGuide.md)，命令和事务见 [UndoTransactions](UndoTransactions.md)。[GodotGuiStudy](GodotGuiStudy.md) 保留研究与阶段快照，后续实际行为需核对源码。任务与验证进度统一回到 [DevelopmentStatus](DevelopmentStatus.md)。

## 工程基线（2026-10-23 快照）

以下取样于 2026-10-23 工作区，只表示已读到对应实现；本轮没有重建或运行引擎。之后执行任务需重新核对相关文件与未提交改动。

| 范围 | 源码入口 | 已有实现与边界 |
| --- | --- | --- |
| GUI 适配 | [ImGuiLayer.cpp](../src/ImGui/ImGuiLayer.cpp)、[依赖版本](vendor/imgui.md) | Dear ImGui `v1.91.9b-docking`，Win32 / DX11；未接通跨平台 GUI 后端。 |
| 窗口与启动 | [MinimalApp.cpp](../app/MinimalApp.cpp)、[LoadingScreen.cpp](../src/Editor/LoadingScreen.cpp) | Project Manager 当前仍在入口实现；加载页使用品牌背景与相对布局。 |
| 工作区 | [EditorDockManager.cpp](../src/Editor/EditorDockManager.cpp) | 五个稳定面板身份，原生 Docking、分割、合并标签、主窗口内悬浮及布局恢复；没有独立 OS 多视口窗口。 |
| 面板与会话 | [SceneEditor.cpp](../src/Editor/SceneEditor.cpp) | Outliner、Details、Viewport settings、Content & Editors、工具栏、草稿和关闭提示。Meshes / Material Editor / Shader Editor 是内部标签；不存在独立 AssetBrowser、MaterialEditor 或 OutputLog 类。 |
| 视口交互 | [ViewportInteraction.cpp](../src/Editor/ViewportInteraction.cpp) | 场景图像、拾取、相机、ImGuizmo与多选。 |
| 命令与历史 | [EditorCommands.cpp](../src/Editor/EditorCommands.cpp)、[UndoStack.cpp](../src/Editor/UndoStack.cpp) | 已接入动作共用命令和 enable predicate；应用级快捷键、部分旧输入路径仍分散。撤销覆盖有限，不能描述为全部属性可撤销。 |
| 视觉与字体 | [EditorTheme.cpp](../src/ImGui/EditorTheme.cpp)、[ImGuiLayer.cpp](../src/ImGui/ImGuiLayer.cpp) | 默认 Black、可选 Dark Blue；配色与共用几何集中，字体仍在适配层。没有完整设计 token、字体配置或跨屏 DPI 体系。 |

Scene / Assets 不依赖 ImGui、ImGuizmo 或 DX11；Renderer 提供场景纹理，不管理 Dock 节点。GUI 专项负责编辑器布局、控件与交互；Shader、Pass、GPU 资源和后端能力由渲染专项负责。跨界任务先约定数据与预览契约，再分派文件。

## 默认设计方向与视觉规则

面向技术美术的桌面工具：以场景视口和高频编辑为中心，使用稳定的停靠面板、清晰分组、紧凑但可读的属性控件。借鉴 Godot 的职责和交互，不引入其 GUI 运行时，也不把其他引擎的品牌图标当本项目资源。Project Manager 和加载页可使用不同的信息密度，仍应保持主题、术语和反馈一致。

| 设计方面 | 任务中的要求 |
| --- | --- |
| 信息层级 | 先划分全局命令、场景操作、对象属性、资产编辑与反馈，再安排位置；主要工作保持在视口及上下文面板，低频选项渐进展开。 |
| 主题与语义色 | 复用 EditorTheme，Black / Dark Blue 的信息层级与控件几何一致。新增共享色按用途命名，如背景层、正文、次要文字、强调、选择、警告和错误；状态不能只用颜色表达。 |
| 排版 | 沿用当前字体加载路径，检查中英文、数字、单位、路径和快捷键。标签与值对齐，单位和精度一致；长名可截断但应能查看完整内容，错误与主要操作不能只剩省略号。 |
| 间距与尺寸 | 新控件优先使用现有样式和 `GetFrameHeight()` / 文本测量；共享调整进入主题或复用控件，局部例外说明用途。不要为每个面板另建间距体系。 |
| 图标与操作 | 使用一致的线宽、尺寸和含义；图标按钮提供名称或提示、焦点与可用状态。高风险操作明确命名，避免含义模糊的仅图标按钮。复用项目资源并核对外部资源许可。 |
| 反馈 | 区分正常、悬停、按下、选中、键盘焦点和禁用；适用时补充空内容、加载、无结果、错误和未保存状态。错误说明原因及下一步，耗时工作让用户看见状态。 |
| 尺寸变化 | 按可用内容宽度安排表格列、换行、堆叠、滚动或溢出入口；低频操作可收纳，主要动作仍可达。不能靠每帧强制面板位置挤出用户布局。 |

当前共用几何的参考值为 WindowPadding `(18,18)`、FramePadding `(10,6)`、ItemSpacing `(10,10)`、ItemInnerSpacing `(8,6)`，Window / Child / Frame / Popup 圆角分别为 `9 / 7 / 5 / 7`。这些是源码基线，不是已确认的全局设计 token 标准；正文数值与代码不同则先查明改动。字体为 ImGui 默认拉丁字体，Windows 本机字体可用时合并 16px 微软雅黑中文字符。各面板仍有局部尺寸和提示色，不能声称已经全面统一。

若任务要求新风格，交付可比较的布局和视觉方案，说明其对现有工作流、密度与可读性的影响。新配色、字阶、密度档或 token 表先标明为提案；在用户已授权的设计范围内推进可审阅产物，不把每个阶段都变成额外审批。

## 外部标准如何使用

以下官方资料于 2026-10-23 核对。桌面编辑器采用其中适用的设计原则与可测量目标；WCAG 原本面向 Web，CSS 像素不直接等于本工程物理像素，不能据此宣称 ImGui 编辑器获得完整 WCAG 合规或屏幕阅读器支持。

| 资料 | 用于本项目的设计与验收 |
| --- | --- |
| [WCAG 2.2：文字对比度](https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum.html) | 普通有效文字以至少 `4.5:1` 为设计目标；满足该标准大字号定义的文字为 `3:1`。禁用、纯装饰等例外单独判定，不将重要说明伪装成禁用文字。 |
| [WCAG 2.2：非文字对比度](https://www.w3.org/WAI/WCAG22/Understanding/non-text-contrast.html) | 识别可操作控件或状态所必需的图形线索，以相邻背景至少 `3:1` 为目标；无需机械要求所有装饰分隔线达标。 |
| [WCAG 2.2：目标尺寸](https://www.w3.org/WAI/WCAG22/Understanding/target-size-minimum.html) | 参考 `24×24 CSS px` 的最小目标及间距/等价入口等例外，结合桌面逻辑尺度验证实际命中面积；高密度控件兼顾误触和操作效率，不把移动端尺寸直接套用到全部工具栏。 |
| [Microsoft：键盘交互](https://learn.microsoft.com/en-us/windows/apps/develop/input/keyboard-interactions) | 设计 Tab / Shift+Tab 顺序、焦点可见、Enter / Space 激活、Esc 退出当前上下文，以及作用域明确的快捷键；选择适合控件类型的方向键行为。目标是完整操作路径可达，现有实现需逐项实测。 |
| [Dear ImGui：当前锁定版本 FAQ](https://github.com/ocornut/imgui/blob/v1.91.9b-docking/docs/FAQ.md) | 输入捕获、稳定 ID、自绘控件、字体与 DPI 按仓库锁定版本核对。先读本地对应源码，不能把新版 API 直接套到旧版本。 |

可读性评估需同时检查实际截图和语义色数值：计算真实背景/前景对比度，检查透明混色和细线显示；截图观感不能代替数值检查，数值达标也不能代替实际字号与布局检查。键盘和辅助技术能力分别验收；开启键盘导航标志不证明所有自绘控件可访问。

## 按任务规模执行设计流程

设计、实现和审查是不同任务模式。只读审查保持只读；仅要求方案时产出方案和验收条件，不自动改运行时代码。实现请求可在授权范围内完成设计、代码和验证。小修复用短说明串联以下步骤，复杂工作区调整再形成完整方案，不要求每次都写一套设计报告。

| 阶段 | 需要解决的问题 | 可审阅产物 |
| --- | --- | --- |
| 任务与现状 | 谁在何时做什么、当前阻碍、完成后可观察变化；现有入口、截图、源码、工作区和约束是什么？ | 简短任务说明，范围和验收条件；区分已确认需求、提案、已实现和已验证。 |
| 信息架构与交互 | 信息归属、主要/次要动作、选择和焦点、操作前后状态、提交/取消/失败路径是什么？ | 面板/命令归属图、任务流程或状态表；优先复用现有行为。 |
| 布局原型 | 默认与窄窗口如何布局，停靠/浮动/折叠时如何变化，主要操作能否到达？ | 标注尺寸规则的线框或可交互原型；固定视口/窗口/字体条件便于比较。 |
| 视觉与组件规格 | 复用哪些主题、控件、文案、图标、状态和命中规则；新增规则放哪里？ | 必要的视觉稿或组件说明，注明提案与实施项。原型/效果图不作为运行证据。 |
| 工程实现 | 文件归属、命令与事务、生命周期、布局持久化、输入仲裁及渲染接口如何落地？ | 明确接口后的代码与领域文档；共享文件只有一名写入者。 |
| 验收与迭代 | 实际用户路径、画面、恢复/失败、两主题及尺寸变化是否符合条件？ | 本次构建、截图和定向交互证据；缺口明确为未验证或未实现，稳定结论回写文档。 |

布局原型可按任务选用 Markdown / Mermaid、独立 HTML 或直接引擎内原型，产物形式服务于验证问题，不预设新 GUI 技术栈。保留一份简明设计记录即可：`用户任务 → 当前问题 → 布局与状态 → 复用/新增规则 → 实现边界 → 验收证据`。

## 实现中必须守住的契约

1. **面板身份与布局。** 面板及 Dock 根 ID `RestArtEngine.DockSpace.v1` 影响持久化与测试。实时停靠交给 ImGui，内部 DockBuilder 隔离在 DockManager 的默认布局创建／恢复与兼容迁移中。已有布局不得每帧重置；更名先处理稳定 ID 与迁移。布局 ini 和主题偏好是本机设置，不写进场景或材质；沿用原生 ini，不另起一套布局序列化。
2. **顶部区域与视口。** 工具栏、DockSpace 和场景图像使用同帧布局。菜单在 BeginMenuBar / EndMenuBar 中绘制，不能在普通内容区用 BeginMenu + SameLine 模拟。图像、投影、拾取、相机与 Gizmo 使用本帧实际图像屏幕矩形，遮挡/隐藏区域不能继续消费视口输入。
3. **输入与控件身份。** 平台输入始终传给 ImGui 后端，再仲裁编辑器/场景是否消费；不因捕获标志而停止给 ImGui 送事件。依文本输入、弹窗、活动控件、面板焦点与拖动所有者仲裁输入；不能仅凭鼠标在视口内就接管全部快捷键。拖动由起始命中的所有者保持或明确取消，不能在中途抢占。稳定业务 ID 与显示名分离，列表重排/重命名不改变编辑对象。自绘控件要接入实际命中、键盘焦点和状态反馈，不能仅画图。ImGui 的输入捕获标志与本帧命中状态一起核对，避免错误屏蔽或双重消费。
4. **命令、编辑与保存。** 已接入动作复用 EditorCommandRegistry，菜单/按钮/快捷键共用 enable 条件；输入作用域由调用方正确处理。新操作明确撤销范围和一次手势的历史粒度，保存值快照与稳定 ID，不持有可能因容器扩容失效的 Actor 指针。属性面板连续变换、批量操作和 Gizmo 聚合遵循事务文档，不擅自把全部操作标为可撤销。
5. **预览与草稿。** 材质实时预览、Apply 内存提交、Save 磁盘保存、Discard 恢复已提交状态各有含义；切换资产和关闭处理未提交/未保存状态。Shader 草稿与最后可用管线分开，编译失败保留可用版本。GUI 的文案、禁用条件和状态反馈必须与真实行为一致。GPU 实时反馈改动与 rendering 专项共同验收。
6. **平台窗口与资源。** 窗口绘制和原生命中区域同步修改，关闭沿用 WM_CLOSE 和未保存提示；保留拖动、缩放、系统菜单和最大化语义。F11 是主窗口全屏，不是独立视口。纹理/字体资源按所属层管理创建和释放。Windows / Linux / macOS 的架构目标不等于已经有各平台 GUI 实现。

## 验收选择与覆盖边界

按变化选择检查，沿用本页与 [GUI 协作约定](AgentDevelopment.md)。配置/文档改动只做配置解析、引用与流程演练；不因新增 agent 而运行 GPU。低风险文案修改不写复述实现的测试。影响画面时检查真实截图，影响行为时执行定向用户路径，必要时补充能发现回归的测试。

| 变化 | 相关验收 |
| --- | --- |
| 主题、字体、组件与布局 | Black / Dark Blue 的层级、状态和对比度；默认与受影响窄布局、长中英文/路径、缩放后无重叠和不可达操作。确认正常/禁用/错误等实际相关状态。 |
| Dock 或视口区域 | 分割、合并标签、浮动回嵌、恢复默认、重新启动读回；同帧图像矩形、遮挡、拾取、相机和 Gizmo。 |
| 编辑交互 | 文本和场景快捷键隔离、焦点和弹窗、单选/多选、一次操作撤销与重做；适用时覆盖 Apply / Save / Discard / Cancel、切换与关闭。 |
| 原生窗口 | 原生命中、缩放、最小化恢复、最大化、关闭确认、F11；改动涉及吸附/跨屏时单独做 OS 交互，不能用命中值代替。 |
| 材质/Shader 反馈 | 渲染专项的实际 GPU 图像、连续预览、提交一致性、失败恢复与资源生命周期；仅 CPU 值改变不通过。 |

已有自动化入口是 [EditorSmoke.cpp](../app/EditorSmoke.cpp)、[Test-Window.ps1](../scripts/Test-Window.ps1) 和 [Validate-Render.ps1](../scripts/Validate-Render.ps1)。执行前核对脚本参数并显式传入本次可执行文件；桌面输入和 GPU 测试串行，使用隔离的工程、布局与输出目录，保留用户正在编辑的工程及进程。

新环境使用 `cmake --preset windows-vs2022` 与 `cmake --build --preset windows-debug-engine`。已有配置优先使用对应本工作区的目录。编辑器自测示例为 `build/cmake-vs2022/Debug/RestArtEngine.exe --editor-self-test --capture-dir artifacts/gui-validation`；窗口脚本须通过 `-Executable` 指向同一产物。

截图注明构建、主题、窗口尺寸、字体/样式缩放和场景，前后比较保持条件一致。当前测试中的 150% 字体/样式缩放不能证明操作系统跨显示器 DPI 支持。系统吸附弹窗、跨屏、完整键盘路径、中文输入法以及屏幕阅读器等，只有实际检查过才报告通过；不因本页列出目标而视为已覆盖。

## GUI 专项协作

角色为 [restarengine_gui](../.codex/agents/restarengine_gui.toml)，技能为 [restarengine-gui](../.agents/skills/restarengine-gui/SKILL.md)。当前 GUI 主聊天可以直接采用该技能；独立的实现或审查按 [协作规范](AgentDevelopment.md) 分派，无需再创建一层经理。客户端未暴露该命名角色时，普通子 agent 显式读取角色与技能执行，报告兼容分派；文件存在不等于原生角色已加载。

可直接请求“用 GUI 专项 agent 设计 Details 面板”或使用 `$restarengine-gui`。任务交付包含设计决策、实际改动、验证结果和未覆盖范围；配置创建、源码实现、构建通过、交互通过、已提交分别记录。
