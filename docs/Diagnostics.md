# 引擎诊断工具

RestArtEngine 的诊断设计覆盖性能、帧执行与画面数据三个方向。它们共享帧身份和渲染语义，界面统一从 `Debug` 进入。

首版已接入 CPU Trace、DX11 GPU Pass 计时、Frame Inspector 和下述实时视口模式。本文另行标记后续设计；当前真实场景后端为 Windows / DX11，DX12、Vulkan、OpenGL 尚未接入场景渲染。

## 三种工具的职责

| 工具 | 回答的问题 | 首版行为 |
| --- | --- | --- |
| Profiler / Trace | 哪一段工作耗时，卡顿发生在哪一帧？ | CPU 嵌套事件、帧耗时、Present 等待、JSON Trace 导出；DX11 异步 GPU Pass 耗时。 |
| Frame Inspector / 帧检视器 | 这一帧执行了哪些 Pass / Draw，生成了什么资源？ | 捕获下一主场景帧，保留事件和资源副本，查看通道、原始像素及元数据。 |
| Viewport Debug | 某个表面、光照或缓冲为什么呈现为当前结果？ | 扩展现有实时显示模式，检查材质有效值、法线、位置和深度。 |

典型路径：从帧耗时发现异常 → 捕获问题帧 → 查看 Pass / 资源 → 用实时视口模式定位表面数据。录制和帧捕获分别开启；诊断关闭后不应持续复制完整渲染资源。

`Frame Inspector` 是引擎自己的帧检查器名称。首版捕获引擎已知的绘制事件和资源，不包含任意图形 API 拦截或命令重放。RenderDoc 的[事件浏览器](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/event_browser.rst)、[管线检查](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/pipeline_state.rst)与[纹理查看器](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/texture_viewer.rst)用于参考调试任务划分；逐事件重建输出需要额外的快照或重放实现。

## 性能数据的含义

命令行可用 `--trace-output <文件.json>` 自动录制主应用循环并在正常退出时导出 Chrome Trace。例如 `RestArtEngine --project <工程.raproject> --no-ui --frames 60 --trace-output <输出.json>`。录制从加载完成后的主循环开始；比较稳定帧时还应排除资源首次上传／光追结构首次构建的预热帧，保持项目、分辨率、VSync 和采样设置一致。`Mesh geometry upload`、`Software ray scene rebuild`、`DXR scene rebuild` 标识缓存失效时的 CPU 工作，不代表 GPU 执行耗时。

CPU 帧耗时使用单调时钟，包含采集范围内的等待。CPU Scene Submit 衡量 CPU 提交渲染命令的时间；Present scope 可以包含 VSync 等待。它们均不能代替 GPU 执行时间。嵌套事件的耗时是包含子事件的 wall time，不能将所有行简单相加得到帧耗时。

Trace 记录事件名、分类、线程、帧及父事件，最多保留 65,536 个事件，满后统计丢弃而不覆盖旧录制。Start 清空并开始新录制，Stop 保留数据，Clear 清空事件但保留录制开关。120 帧 wall time 历史独立于录制状态。界面快照取最近 512 个已完成事件，每帧最多显示 256 行；JSON 导出全部已保留 CPU 事件，首版不包含 GPU query 结果。录制热路径不写磁盘；导出由用户显式选择位置。首版选择 Chrome Trace JSON 的基础事件，便于在外部查看；Perfetto 提供[该格式导入](https://perfetto.dev/docs/getting-started/other-formats)，其 JSON 兼容是 [best effort](https://perfetto.dev/docs/faq)，并非原生 Perfetto SDK 集成。

DX11 GPU 计时使用每帧 disjoint query 和 Pass 边界 timestamp；仅可靠且频率有效的数据转换为毫秒。[Microsoft query 定义](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query)说明了时间戳与频率/有效性关系。结果延迟读取并保留原 GPU Frame ID，`Pending`、`Unavailable`、`Disjoint` 不显示为 0 ms。[GetData](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata) 与 [DONOTFLUSH](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_async_getdata_flag)用于非阻塞检查；未就绪时留到后续帧，不自旋等待。

首版 GPU scope 以主场景 Pass 为范围，不自动包括 ImGui 平台窗口或 Present。CPU/GPU 尚无绝对时钟校准时，只关联 Frame ID 与持续时间，不伪造精确重叠时间轴。启用资源捕获、显式像素读回或高密度事件会影响性能；比较数据时记录构建类型、设备、分辨率、管线、VSync 和诊断开关。

## 视口数据契约

### 场景与视口面数

Scene 的 Overview 与 Scene Viewport 的只读统计显示两种三角面数；Profiler 的 Live renderer 也展示相同数据：

- **Scene triangles / Scene tris**：按场景 Actor 实例累加所引用 Mesh 的 `indices.size() / 3`，包含隐藏实例，共享一个 Mesh 的多个实例分别计数。未被实例引用的资产、缺失的 Mesh 不计入；材质缺失不改变这项拓扑总数。
- **After clipping (GPU)**：仅测量主视角 Forward geometry 或 GBuffer 通道的 GPU 裁剪输出，不累计阴影、描边、全屏三角形或工具预览。当前 DX11 使用独立四槽 `D3D11_QUERY_PIPELINE_STATISTICS` 查询的 `CPrimitives`，同时保留 `IAPrimitives` 输入量与原始主场景样本序列。按 [Microsoft 的字段定义](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_query_data_pipeline_statistics)，裁剪可拆分三角形，因此输出可能多于输入；这不是深度测试或遮挡之后的可见三角面数。

场景总数读取最近主场景提交的快照，GPU 结果额外异步延迟；提示与 Profiler 标明样本序列，使用 `GetData(...DONOTFLUSH)`，查询未就绪时不等待 GPU，环满时跳过当前采样。统计不依赖打开 Profiler，未就绪／不可用／视口未绘制均显示相应状态，不用 0 冒充有效结果；空场景的有效查询可以为 0。场景视口隐藏仍更新拓扑总数，resize、窗口切换和显示路径切换废弃旧查询；材质球、资源详情和场景缩略图不改变主场景统计。

完整硬件光追图像覆盖光栅输出时，裁剪统计显示 **N/A**；光追回退至光栅、以及使用光栅主可见性的 Realtime GI 路径仍能统计。Profiler 原有的 **submitted triangles (all passes)** 是全部已记录绘制通道的提交量，可能重复计算同一实例，不能替代上述两项。统计直接测量 GPU 裁剪结果，不以各 Pass 的提交量估算。

### GBuffer 与实时显示模式

当前 Deferred 的三张 GBuffer 均为 RGBA16F：

| 资源 | RGB | Alpha |
| --- | --- | --- |
| GBuffer0 | `EvaluateMaterial()` 后的 Base Color | 有效 Roughness |
| GBuffer1 | World Normal，保留有符号值 | 有效 Metallic |
| GBuffer2 | World Position | 几何有效标记 |

材质查看应使用 Shader 最终有效数据，包含工程材质 Shader 与未 Apply 的实时预览，而非只显示 CPU 资产字段。当前 Roughness 最低钳制为 0.045，Metallic 为 0–1。

当前材质模型没有独立 Specular 参数：`F0 = lerp(0.04, albedo, metallic)`。`Specular F0` 表示法向入射基础反射率；`Direct Specular` 表示直射高光贡献；环境高光和光追反射另有各自的路径。名称必须区分这些含义。

| 分组 | 模式与检查内容 | 实施边界 |
| --- | --- | --- |
| 基础 | Lit、Unlit、Base Color | 复用当前模式与材质求值。 |
| 材质 | Roughness、Metallic、Specular F0 | 首版扩展；前两者为标量灰度，F0 为 RGB 并使用显示 gamma，均不受美术曝光影响。 |
| 几何 | World Normal、World Position、Linear Depth | 法线 XYZ 映射到 0–1；位置周期色仅供结构观察，原始值从资源检查读取；深度为正向 view-space Z（`-viewZ`）除以 100，并钳制到 0–1，非到相机的径向距离。 |
| 缓冲 | GBuffer0/1/2、主深度 | 首版在冻结帧资源中检查。Forward 帧只列实际生成的资源，不读取旧 Deferred 附件。 |
| 光照 | Direct Diffuse / Specular、Ambient / Environment、Shadow Visibility、AO | 首版已提供直射 Diffuse / Specular：从实际光照计算拆出，使用曝光和显示变换；Ambient / Environment、Shadow Visibility、AO 诊断待后续接入，非独立附件时需注明 Shader 派生。 |
| 身份 | Actor / Material ID、选中遮罩 | 后续需新增或暴露对应的 GPU 输出，不以 CPU 拾取代替像素身份。 |
| 成本与错误 | Wireframe、Overdraw、复杂度、NaN / Inf、越界与无效法线 | 后续分别实现实际测量或检测；启发式成本图须注明估算，不能称为 GPU 耗时。 |
| 新材质数据 | UV、Tangent、AO 输入、贴图 mip、Motion Vector 等 | 随顶点/材质/时序管线落地；当前 Position / Normal 顶点格式不产生这些数据。 |

Surface 模式可由 Forward 和 Deferred 共用材质求值；Raw GBuffer 只适用于真正生成附件的路径。不为显示一个模式静默切换用户的渲染管线。未来能力查询应返回用户请求、实际模式和禁用/降级原因。

纹理显示与原始值读取分开：有符号法线映射到可见范围，位置/深度注明显示变换，RGB/R/G/B/A 通道选择不改动原数据。深度显示白色为 100 世界单位（当前 UI 标作 m），冻结 Depth 的原始像素则为非线性 device depth 0–1。F0 模式的 SceneColor 已经过显示 gamma；原始 F0 可由 GBuffer albedo 与 metallic 推导。首版像素检查在捕获图片中进行，显示资源名、纹素坐标和原始值；显式读回可以阻塞，不能将其延迟计为普通渲染成本或每帧跟随鼠标持续执行。

## 帧捕获与生命周期

捕获默认针对下一次有效主场景渲染，材质球、Asset Details 和场景缩略图不消费该请求。一次 `render()` 调用并不等于应用的一帧；工具预览复用场景渲染器，必须保留用途边界。

捕获应保留：

- Frame ID、管线、Shader revision、尺寸和捕获状态。
- Pass / Draw 名称、Actor / Mesh / Material ID、几何计数和明确可观察的提交状态。
- 捕获资源描述及独立副本；首版保留一帧，当前 raw 副本与缓存显示纹理合计限制 256 MiB，提供 Clear。替换候选和安全退役期间新旧副本可短暂并存，该限制不是进程或诊断功能的峰值 GPU 内存上限。

快照不是借用 Live SRV；后续渲染、工具预览、resize、GC 或 Shader 更新不能把旧捕获变成新内容。创建失败保留上一次有效捕获并报告错误。清理捕获只释放诊断数据，不修改工程资产。

首版资源是指定帧捕获点的内容。选择事件行只查看事件元数据；不能声称图像是该 Draw 刚执行后的输出。中途反复覆盖的阴影/目标若未在对应点留存，也不能从最终副本恢复历史。

首版元数据导出应明确标注仅包含事件和描述；没有图像载荷的 JSON 不能当成可离线恢复的完整帧。后续独立捕获文件需包含版本、格式、资源字节、有效着色数据/常量、源码或字节码身份、预算及兼容错误，才能提供离线资源浏览。可重放文件还需完整命令、资源初始化与更新顺序，属于下一层能力。

## 界面与工程边界

`Debug` 提供 Profiler、Frame Inspector 和捕获入口。Diagnostics 是按需打开的可关闭工具窗，复用 Black / Dark Blue 主题与原生 Docking；已有布局身份保持稳定。现有 Screenshot / F9 继续保存合成后的窗口图片，与帧资源捕获分开。

Profiler 显示当前录制状态、最近完成帧的 wall time 和有界保留的事件；Frame Inspector 持续显示 `Captured` 与捕获帧身份。冻结图像中的像素选择不操作 Live 场景，不改变 Actor 选择、Gizmo 或 Undo。诊断窗获得焦点后，场景快捷键不得穿透。未来若添加 Live 像素工具，需要在拾取/拖动前统一分配输入所有权。

布局和显示偏好属于本机设置；录制、待捕获和取像素的活动状态属于当前会话，重启默认关闭。它们不写入 `.rascene` / `.rasset`，不增加撤销历史，也不标记场景 dirty。用户导出的诊断文件不自动加入工程 Content。

## 模块与扩展

Core 提供平台无关 Trace 服务；Renderer 提供主场景计时、事件、捕获和资源检查；Editor 只消费诊断数据并管理工具界面。公共诊断结构使用自有类型，不包含 D3D11 指针。具体 query、纹理复制、GPU 生命周期留在后端实现内；当前渲染器的原生设备接口仍按既有兼容边界使用。

后续新增 Pass 需同时声明名称、输入输出、参数/Shader 身份、可选计时和可检查输出。新增诊断模式需声明数据来源、单位、范围、支持路径、显示变换及禁用原因，不将所有模式硬塞成无说明的整数分支。进一步扩展时逐步引入描述表与能力查询，不先建设没有消费者的通用框架。

后续按依赖推进：照明贡献和特性诊断 → 完整离线捕获文件 → 指定事件后的快照/受限重放 → Pixel History / Shader 调试。硬件计数器、采样式 CPU 分析、全分配追踪、设备故障诊断分别评估能力和开销，不把插桩 Trace 当成这些能力已经具备。

## 验收要求

- Trace 可解析，嵌套/线程/帧关联正确，关闭与溢出行为可观察；停止后数据保留，导出失败不清空录制。
- GPU 查询返回原 Frame ID，无数据状态明确；压力下不阻塞，释放和设备生命周期安全。
- 已知材质的 GBuffer 原始值与实时草稿一致，Roughness / Metallic / F0 实际画面正确，Forward / Deferred 往返不读取陈旧附件。
- 数据显示不意外受曝光影响；新 Shader 改动成功发布，失败保留可用画面和 revision。
- 捕获经后续渲染、preview、resize 和 Shader 更新保持冻结；失败不破坏上一快照，Clear 使捕获失效并安全退役其副本。query 池和转换 Shader 等缓存保留至渲染器 shutdown，CPU Trace 独立清理。
- 检查双主题及窄工具窗截图，取像素不触发场景操作；保留既有保存、预览和 Undo 语义。

构建通过、CPU 测试、DX11 画面验收与其他平台支持分别判断。`TraceProfilerTests` 检查平台无关的录制与导出；`--editor-self-test` 中的 Diagnostics 用例通过实际 GPU 回读检查 GBuffer、全部资源通道显示、实时材质覆盖、视口模式、冻结生命周期和异步帧计时，并检查工具窗及输入。其余验证入口参照[渲染开发要求](RenderingDevelopment.md)，测试使用隔离工程和布局。
