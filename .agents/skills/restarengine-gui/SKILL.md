---
name: restarengine-gui
description: "Design, implement, and review RestArtEngine Project Manager and editor GUI/UX, information architecture, interactions, themes, components, and Dear ImGui layouts. Use for this engine's GUI work; route pure shader, pass, GPU, or backend work to rendering."
---

# RestArtEngine GUI 设计与开发

承担本引擎工程管理器与编辑器的 GUI 任务。项目角色见 [restarengine_gui.toml](../../../.codex/agents/restarengine_gui.toml)；当前 GUI 主聊天可直接使用本技能，无需另建经理 agent。

## 恢复现状与选择模式

先核对 Git 状态、相关 diff、任务目标与源码证据，保留工作区已有改动。读取仓库 `AGENTS.md`（若存在）及 [GuiDevelopment.md](../../../docs/GuiDevelopment.md)，后者是 GUI 架构基线、设计标准、流程与验收的事实入口。已经核对且没有相关变化的内容无需重复读取。

按任务选读资料：

- 面板、视口与平台职责：[Architecture.md](../../../docs/Architecture.md)；停靠研究及历史边界：[GodotGuiStudy.md](../../../docs/GodotGuiStudy.md)。
- 操作、快捷键与保存：[EditorGuide.md](../../../docs/EditorGuide.md)；命令、输入与撤销：[UndoTransactions.md](../../../docs/UndoTransactions.md)。
- 工程管理器行为：[ProjectManagement.md](../../../docs/ProjectManagement.md)。
- 角色路由、依赖与文件归属：[AgentDevelopment.md](../../../docs/AgentDevelopment.md)。

根据请求采用设计、实现或只读审查模式。设计交付可评估的方案与原型，不自行修改引擎代码；实现推进到可观察验收；审查报告具体问题、影响与证据。用户已授权的后续操作继续完成，不把流程变成逐步审批。

## 从行为到可验证界面

1. 确定用户任务、触发场景、当前痛点与可观察结果，查实际面板和状态来源。分清已有实现、已有运行证据、新提案及未验证项。
2. 先处理信息架构、操作入口、反馈与状态转换，再确定布局和视觉表达。按任务需要覆盖空、禁用、编辑、成功与失败等相关状态；标明草稿、预览、提交、保存及取消边界。
3. 用合适的线框、布局说明或可检查原型表达方案，落实组件、层级和尺寸规则。默认延续已有 Black / Dark Blue 主题与工具型桌面编辑器风格；用户要求新方向时按其目标设计，将新的风格选择注明提案。具体视觉和可访问性标准按 GUI 文档执行，引用外部标准时核对原始资料及适用边界。
4. 获得实现授权后沿现有模块落地。先确定跨模块接口与唯一文件写入者，保留他人改动；保护本帧视口图像区域、输入归属、命令和撤销事务，以及预览／Apply／Save／Discard 语义。纯渲染任务路由到渲染专项执行者，混合任务与主 agent 协调接口。
5. 依据 GUI 文档选择实际截图、交互及必要回归验证，按 GUI 规范交付变化、执行命令、运行证据与剩余缺口。构建通过不能代替视觉和交互验收；设计原型不能证明引擎功能已实现。

流程按影响范围精简：小幅布局或文案修改不必制作整套设计稿；复杂工作流需要先说明交互契约。规则与进度维护在领域文档，不在技能中复制快照。

## 专项角色与交付边界

主 agent 分派时提供目标、模式、文件范围、依赖和验收；当前聊天承担 GUI 主任务时直接使用同一契约。跨模块集成与共享进度由主 agent 统一维护。需要修改分派范围或既定接口时先回报协调者。

客户端支持命名角色时使用 `restarengine_gui`；否则由普通子 agent 显式读取角色和技能执行，并如实说明兼容方式。角色文件创建、配置检查、原生加载与引擎运行验证分别报告，不固定模型、provider 或权限，不修改本机配置。
