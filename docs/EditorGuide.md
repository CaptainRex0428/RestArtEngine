# 基础场景与材质编辑器

在 Rider 打开根目录 `RestArtEngine.sln`，编译并运行 `RestArtEngine`（输出文件名为 `RestArtEngine.exe`）。无参数启动会先进入 Project Manager；选择工程后再进入编辑器。本页解释当前基础编辑器的数据关系、保存方式，以及引擎内编辑 HLSL 后 Apply 的工作流。界面使用 Dear ImGui；功能代码位于 `src/Editor/`。

工程管理器列出 `Projects/` 下的所有示例工程（`Sandbox`、`RayTracing_Simple`）。`--project` 接受工程目录或 `.raproject` 文件，例如 `--project E:\Private\RestArtEngine\Projects\Sandbox`，或 `--project E:\Private\RestArtEngine\Projects\Sandbox\Sandbox.raproject`，可绕过工程管理器直接打开。工程描述决定引擎目录、Content、可编辑 Shader 与默认场景的位置，截图目录独立设置。

引擎目录根部的 `Engine.raeengine` 保存引擎版本和 hash；工程描述中的 `engineVersion` / `engineHash` 绑定到该身份。工程管理器打开工程时先做兼容检查：匹配时直接进入编辑器；版本或 hash 不匹配时可更新绑定、切换到匹配的引擎，或者关闭；旧的 v1 工程没有绑定，首次打开后可升级保存。

以下提到的 `Content/` 和材质 `Shaders/` 都是**当前工程内的目录**。默认 Sandbox 分别对应引擎仓库内的 `Projects/Sandbox/Content/` 和 `Projects/Sandbox/Shaders/`。引擎根目录只保留公共 Shader 管线与默认材质模板，编辑器不会将工程材质或导入网格保存到引擎根目录。

如果在 `.raproject` 中修改了 `contentRoot` 或 `shaderRoot`，实际资产与 Shader 路径随这些配置解析。当前 Content 面板提供网格列表、材质编辑与 Shader 编辑页；尚未实现类似 UE Content Browser 的任意目录浏览、缩略图或资产移动重命名工具。阅读较长源码时可拖动分隔条扩大 Content & Editors，或将该面板拖到其他停靠区域。

## 面板布局

五个面板（World Outliner、Details、Viewport settings、Content & Editors、Scene Viewport）由 Dear ImGui 官方 Docking 系统统一排版。默认布局为左侧 Outliner / Details、右侧设置、底部 Content 和中央场景视口。

- 拖动面板标签，或悬浮窗口的标题栏，在停靠预览出现时松开鼠标。
- 放到目标区域的左、右、上、下停靠目标，可分割区域；放到中央目标，可将两个面板合并到同一个标签组。
- 拖离标签组可悬浮，再拖回停靠目标即可回嵌。悬浮范围限于主编辑器窗口，当前不支持独立操作系统窗口或跨显示器拖出。
- 拖动相邻面板间的分隔条调整占比；悬浮窗口可通过边框调整尺寸。Scene Viewport 会跟随本帧内容区域改变大小。
- 顶部 **Layout** 菜单可以聚焦指定面板；**Restore default layout** 重建默认五面板布局。

Scene Viewport 在自己的窗口中显示场景渲染纹理；拾取、相机和 Gizmo 使用该图像的当前屏幕矩形。视口窗口浮动或与其他面板合并标签后，场景图像仍跟随它显示，不占用原位置的固定矩形。

交互式编辑器通过 Dear ImGui 原生设置将 Dock 树、窗口位置和尺寸写入本机 `RestArtEngine.imgui.ini`，重启后加载上次布局；没有另一份自定义布局文件。首次从旧版自由窗口布局升级时，新 DockSpace 根节点会建立默认布局，之后保留用户的停靠安排。自动化自测不加载个人布局文件。Content & Editors 内部的 Meshes、Material Editor、Shader Editor 标签仍可拖拽重排。

## 工具栏

顶部工具栏使用两行：第一行放菜单、场景保存 / 加载与保存状态，第二行放渲染路径、检查视图、光追、截图和运行状态。高度由当前字体行高、内边距和行距计算，DockSpace 从工具栏下方开始。

## 主题切换（当前工作区）

截至 2026-10-23，工作区已接入 `UI > Theme > Black / Dark Blue`，默认 Black，Dark Blue 保留原蓝色配色。这部分尚未提交；测试代码已补充，本轮文档同步未重新构建或执行测试，实际运行效果仍待验证。

切换通过 `EditorTheme` 更新 ImGui 配色，两种主题共享间距和圆角等几何样式。选择保存到进程工作目录下的 `RestArtEngine.theme.ini`，启动时读取；文件缺失或内容无法识别时回退 Black。这是本机偏好，不属于工程资产，也不是系统用户目录中的全局设置。字体加载仍由 `ImGuiLayer` 负责，尚无字体 / DPI 配置界面。

## Rider 编译按钮

Rider 中的重新编译使用 IDE 自带的 **Rebuild Solution** 或 **Rebuild Selected Projects** 动作，不生成额外的 Shell Run Configuration。右键主工具栏选择 **Customize Toolbar**，在 **Build bar** 下添加对应动作即可；需要图标时，在同一处使用 **Edit Icon** 选择 Rider 内置图标。该按钮只执行编译，不会自动启动 `RestArtEngine.exe`。工具栏和图标属于 Rider 的全局 `customization.xml`，不由 Premake 写入项目 `.idea`。

如果点击绿色运行按钮后出现 `Process creation exception`，那是启动配置的问题，不是编译失败。重新生成 Premake 工程后，`RestArtEngine.vcxproj.user` 会为 Debug、Release 和 Dist 写入正确的 `RestArtEngine.exe` 调试路径。

## Actor 与 Mesh

1. 在 Outliner 中选择 Actor，或直接左键点击视口中的网格。Details 显示其名称、可见性、位置、旋转与缩放。
2. 用 `+ Sphere` 或 `+ Cube` 新建 Actor，或用 Duplicate 复制当前 Actor。复制得到独立 Actor ID，仍可共享原来的 Mesh / Material 资产。Plane 可以在 Content 的 Meshes 页选择后用 `Add Actor from mesh` 创建。
3. 在 Details 中更换 Mesh 与 Material 引用。使用 Outliner 的 `Delete` 按钮，或右键 Actor 后选择 `Delete Actor` 删除场景实例；对应的 Mesh 与 Material 资产保留。
4. 在 Content 的 Mesh 页面导入 OBJ；源文件复制到 `Content/Meshes/`，再将该 Mesh 分配给 Actor。
5. `Save Scene` 保存到工程描述中的 `defaultScene`，默认是 `Content/Scenes/Default.rascene`。重新加载场景会读取保存的 Actor 与资产引用；加载失败时保留当前场景并显示错误。

旋转使用度，场景 Y 轴向上。OBJ 暂时读取位置和法线，不读取贴图、UV、MTL 或骨骼；需要纹理的 DCC 材质不会自动迁移。缺少法线时由导入器生成，复杂多边形建议导入前先三角化。

## 视口变换工具

选中 Actor 后，视口显示变换手柄。用鼠标拖动轴或平面手柄即可编辑 Actor 的 Transform；Details 的数值会同步更新。工具借鉴 UE 的移动、旋转、缩放操作，使用 ImGuizmo 绘制和交互，并非给 Actor 添加新的运行时组件。

| 操作 | 效果 |
| --- | --- |
| 左键点击网格 | 选择 Actor |
| Ctrl / Shift + 点击 Outliner 或视口网格 | 切换选中集合中的成员；Shift 当前不做连续范围选择 |
| W / E / R | 切换移动 / 旋转 / 缩放工具 |
| Space | 循环切换变换工具 |
| 工具栏 World / Local | 切换移动和旋转的世界 / 局部坐标模式；缩放始终沿局部轴 |
| 工具栏 Snap | 开关步进吸附：移动 0.5 单位、旋转 15°、缩放 0.1 |
| 右键拖动视口 | 环绕观察场景 |
| Alt + 左键拖动视口 | 环绕观察场景（与右键相同） |
| 中键拖动视口 | 平移相机焦点 |
| 视口滚轮 | 调整相机距离 |
| 选中 Actor 后按 F | 相机聚焦到该 Actor |
| Delete | 删除选中 Actor；多选删除为一条撤销记录 |
| Ctrl+D | 复制选中 Actor；多选复制逐个记入撤销历史 |
| Ctrl+Z / Ctrl+Y（或 Ctrl+Shift+Z） | 撤销 / 重做已记录的编辑动作 |
| Ctrl+S | 保存当前场景 |

W / E / R 和 Space 在视口具有焦点或鼠标位于视口时生效。Delete 等注册命令由编辑器统一派发；当前 Delete 的可用条件是存在主选中 Actor 且未拖动 Gizmo，没有额外的面板焦点限制。文本输入或 Gizmo 拖动期间不派发注册命令的快捷键；编辑材质或 Shader 文本时，W / E / R 和 Space 也保留文本输入用途。删除后场景会标记为未保存，点击 `Save Scene` 才写入当前工程的场景文件。

多选时 Details 显示主 Actor，Gizmo 以主 Actor 为枢轴对整组应用变换；一次拖拽记为一条撤销记录。Details 的位置、旋转、缩放编辑在控件结束编辑时记录，同一 Actor 的同一字段在 700ms 内连续提交可合并。新增、复制、删除也支持撤销；撤销批量删除会恢复所有实例和顺序，但当前只选中其中一个恢复的 Actor。

历史默认最多 64 步，不跨会话保存；材质 / Shader 草稿及 Actor 名称、可见性、Mesh / Material 引用修改尚未纳入历史。Actor 仍持有一个 StaticMeshComponent，没有父子组件层级。事务实现及边界见 [UndoTransactions.md](UndoTransactions.md)。

## Material 参数编辑

Ground 默认启用 `Grid`：世界 XZ 平面每 1 个单位一格，每 5 个单位加粗。Material Editor 的 `Grid` 复选框随材质 Apply / Save 保存，可配合 Base Color 调整底色；前向、延迟、Unlit 与解析反射都使用相同网格材质。

Content 中的 Material Editor 可以创建和选择材质，调整 Base Color、Roughness、Metallic。界面维护参数草稿；`Apply material` 把草稿提交给运行中的资产，让引用它的 Actor 立即更新。之后使用 `Save material assets` 将已应用的材质参数写入 `Content/Materials/{id}.ramat`。草稿尚未 Apply 时该保存按钮禁用。

`Save material assets` 除了写入工程的独立 `.ramat` 文件，也保存当前场景快照，使下次启动恢复材质外观。`Save Scene` 保存当前 Actor 与已应用材质的快照，不会自动应用仍在编辑的参数草稿。`Save All` 会先应用草稿，再保存材质、Shader 与场景；Shader 编译失败时显示错误并留在编辑器中。

程序启动与 `Load Scene` 以工程场景快照为准。需要读入工程 `Content/Materials/` 下的独立材质文件时，使用 `Reload material assets`；这会更新引用这些材质的 Actor。加载失败时保留当前资产并显示错误。

多个 Actor 可以引用同一 Material。因此，在一个材质上 Apply，会同步更新所有引用它的物体。要给某个 Actor 独立外观，先创建新材质，再通过 Details 将它分配给该 Actor。

这是参数式材质编辑器，使用体验接近编辑材质实例的参数。当前没有 UE 式节点连线、节点到 HLSL 生成、多材质槽或独立的每材质 Shader 图。

## Shader 编辑与 Apply

Shader 页面编辑**当前工程**的 `Shaders/Materials/Standard.hlsli`，默认完整路径是 `Projects/Sandbox/Shaders/Materials/Standard.hlsli`。这是当前工程所有标准材质共享的 HLSL 源码。材质参数资产保存每个材质的数值，Shader 决定这些数值如何参与表面计算。修改共享 Shader 会影响该工程中使用它的所有 Actor，不会修改引擎默认模板或其他工程。

当前可编辑入口为：

```hlsl
Surface EvaluateMaterial(float3 worldPosition, float3 normal, Surface material)
```

`Surface` 包含 `float3 albedo`、`float roughness`、`float metallic`。例如，可以在函数返回前插入以下代码，Apply 后观察以世界高度变化的颜色条纹：

```hlsl
float stripe = 0.5 + 0.5 * sin(worldPosition.y * 10.0);
material.albedo = lerp(material.albedo, float3(0.05, 0.25, 0.8), stripe);
```

保留函数末尾的 `return material;`。这是共享表面函数，修改后同时影响前向和延迟路径。

1. 先保存希望保留的场景和材质参数。
2. 在 Shader 文本编辑区修改源码。
3. 点击 `Apply / Compile`，在引擎进程内调用 Shader 编译器并更新预览，无需重启引擎或重新编译 C++。
4. 查看编译诊断。若有语法或接口错误，修正后再次 Apply。
5. 确认预览符合预期后 `Save shader`，将源码写入当前工程的 `Shaders/Materials/Standard.hlsli`。有未应用草稿时该按钮禁用。

Apply 与 Save 的区别：

| 操作 | 运行中预览 | 磁盘文件 |
| --- | --- | --- |
| 编辑文本草稿 | 不变 | 不变 |
| Shader Apply 成功 | 切换到新编译的 Shader | 不变 |
| Shader Apply 失败 | 保留上一次可用 Shader，显示错误 | 不变 |
| Save shader | 不变，仅已应用且没有新草稿时可用 | 写入最近成功 Apply 的源码 |
| Reload from disk | 确认后读入磁盘源码并重新编译；失败时保留运行版 | 不变 |
| Revert draft | 不变；文本恢复为最近成功 Apply 的版本 | 不变 |

编译器重新生成前向与延迟等所需入口。所有入口和 GPU Shader 创建成功后才替换现有管线；单个入口失败不会留下新旧 Shader 混用的半更新状态。失败草稿仍可继续修改，不会因为编译错误而黑屏或关闭程序。

关闭窗口时如有未保存的场景、材质或 Shader 修改，会提供 `Save All & Exit`、`Discard & Exit` 和 Cancel。Shader 编译失败时不能通过保存退出，需修正草稿或明确丢弃修改。

**引擎目录**的 `Shaders/Private/Scene.hlsl` 提供当前渲染管线和工程材质入口之间的接口；引擎的 `Shaders/Materials/Standard.hlsli` 是默认模板，编辑器 Save 不覆盖它。编辑工程的 `Standard.hlsli` 时保留入口函数签名与结构；想改变顶点布局、常量缓冲布局或渲染入口，需要同步修改渲染器 C++。Apply 会通过反射检查绑定、常量布局与输入输出接口，不兼容的修改会被拒绝并保留原运行版本。

Apply 编译当前在内存里的编辑内容，外部编辑器保存的内容需要在引擎内 Reload 才能更新。尚未实现后台文件监视、异步 Shader 编译任务或自动重编译。不要通过重建 C++ 项目来代替 Shader 页面中的 Apply。

## 预览与验证

F11 进入视口全屏：主窗口无边框覆盖所在显示器，隐藏编辑面板。再次 F11、Esc 或 F1 恢复之前的窗口与 UI 显示状态；如果进入全屏前已隐藏 UI，退出后仍保持隐藏。Esc 在普通窗口模式仍是请求关闭。上述快捷键在文本输入期间不触发。该模式不启用独立 Dock 窗口，也不代表独占显示模式。

F2 切换 Forward / Deferred；F3 开关解析光追效果；F1 隐藏 / 显示面板；右键拖动视口旋转相机，滚轮缩放；F9 截图。Lit 显示完整光照，Unlit 显示不受光照影响的材质本色并保留曝光和色调映射，Albedo 用于检查不经过曝光和色调映射的基础颜色。Viewport 的 Shadow method 可选择 2x2 PCF、硬比较、3x3 PCF、PCSS 软阴影、两级 CSM、GTAO、Deferred Contact Shadow、解析光追或禁用阴影；PCSS 复用现有深度图并执行 blocker search 与百分比渐进滤波，CSM 使用两个方向光级联切片改善近景和远景的深度分配，GTAO 与接触阴影使用延迟 G-buffer，前向路径回退到普通深度阴影。

解析光追同时追踪内置 Sphere / Plane / Cube 与通用 MeshAsset。前 32 个内置对象使用精确解析求交，超出的内置对象、OBJ 和未来网格类型通过结构化三角形缓冲参与环境反射、相互反射和阴影。通用路径不设置引擎层三角形预算，实际可用范围取决于 D3D11 资源创建和显存。它仍是 GPU 解析求交，不是 DXR。

渲染自测和编辑器自测需要 Windows / DX11 设备，分别验证图像与 Shader Apply / 场景编辑效果。

完整目录职责、UE 概念对照及当前能力边界见 [Architecture.md](Architecture.md)。

