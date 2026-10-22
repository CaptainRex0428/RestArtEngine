# Godot 编辑器 GUI 研究与 RestArtEngine 移植约束

本文记录对 Godot 编辑器源码的研究结果，以及 RestArtEngine 已实现的停靠基础层与后续扩展方向。当前实现使用 Dear ImGui 官方 Docking，不包含 Godot GUI 源码的整体移植。

## 结论

Godot 的编辑器 GUI 不是一个可以脱离引擎复制的资源包。它建立在 Godot 自己的 `Object`、`Node`、`Control`、`Container`、`Theme`、信号、输入和 `DisplayServer` 体系上。把 `editor/` 目录整体复制到 RestArtEngine 不会得到可编译的 GUI，反而会引入 Godot 的对象系统、资源系统和编辑器插件系统。

RestArtEngine 借鉴 Godot 的职责划分和交互行为，使用 Dear ImGui 作为实现层。只有边界明确、依赖可隔离的算法或布局数据结构才适合逐段移植；若复制上游代码，需按其许可证保留版权与许可信息。本项目另要求记录来源、版本和改动，便于追踪维护。

## Godot 源码入口

| Godot 路径 | 职责 | RestArtEngine 对应方向 |
| --- | --- | --- |
| [`editor/editor_node.cpp`](https://github.com/godotengine/godot/blob/master/editor/editor_node.cpp) | 编辑器总编排、主菜单、主编辑区域、布局加载与保存 | `EditorShell`，负责创建 DockManager、工具栏和编辑器生命周期 |
| `editor/docks/editor_dock_manager.{h,cpp}` | Dock 注册、默认位置、停靠槽、浮动窗口和布局恢复 | `EditorDockManager` |
| `editor/docks/editor_dock.{h,cpp}` | 一个可停靠面板的生命周期和标题状态 | `DockPanel` |
| `editor/docks/dock_tab_container.{h,cpp}` | Tab、分割容器、拖动目标、Dock 配置序列化 | Dear ImGui DockSpace / TabBar 适配层 |
| `editor/docks/scene_tree_dock.cpp`、`inspector_dock.cpp` | 典型编辑器面板实现 | Outliner、Details、Viewport settings |
| `editor/gui/editor_title_bar.*`、`editor/gui/editor_bottom_panel.*` | 标题栏、底部面板和公共编辑器控件 | 工具栏、Content & Editors |
| `scene/gui/control.*`、`container.*`、`split_container.*`、`tab_container.*` | Godot 自己的 GUI 基础控件和布局系统 | 不复制；由 Dear ImGui 提供基础绘制和输入 |
| `editor/plugins/editor_plugin.cpp` | 面板注册、插件生命周期和编辑器扩展入口 | 后续的 `EditorPanelRegistry` |

Godot 官方文档说明 Dock 可以移动、调整边界、浮动、重新嵌入，并且编辑器布局可以保存和恢复。这些行为是本项目要复刻的规范，而不是照搬 Godot 的类层次。

## 当前实现

```text
SceneEditor
├── EditorDockManager
│   ├── 面板身份与默认位置
│   ├── DockSpace 与默认 DockBuilder 树
│   └── Layout 菜单与恢复默认布局
├── World Outliner / Details / Viewport settings
├── Content & Editors
└── Scene Viewport / ViewportInteraction
    └── 场景纹理显示、拾取与 Gizmo

Dear ImGui Docking
├── 实时分割树与标签组
├── 主窗口内悬浮与回嵌
└── RestArtEngine.imgui.ini 原生布局持久化
```

`EditorDockManager` 集中管理五个面板的身份、默认位置和布局恢复；`SceneEditor` 保留场景与资产编辑逻辑。普通面板通过 `drawPanel` 提交内容，视口由 `ViewportInteraction` 提交图像和交互。实时分割、标签合并、悬浮回嵌及布局保存使用 Dear ImGui 的实现，删除了旧版距离判断和手写吸附规则。

默认布局为左侧 Outliner / Details、右侧 Viewport settings、底部 Content & Editors 和中央 Scene Viewport。拖动标签使用原生停靠目标和预览，拖动分隔条改变相邻区域大小。`Layout > Restore default layout` 重建默认布局。当前悬浮仅限主编辑器窗口，未启用独立操作系统窗口或多显示器视口。

视口采用渲染纹理合成：Renderer 提供场景纹理，Scene Viewport 在当前窗口内容区域显示它。拾取、相机投影和 Gizmo 使用本帧图像的屏幕矩形，避免面板移动后继续使用旧渲染区域。Renderer 不管理 Dock 布局，Scene / Assets 仍不依赖 ImGui。

布局持久化使用现有 `RestArtEngine.imgui.ini`，由 ImGui 保存 Dock 树与悬浮窗口几何信息，没有自定义 `EditorLayoutState` 或第二份布局文件。根 ID `RestArtEngine.DockSpace.v1` 将旧版自由窗口配置迁移为默认停靠布局；后续启动使用保存的树，只有缺少根节点或显式恢复默认布局时才重建。该版本标记用于默认布局迁移，不表示本项目拥有另一套序列化格式。

## 后续扩展方向（未实现）

1. **主题层**：将颜色、间距、圆角、字体和控件高度集中到 `EditorTheme`，逐步收敛面板样式定义。
2. **命令与插件**：抽象 `EditorCommandRegistry` 和动态面板注册，统一快捷键与面板生命周期；当前五面板登记是静态的，不是外部插件接口。
3. **布局管理增强**：命名布局、导入导出、异常配置恢复和原子保存策略需单独设计；当前沿用 ImGui 原生 ini 读写机制，不声称已具备这些能力。
4. **独立窗口**：需要同时完成平台消息、DPI、焦点以及渲染后端多窗口资源管理，不能仅打开开关就作为已支持功能。

验证应覆盖停靠后的视口矩形一致性、分割、标签合并、悬浮回嵌、布局读写及既有拾取和 Gizmo 操作。具体通过情况以实际构建和测试输出为准，本文不将验证计划记作测试结果。

## 许可证边界

Godot Engine 主体使用 MIT 许可证，允许复制、修改和商用；复制文件或实质部分时需保留版权和许可声明。MIT 本身不要求在修改文件中标明改动，记录修改来源属于本项目维护实践。Godot 仓库的 `COPYRIGHT.txt` 还列出了第三方组件及其不同许可证；图标、Logo 和文档应按对应文件的许可核对。

本轮没有复制 Godot GUI 源码、图标或品牌资源，停靠实现使用已保留 MIT 许可证的 Dear ImGui。后续如移植 Godot 代码，应逐文件确认许可证，并记录原始路径、版本、版权声明及 RestArtEngine 的修改内容。

参考：

- [Godot Engine 源码](https://github.com/godotengine/godot)
- [Godot 编辑器 Dock 定制文档](https://github.com/godotengine/godot-docs/blob/master/tutorials/editor/customizing_editor.rst)
- [Godot 许可证说明](https://godotengine.org/license/)
- [Godot 版权与第三方许可证清单](https://github.com/godotengine/godot/blob/master/COPYRIGHT.txt)
