# GUI 开发状态

本页记录 GUI 聊天相关的阶段与证据，领域契约见 [GuiDevelopment](GuiDevelopment.md)、[Architecture](Architecture.md) 和 [EditorGuide](EditorGuide.md)。本机 artifacts 不随仓库分发，不存在时只作历史记录。

## GUI 专项配置（2026-10-23）

新增 GUI 角色、skill、UI 元数据与设计规范，覆盖任务／状态设计、信息架构、布局、Black / Dark Blue 风格、外部标准适用范围、实现与截图／交互验收。TOML／YAML 解析、技能格式和本地链接检查通过；普通子 agent 显式读取技能完成只读窄面板设计演练，未越界实现。

此结果验证配置及兼容分派；客户端原生命名角色加载、完整键盘路径、辅助技术与跨屏 DPI 分别验收，不能由配置文件存在推断通过。

## 系统材质窗口与 Scene / Details（2026-10-26）

本聊天将材质参数和共享 Shader 编辑迁至独立系统窗口，并将 Actor 编辑页命名 Scene，Mesh 信息页命名 Details，默认左下同组。底部保留 Meshes。支持真实材质入口、复用／关闭重开、主窗最小化继续编辑、主窗口焦点保护及旧布局增量迁移。

原完整工作区的 Windows/DX11 验证见本机 `artifacts/native-material-validation/pass4/` 和 `artifacts/mesh-details-validation/pass3/`，覆盖原生窗口、选择、布局、事务与双主题。该记录不表示同目录中其他任务也随本聊天提交。提交前另从 HEAD 提取本聊天变更，在隔离 worktree 重新构建和运行；最终结果见提交说明。

Visual Studio 式单窗显隐、图钉自动隐藏和命名布局仅完成对标设计，未在本聊天实现。跨显示器字体 DPI 和其他平台／后端未验收。

提交前隔离验证已完成：以当时已提交的 `3c6de25` 为基线，仅叠加本聊天 GUI 增量，Premake / MSVC Debug 构建及完整编辑器自测通过，D3D11 debug layer 错误 0；保留已提交材质预览／Two-sided 的行为和回归覆盖。最终证据位于本机 `artifacts/gui-chat-commit/build-scoped.txt` 与 `validation-pass4/editor-stdout.txt`，GUI 文档的 64 个本地引用通过检查。测试使用独立 worktree 和克隆工程，原工作区文件内容保持不变。
