# GUI agent 协作约定

GUI 专项角色为 [restarengine_gui](../.codex/agents/restarengine_gui.toml)，入口为 [GUI skill](../.agents/skills/restarengine-gui/SKILL.md) 和 [GUI 设计规范](GuiDevelopment.md)。当前 GUI 主聊天可直接承担该职责，无需新增一层经理。

任务先明确目标、设计／实现／只读审查模式、相关源码与已有改动、唯一文件写入者和验收条件。角色负责 Project Manager、编辑器信息架构、面板、主题、交互与 Dear ImGui 实现；纯 Shader、Pass、GPU 和图形后端工作交给渲染专项。

按实际依赖分派独立子任务，共享文件只有一名写入者。执行者保留他人改动，不自行扩大范围；发现接口或文件归属变化时先协调。源码事实与约束见 [Architecture](Architecture.md)，操作与事务见 [EditorGuide](EditorGuide.md) 和 [UndoTransactions](UndoTransactions.md)。

回报包含实际改动、执行命令与退出码、截图／交互证据、未验证范围。协调者检查 diff 和证据后再交付。构建通过不等于交互通过，设计稿不等于运行实现；构建、GPU 和桌面输入测试串行或隔离，使用克隆工程及本次拥有的测试进程。

客户端未暴露命名角色时，普通子 agent 显式读取角色与技能执行；配置创建、兼容分派和原生角色加载分别报告。不固定模型、provider 或权限，不提交本机配置、凭据、原始聊天或运行产物。
