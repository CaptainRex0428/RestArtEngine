# 引擎 agent 与 skills 协作规范

本体系服务于 RestArtEngine 开发协作，不是引擎运行时内嵌的 AI 系统，也不是应用中的 Project Manager 界面。主 agent 对用户目标、模块边界、任务分派和最终交付负责；专项 agent 承接有边界的设计、实现或审查；skill 保存可复用流程，领域文档保存需求与进度事实。

## 使用入口

在引擎仓库聊天中使用：

```text
$restarengine-core 实现材质参数实时预览，保留 Apply、Save、Cancel 语义，自动分派并完成验证。
$restarengine-core 继续当前任务，先核对工作区和任务记录，再推进尚未完成的部分。
$restarengine-context 同步当前实现与验证进度。
$restarengine-gui 为 Details 面板设计布局和交互状态，先给方案与验收条件。
```

也可要求“调用主引擎 agent 处理……”。项目角色是 [restarengine_core](../.codex/agents/restarengine_core.toml)，专项角色是 [restarengine_rendering](../.codex/agents/restarengine_rendering.toml) 和 [restarengine_gui](../.codex/agents/restarengine_gui.toml)。当前主聊天可以直接采用对应 skill 承担协调或 GUI 专项工作；若客户端支持命名角色，可使用对应角色。普通子 agent 的兼容方式是显式读取角色 TOML 和技能，不等同于客户端原生加载了该角色。

项目 agent 放在 `.codex/agents/*.toml`，skills 放在 `.agents/skills/<名称>/SKILL.md`，对应官方的[自定义子 agent](https://learn.chatgpt.com/docs/agent-configuration/subagents)和[技能发现](https://learn.chatgpt.com/docs/build-skills)机制。角色只定义职责，不固定模型、provider 或权限；是否能分派、可用并发数及原生角色支持，以当前客户端工具为准。技能未出现在列表时重新打开会话；仍不可用时可按文件路径显式读取。配置文件存在与实际运行验证分别记录。

## 两类标准

| 标准 | 事实来源与职责 |
| --- | --- |
| 项目需求和进度 | [DevelopmentStatus.md](DevelopmentStatus.md) 是统一索引；需求、设计、当前边界仍在各领域文档；[context skill](../.agents/skills/restarengine-context/SKILL.md) 负责核对源码与证据。 |
| 引擎流程和专项特性 | [Architecture.md](Architecture.md) 定义模块与数据契约；[delivery skill](../.agents/skills/restarengine-delivery/SKILL.md) 定义开发、验证、集成交付；专项 skill 定义领域特有约束。 |

`AGENTS.md` 是本机补充入口，不提交；共享技能、角色和本规范可随仓库维护。不复制本机模型配置、凭据或本机写作路径。写作流程不因开发任务自动触发。

## 角色路由与边界

| 工作 | 执行者 | 必须理解的领域特性 |
| --- | --- | --- |
| 目标、依赖、跨模块接口、集成、进度 | core 主 agent | 工作区状态与历史证据、唯一文件写入者、接口先行、验收后完成。 |
| Shader、GPU 参数、Pass、管线、后端 | rendering 专项 agent + [rendering skill](../.agents/skills/restarengine-rendering/SKILL.md) | CPU/GPU 契约、热编译原子替换、画面反馈、生命周期、能力协商与移植边界。 |
| Project Manager、编辑器 GUI/UX、主题、面板、输入与编辑事务 | gui 专项 agent + [gui skill](../.agents/skills/restarengine-gui/SKILL.md) | [GUI 规范](GuiDevelopment.md)、[编辑器指南](EditorGuide.md)、[事务](UndoTransactions.md)；任务与状态设计、视觉规则、实际可见区域、输入归属、事务原子性、布局持久化及截图/交互验收。 |
| 工程、资产、构建与分发 | 主 agent 或有边界的通用子 agent；尚无独立角色 | [工程管理](ProjectManagement.md)、[分发](EngineDistribution.md)；工程纯数据、路径归属、版本兼容、失败不破坏原数据。 |
| 独立验证或审查 | 有需要时分派通用子 agent + delivery skill；尚无独立角色 | 只读审查不写代码，检查实际证据，明确覆盖与缺口。 |

复杂任务按真正依赖拆分，不按表格机械创建全部角色。例如材质连续预览涉及渲染覆盖与编辑器状态：先确定预览／提交／取消契约，再分派独立模块；若都要改 `SceneEditor.cpp`，只指定一名写入者，其他任务提供调查或审查结果。只有可独立执行的部分并行。

## 分派与回报契约

每个子任务都携带以下信息，可写在工具消息中；跨轮次复杂任务同时保存本机记录。

| 输入 | 内容 |
| --- | --- |
| 目标与模式 | 实现、只读调查或只读审查；用户可观察结果和明确范围。 |
| 上下文 | 仓库位置、相关需求与技能路径、代码入口、基线和现有未提交改动。 |
| 依赖与接口 | 前置任务、冻结的接口、不可改变的语义及尚未确定的选项。 |
| 文件归属 | 可写文件、共享文件的唯一写入者；越界需求先回报协调者。 |
| 验收 | 目标平台／后端、场景、预期结果和可用验证入口。 |
| 返回 | 行为变化、改动文件、已执行命令及结论、证据位置、未验证项、阻塞和建议。 |

共享工作区不靠事后覆盖解决冲突。主 agent 在启动写入前确定范围，子 agent 不自行撤销他人改动、不扩大任务、不递归创建经理；需要额外分派时回报主 agent 统一安排。构建、GPU 和桌面输入资源也需要协调。

文件归属是协作约定，不是操作系统锁，无法阻止其他聊天或用户编辑。分派前与收回结果时重新检查相关 diff；发现任务外的新改动则先核对受影响接口，暂缓冲突写入，继续无关工作。只读演练记录采样范围和待复核项，不把变化中的旧结论写成稳定现状。

任务状态采用“待执行 → 执行中 → 待验收 → 完成”，另可标注“阻塞”及具体依赖；子 agent 完成只进入待验收，主 agent 检查实际结果后才完成。源码实现与运行验证是独立事实。文档任务的完成条件是文档／配置验证，不要求 GPU 验证。

复杂任务续接记录放 `artifacts/agent-tasks/<任务标识>.md`，由主 agent 维护：用户目标、相关基线、任务及依赖、文件写入者、当前状态、证据、尚缺条件与下一步。进程或聊天中断后先核对记录与实际 diff；本机记录不在时从共享索引和源码恢复，不假定旧子 agent 仍运行。稳定结果写回领域文档与共享索引，不提交本机原始日志。

## 按需新增专项角色

出现可重复的独立领域工作、明确模块归属和专项验收后再建立角色；一次性工作用有边界的通用子任务。新增时沿用已有文件组织，确保：

1. 角色定义说明职责、边界和必读 skill，不绑定模型或 provider。
2. skill 同时接入项目事实索引与领域标准；领域特有要求写在所属领域文档，避免复制主流程或进度快照。
3. 明确接口／数据契约、资源生命周期、失败恢复、平台限制及可观察验收；不相关的项无需套用。
4. 更新本页路由，解析 TOML、校验 skill 和引用，再以有边界的真实场景或只读演练检查分派行为。原生角色加载、兼容分派与仅配置检查分别报告。

当前配置了 core、rendering 与 gui；其余领域由实际需求推动。角色文件存在、兼容分派演练与客户端原生加载分别验收。
