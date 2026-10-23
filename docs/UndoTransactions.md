# 编辑事务研究：Godot UndoRedo 与 UE FScopedTransaction

本文记录对 Godot `UndoRedo` 和 Unreal Engine 事务系统的源码研究，以及 RestArtEngine 已落地的编辑撤销 / 重做、合并模式、命令注册表与多选。结论先行：**借鉴两者的模型，不复制任何代码**。

## 结论

两个引擎解决的是同一个问题——把一次编辑变成一个可回放的单元——但入口完全不同：

- **Godot 是「声明式」的**：调用方在编辑前后分别登记 do / undo 操作，`commit_action()` 执行 do 操作并把 Action 压入历史。历史有一个游标，提交新动作时截断重做尾巴；`MergeMode` 让连续同类操作合并成一条，`max_steps` 限制历史上限。
- **UE 是「作用域式」的**：`FScopedTransaction` 在构造时开始事务、析构时结束，中间的对象修改由引擎的对象序列化自动捕获，`Cancel()` 可回滚整个作用域。

RestArtEngine 没有反射和对象序列化，无法自动捕获「中间发生了什么」，因此只能走 Godot 的路线（显式声明两个方向）；但 UE 的**作用域**语义直接解决了「一次拖拽 = 一条记录」的问题，于是两者结合：`UndoStack` 提供历史与游标，拖拽用作用域边界（开始 / 结束）聚合成一条记录。

## 上游源码入口

| 上游 | 路径 | 关键内容 |
| --- | --- | --- |
| Godot | [`core/object/undo_redo.h`](https://github.com/godotengine/godot/blob/master/core/object/undo_redo.h) | `MergeMode`、`Operation`、`Action`、`create_action` / `add_do_method` / `add_undo_method` / `commit_action` / `undo` / `redo` / `set_max_steps` |
| Godot | [`core/object/undo_redo.cpp`](https://github.com/godotengine/godot/blob/master/core/object/undo_redo.cpp) | 提交时截断重做尾巴、`_process_operation_list` 执行顺序、合并窗口、`action_level` 嵌套 |
| UE | `Engine/Source/Editor/UnrealEd/Public/ScopedTransaction.h` | `FScopedTransaction` 构造开始事务、析构结束、`Cancel()`、`IsOutstanding()` |

UE 仓库需要 Epic 账号授权；本次通过公开镜像阅读了 `ScopedTransaction.h`（UE 4.x），只取作用域语义，未复制代码。

## 采纳与不采纳

| 上游机制 | 是否采纳 | 本仓库的做法 |
| --- | --- | --- |
| 声明 do / undo 两个方向 | 采纳 | `UndoStack::push(label, undo, redo)`，两个闭包都捕获**值拷贝** |
| 游标 + 提交时截断重做尾巴 | 采纳 | `entries_.resize(cursor_)` 后追加，`cursor_` 指向已应用前缀 |
| 历史上限 | 采纳 | `setMaxSteps()`，默认 64，超出丢弃最旧记录 |
| 作用域聚合成一条 | 采纳 | Gizmo 拖拽期间只记录「拖拽前」快照，松手时才 `push` |
| 合并模式（MERGE_*） | 采纳（时间窗） | `UndoStack::Merge{key, windowMs}`：同键窗口内保留最早 undo、刷新最新 redo；Details 面板三个变换控件按 700ms 合并 |
| `action_level` 嵌套 | 未采纳 | 当前没有宏动作或复合命令 |
| 对象引用保活 | 未采纳 | 记录保存 `scene::Actor` 的值拷贝，不持有指针 |
| 对象序列化自动捕获 | 未采纳 | 引擎无反射；改为调用方显式提供两个方向 |

## 当前实现

```text
include/RestArtEngine/Editor/UndoStack.h        历史、游标、上限、合并策略
src/Editor/UndoStack.cpp
include/RestArtEngine/Editor/EditorCommands.h   命令注册表（id + 分组 + 快捷键 + 可用判定）
src/Editor/EditorCommands.cpp
src/Editor/SceneEditor.cpp                      把编辑动作登记成命令并写入历史
app/EditorSmoke.cpp                             命令 / 合并 / 撤销 / 多选的断言
```

覆盖范围：

| 操作 | 记录方式 |
| --- | --- |
| `+ Sphere` / `+ Cube` / `Add Actor from mesh` | 记录新 Actor 的拷贝；撤销时按 id 删除，重做时追加 |
| `Duplicate` | 同上；多选时为每个 Actor 各记一条 |
| `Delete`（单选或多选） | 记录每个 Actor 的拷贝与原始下标；撤销时按升序插回原位，当前仅选中其中一个恢复的 Actor |
| Gizmo 平移 / 旋转 / 缩放（单个或整组） | 拖拽开始前记住整组「稳定态」变换，松手时将所有变化的 Actor 合为一条记录 |
| Details 面板数值拖动 | 控件激活时记下稳定态，`IsItemDeactivatedAfterEdit` 后按 700ms 时间窗合并 |

## 命令注册表

`EditorCommandRegistry` 把动作收敛成稳定 id（如 `actor.delete`、`edit.undo`、`scene.save`），每条命令带分组、标签、显示用快捷键、键位和「是否可用」判定：

- 顶部 `Edit` 菜单按分组渲染，可用状态与实际选择一致；
- Outliner 的 `+ Sphere` / `+ Cube` / `Duplicate` / `Delete` 按钮通过 `commandButton(id)` 渲染并调用同一命令；
- 键盘快捷键由 `processShortcuts()` 统一匹配，文本输入获得焦点或 Gizmo 使用中时整组不触发。

默认快捷键：Ctrl+Z 撤销，Ctrl+Y / Ctrl+Shift+Z 重做，Ctrl+D 复制，Delete 删除，Ctrl+S 保存场景。文本输入与 Gizmo 的快捷键屏蔽由 `SceneEditor` 调用方负责，注册表自身只匹配键位与可用条件；当前 Delete 命令没有面板焦点限制。F1 / F2 / F3 / F11 等应用级快捷键仍在 `MinimalApp`，命令注册表尚未覆盖全部 UI 动作。

这让菜单、按钮和快捷键不再各自维护一份判断。后续的「命令面板 / 插件」可以直接遍历注册表，而不必改动面板代码。

## 多选

- Outliner：`Ctrl` / `Shift` 点击切换成员；视口：`Ctrl` / `Shift` 点击同样切换。Shift 当前不做连续范围选择。
- `selectedActor()` 始终是主 Actor（Gizmo 枢轴、Details 目标）；`selection()` 是完整集合。
- 批量删除一次记录、一次撤销；整组 Gizmo 以主 Actor 为枢轴，对其他成员应用同一世界增量（旋转 / 缩放时同时更新位置）。

## 边界（未实现）

1. 多选下的批量复制目前是「每个 Actor 一条历史」，尚未合并成一条。
2. 材质参数草稿与 Shader 草稿未入栈；它们仍走 Apply / Save 两段式提交。Actor 名称、可见性和 Mesh / Material 引用修改也未入栈。
3. 没有命名快照、跨会话持久化或撤销历史面板。
4. 默认上限 64 步，可由 `setMaxSteps()` 调整，暂不暴露到设置界面。
5. 批量删除撤销会恢复全部实例与顺序，但不会恢复原来的完整多选集合。

## 许可证边界

- Godot Engine 使用 MIT 许可证；本轮只阅读源码，未复制代码、注释或资源。
- UE 源码受 Epic 许可约束，**未复制任何代码**；`FScopedTransaction` 只作为「作用域内聚合为一条记录」的设计参考。
- 已落地的 `UndoStack` 与 `EditorCommandRegistry` 为本项目自行实现，不含上游代码。

参考：

- [Godot UndoRedo](https://github.com/godotengine/godot/blob/master/core/object/undo_redo.h)
- [UE FScopedTransaction（需授权）](https://github.com/EpicGames/UnrealEngine/blob/release/Engine/Source/Editor/UnrealEd/Public/ScopedTransaction.h)
