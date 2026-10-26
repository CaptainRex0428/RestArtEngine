---
name: restarengine-rendering
description: "Develop and review RestArtEngine rendering, hand-written shaders, passes, pipelines, and features; preserve shader hot compilation, live parameter feedback, and portable DX11/DX12/Vulkan/OpenGL boundaries. Use for this engine's rendering work and cross-platform rendering design."
---

# RestArtEngine Rendering

本技能用于 RestArtEngine 的核心渲染开发与审查。四条要求及验收定义以 [docs/RenderingDevelopment.md](../../../docs/RenderingDevelopment.md) 为准。Windows、Linux、macOS 均纳入首轮架构设计，图形后端范围包括 DX11、DX12、Vulkan、OpenGL；设计目标不代表已经实现。

## 先确认现状

读取仓库 `AGENTS.md`（若存在）、[docs/Architecture.md](../../../docs/Architecture.md) 和 [docs/RenderingDevelopment.md](../../../docs/RenderingDevelopment.md)，检查工作区状态，再读与任务有关的源码。现有结构以 `README.md` 与 `Architecture.md` 为准；渲染开发文档将要求、现状缺口和设计草案分开记录，不把草案当作现成接口。

按改动范围选择入口：

- GPU 管线与数据契约：`include/RestArtEngine/Renderer/SceneRenderer.h`、`src/Renderer/SceneRenderer.cpp`、`Shaders/Private/Scene.hlsl`。
- 材质与编辑预览：`Shaders/Materials/Standard.hlsli`、当前工程的材质 Shader、`src/Editor/SceneEditor.cpp`。
- 后端与构建：`include/RestArtEngine/Renderer/RenderBackend.h`、`src/Renderer/RenderBackend.cpp`、`CMakeLists.txt`、`RestArtEngine.lua`。
- 可观察验证：`app/EditorSmoke.cpp`、`scripts/Validate-Render.ps1`。

## 开发流程

1. 追踪“源码或界面参数 → CPU 状态 → GPU 数据／Shader／Pass → 实际场景视口”。确定模块归属、资源生命周期、后端能力和可见验收，再修改代码。
2. 按所属模块放置可复用功能。手写 Shader、Pass、管线或 Feature 时，明确源码入口、参数契约、资源读写、执行顺序和开关。小改动沿用现有结构；需要扩展时逐步引入文档中的职责边界，不创建空框架，不向共享场景、资产或特性代码增加平台依赖。
3. 热编译使用候选构建、完整校验、成功后整体替换；编译、反射或 GPU 管线创建失败时保留上一版。普通数值变化应在下一次可呈现的场景帧生效，无需保存、重启或源码重编译。材质通过只读草稿覆盖实时预览，预览、Apply、Save、Discard 及切换时 Cancel 的语义遵守渲染开发文档；不将预览写入已提交资产。
4. 从用户意图、构建可用性、设备能力及特性依赖计算实际启用状态。差异放在后端或特性实现边界内；不支持的模式给出原因，按定义禁用或降级。禁用后不能遗留无效资源依赖；切后端不能丢失用户请求值。
5. 依据改动运行相关验证并检查真实视口结果。Shader 改动验证成功改画面、失败保留画面及受影响的 include；参数改动验证编辑过程中的反馈和预览／提交／保存语义；后端或特性开关验证资源重建、不支持项、失败恢复及逻辑设置保留。复用下方现有检查，仅针对有意义的新增缺口补充覆盖。
6. 交付说明改动、模块归属、运行证据和未验证范围。Windows/DX11 结果不能外推为其他后端或平台已通过。涉及尚未确定的 Shader 工具链、macOS 路线、最低能力等级时，更新设计选项并与用户讨论；继续完成不依赖该选择的已授权工作。

## 验证入口

可执行渲染改动按架构约定及影响范围选择检查；当前 CMake Debug 入口：

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-debug
$renderExe = Join-Path (Get-Location) 'build/cmake-vs2022/Debug/RestArtEngine.exe'
& $renderExe --editor-self-test --capture-dir artifacts/rendering-editor
.\scripts\Validate-Render.ps1 -Executable $renderExe -CaptureDirectory artifacts/rendering-validation
```

显式指定本次构建的可执行文件；验证脚本默认寻找 Premake 输出。只通过构建、修改 revision 或 CPU 数值，都不足以证明画面正确。仅修改文档或技能时检查格式、引用和约束一致性，不宣称进行了 GPU 验证。

## 渲染 agent

项目角色定义于 [.codex/agents/restarengine_rendering.toml](../../../.codex/agents/restarengine_rendering.toml)。主 agent 分派有明确文件归属、预期画面与验证范围的实现或审查任务。角色不指定模型或 provider。若当前客户端无法选择项目角色，普通子 agent 显式读取本技能后执行，并如实说明采用了该方式。
