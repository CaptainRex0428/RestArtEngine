# GUI 开发状态

本页记录 GUI 聊天相关的阶段与证据，领域契约见 [GuiDevelopment](GuiDevelopment.md)、[Architecture](Architecture.md) 和 [EditorGuide](EditorGuide.md)。本机 artifacts 不随仓库分发，不存在时只作历史记录。

## GUI 专项配置（2026-10-23）

新增 GUI 角色、skill、UI 元数据与设计规范，覆盖任务／状态设计、信息架构、布局、Black / Dark Blue 风格、外部标准适用范围、实现与截图／交互验收。TOML／YAML 解析、技能格式和本地链接检查通过；普通子 agent 显式读取技能完成只读窄面板设计演练，未越界实现。

此结果验证配置及兼容分派；客户端原生命名角色加载、完整键盘路径、辅助技术与跨屏 DPI 分别验收，不能由配置文件存在推断通过。
