# 编辑事务研究：Godot UndoRedo 与 UE FScopedTransaction

本文记录对 Godot `UndoRedo` 和 Unreal Engine 事务系统的源码研究，以及 RestArtEngine 已落地的编辑撤销 / 重做实现。结论先行：**借鉴两者的模型，不复制任何代码**。

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
| UE | [`Engine/Source/Editor/UnrealEd/Public/ScopedTransaction.h`](https://github.com/EpicGames/UnrealEngine) | `FScopedTransaction` 构造开始事务、析构结束、`Cancel()`、`IsOutstanding()` |

UE 仓库需要 Epic 账号授权；本次通过公开镜像阅读了 `ScopedTransaction.h`（UE 4.x），只取作用域语义，未复制代码。

## 采纳与不采纳

| 上游机制 | 是否采纳 | 本仓库的做法 |
| --- | --- | --- |
| 声明 do / undo 两个方向 | 采纳 | `UndoStack::push(label, undo, redo)`，两个闭包都捕获**值拷贝** |
| 游标 + 提交时截断重做尾巴 | 采纳 | `entries_.resize(cursor_)` 后追加，`cursor_` 指向已应用前缀 |
| 历史上限 | 采纳 | `setMaxSteps()`，默认 64，超出丢弃最旧记录 |
| 作用域聚合成一条 | 采纳 | Gizmo 拖拽期间只记录「拖拽前」快照，松手时才 `push` |
| 合并模式（MERGE_*） | 未采纳 | 连续同类操作各占一条；拖拽本身已聚合，暂不需要时间窗合并 |
| `action_level` 嵌套 | 未采纳 | 当前没有宏动作或复合命令 |
| 对象引用保活 | 未采纳 | 记录保存 `scene::Actor` 的值拷贝，不持有指针 |
| 对象序列化自动捕获 | 未采纳 | 引擎无反射；改为调用方显式提供两个方向 |

## 当前实现

```text
include/RestArtEngine/Editor/UndoStack.h   历史、游标、上限（无 UI 依赖）
src/Editor/UndoStack.cpp
src/Editor/SceneEditor.cpp                 pushActorCreated / pushActorDeleted / trackTransformEdit
app/EditorSmoke.cpp                        Ctrl+Z / Ctrl+Y 的三段断言
```

覆盖范围：

| 操作 | 记录方式 |
| --- | --- |
| `+ Sphere` / `+ Cube` / `Add Actor from mesh` | 记录新 Actor 的拷贝；撤销时按 id 删除，重做时追加 |
| `Duplicate` | 同上 |
| `Delete` | 记录 Actor 拷贝与原始下标；撤销时插回原位置并恢复选中 |
| Gizmo 平移 / 旋转 / 缩放 | 拖拽开始前记住「稳定态」变换，松手时若发生变化则提交一条 `Transform <name>` |

交互入口：顶部 `Edit` 菜单（显示 Undo/Redo 的动作名与可用状态），快捷键 `Ctrl+Z` 撤销、`Ctrl+Y` 或 `Ctrl+Shift+Z` 重做。文本输入获得焦点时快捷键不生效（保留输入框自己的撤销）；Gizmo 使用中也不触发，避免打断拖拽。加载场景后清空历史，避免旧记录引用已替换的 Actor。

## 边界（未实现）

1. Details 面板的连续数值编辑（`DragFloat3`）尚未入栈，需要按控件激活 / 失焦聚合成一条。
2. 材质参数草稿与 Shader 草稿未入栈；它们目前走 Apply / Save 两段式提交。
3. 没有命名快照、跨会话持久化或撤销历史面板。
4. 上限固定 64 步，暂不暴露到设置界面。

## 许可证边界

- Godot Engine 使用 MIT 许可证；本轮只阅读源码，未复制代码、注释或资源。
- UE 源码受 Epic 许可约束，**未复制任何代码**；`FScopedTransaction` 只作为「作用域内聚合为一条记录」的设计参考。
- 已落地的 `UndoStack` 为本项目自行实现，不含上游代码。

参考：

- [Godot UndoRedo](https://github.com/godotengine/godot/blob/master/core/object/undo_redo.h)
- [UE FScopedTransaction（需授权）](https://github.com/EpicGames/UnrealEngine/blob/release/Engine/Source/Editor/UnrealEd/Public/ScopedTransaction.h)
