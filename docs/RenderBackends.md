# 主渲染后端选择

引擎提供三种实际执行方式：严格 D3D11、D3D11 光栅＋D3D12/DXR 混合模式，以及**实验性的独立 D3D12 光栅后端**。Forward／Deferred 的场景绘制通过共享 RHI 执行；原生 D3D12 成功启动时不创建 D3D11 device，也不依赖 D3D11On12 或旧 DXR bridge。实验后端仍存在下文列出的功能缺口，Auto 暂不优先选择它。

## 工程配置与界面

Project → Project Settings → Rendering 中配置主后端及回退。保存后在下次启动生效，不在当前编辑会话热切设备；受当前后端支持的光追、GI 等普通 Feature 请求实时修改。回退仅在选择原生 D3D12 时可以编辑，其他后端显示为未勾选并禁用。

窗口底部区分“保存并重启”“已保存，仍需重启”以及“保存撤回以取消重启”。只改变即时光照参数不要求重启；不适用后端的回退偏好不制造重启提示。提示比较本次实际启动请求而非当前设备名，因此启动回退或命令行覆盖不会被误判为已经切换成功；使用工程配置启动时须移除对应命令行覆盖。窗口不会自动重启或关闭程序。

在 `.raproject` 的 `rendering` 对象中保存：

```json
"rendering": {
  "backendPreference": "d3d11",
  "allowFallbackToD3D11": true,
  "hardwareRayTracing": true
}
```

| backendPreference | 当前行为 |
| --- | --- |
| `d3d11Dxr` | 正式混合模式：D3D11 主渲染，D3D12/DXR 执行请求的硬件光追；缺少此字段的工程保持此行为 |
| `auto` | 选择已验收的严格 D3D11；不尝试实验性的原生 D3D12，不初始化 DXR bridge |
| `d3d11` | 严格 D3D11，不创建 D3D12 device 或 DXR bridge |
| `d3d12` | 真正尝试独立 D3D12 设备与目标初始化；成功后使用原生 D3D12，只有启动失败且允许回退时才转用严格 D3D11 |

Auto 当前直接选择 11，属于初始选择，因此回退开关关闭也可启动。`allowFallbackToD3D11` 必须是布尔值，后端字段接受上述字符串，以及混合模式的兼容别名 `legacyHybrid`（保存时统一为 `d3d11Dxr`）；非法设置报告错误。保存只写请求值，不以实际后端覆盖它们，并保留未知工程字段。

此回退开关只控制原生 D3D12 device／窗口目标初始化失败时转用 D3D11。系统或设备不支持、原生设备／目标创建失败时，先清理失败实例，再按策略选择严格 D3D11；禁止回退时直接报告错误。Shader 编译、资产及普通渲染错误继续报告，不能因换 API 而被吞掉。混合模式已经以 D3D11 为主后端，DXR 不可用时继续光栅属于 Feature 降级，具体原因另行报告。

格式仍允许严格 11 工程保存 `hardwareRayTracing=true` 及 GI 配置；仅打开工程或设备能力不足不清空这些请求，界面说明其未激活。用户在设置中主动选择 Auto、严格 11 或当前原生 12 后端时，联动取消该版本不支持的硬件光追、独立反射／阴影和 GI 算法请求；相关控件禁用且不显示为已激活。切回混合模式后可重新配置，启动模式变化仍需保存重启。解析式 D3D11 光追不等于 DXR，不受“禁止创建 D3D12”限制。具体能力要求见 [HardwareRayTracing.md](HardwareRayTracing.md)。

混合模式中，显式关闭硬件光追同时取消其独立反射／阴影和 GI；重新开启选择默认反射／阴影。选择 Path Tracing 自动开启硬件光追并取消竞争的独立反射；Realtime GI 再取消独立光追阴影。退出 GI 不自动恢复刚取消的效果；GI 质量、贡献参数保留。硬件光追开启时取消本会话的解析式反射，避免同时勾选竞争路径。这些是显式编辑规则，不是设备探测或文件加载时的规范化。

工程配置页显示本次启动的请求、实际主后端和选择／回退原因。命令行覆盖和界面草稿不会改变本次已创建的设备；保存后端草稿也不会立即开关 DXR bridge。

## 原生 D3D12 首阶段范围

下表描述已接入的执行路径，不代表每种编辑器交互和设备组合都已完成验证。

| 功能 | 严格 D3D11／混合模式 | 原生 D3D12 |
| --- | --- | --- |
| 窗口、resize、加载图片与 ImGui 编辑器 | D3D11 呈现 | 独立 D3D12 呈现，加载与编辑窗口复用设备并重建各自 swapchain |
| Forward／Deferred、Mesh／材质、相机与灯光 | 共享场景与 Shader 语义 | 通过同一 RHI 绘制，支持颜色／深度／G-buffer 和网格材质管线 |
| 光栅阴影、GTAO、Contact Shadows | 按管线依赖执行 | 共享实现；GTAO／Contact Shadows 仍要求 Deferred |
| 工具预览、缩略图与视口图像 | D3D11 离屏纹理 | 原生 D3D12 离屏纹理、UI 采样与像素回读 |
| 用户截图／F9 | 保存当前 backbuffer | 原生 D3D12 backbuffer 回读；与 Frame Inspector 资源快照不同 |
| 解析／软件射线效果 | D3D11 Shader 路径可用 | 尚未迁移，保留请求并报告不可用 |
| 硬件光追与渐进／实时 GI | 仅混合模式可使用 D3D12/DXR | 原生 DXR 尚未接入，不能使用旧混合 bridge 冒充 |
| 景深与选中描边 | 现有 D3D11 路径 | 尚未迁移，保留请求并报告不可用 |
| Frame Inspector 资源捕获／冻结纹理 | 终帧或选定 Pass 边界，实际 RHI Draw/常量状态、raw 像素与离线包 | 原生 D3D12 资源复制及分层阴影快照，按需回读/显示，复用 `.raframe` 离线包；不依赖 D3D11 纹理 |
| GPU Pass 计时 | D3D11 timestamp/disjoint，异步关联原帧 | 主队列 timestamp query heap、频率与提交 fence，完成后回读；不是 CPU submit 耗时 |
| GPU primitive 统计查询 | D3D11 主几何查询，范围见诊断说明 | 尚未迁移；拓扑数量和 RHI 提交统计不冒充 GPU 裁剪结果 |
| CPU Trace / Runtime 调试信息 | 线程时间线、Scope 聚合、结构化消息、后端/图形 Debug 消息和内存分类 | 共享 CPU 工具及 D3D12 InfoQueue；非 CPU 断点、Shader 单步或全分配分析 |

不支持的 Feature 不会改写工程中的请求。选择 D3D12 本身不要求开启光追；需要目前的 DXR／GI 功能时使用混合模式。原生 D3D12 首阶段使用同步 fence 提交与资源退役，以生命周期正确性为先；尚未提供异步多帧并行优化，不能据 API 名称推断性能更快。

Inspector 和 GPU 计时属于已接入的诊断契约，具体后端仍须按[诊断验收](Diagnostics.md#验收要求)取得实际结果。GPU Pass 样本只覆盖主场景队列，不自动包含 ImGui 副窗口、Present 或独立混合 DXR 队列。CPU 时间、进程 working set/private memory、逻辑 GPU 缓存估算和驱动驻留是不同数据，不能混标为同一性能或显存读数。

Frame Inspector 的 `.raframe` 包包含原始资源载荷、Draw/绑定/常量/Shader/Trace 描述，可在另一渲染会话打开；旧 metadata JSON 不含图像。选定 Pass 的 Before 仅保留本帧此前初始化的资源，资源具有自己的边界/层身份；选择 Draw 只检查记录状态，不重放该事件。raw 资源预算最高 256 MiB，超预算或包损坏等失败保留上一次有效捕获；详细规则见 [Diagnostics](Diagnostics.md#帧捕获与生命周期)。这些能力不意味着已完成外部 RenderDoc 抓帧验收。

## 命令行覆盖

```powershell
RestArtEngine.exe --project Projects/RayTracing_Simple --backend d3d11
RestArtEngine.exe --project Projects/RayTracing_Simple --backend d3d12 --allow-backend-fallback
RestArtEngine.exe --project Projects/RayTracing_Simple --backend d3d12 --no-backend-fallback
RestArtEngine.exe --project Projects/RayTracing_Simple --backend d3d11Dxr --hardware-rt
```

`--backend` 接受 `auto`／`d3d11`／`d3d12`／`d3d11Dxr`，另接受 `dx11`／`dx12`／`legacyHybrid` 别名；覆盖仅作用于本次启动，不写回 manifest。回退开关同样仅覆盖本次启动，未指定时读取工程配置。原有裸参数 `auto`／`dx11` 也按对应启动策略执行。

启动输出主后端请求、实际选择、是否回退、是否尝试初始化 D3D12 及是否创建 D3D11 device；发生回退时保留并输出真实 D3D12 初始化错误。光追状态和实际 dispatch 次数另行报告。Project Manager 默认使用严格 D3D11；显式 `--backend` 同样作用于 Project Manager，不为原生 D3D12 启动暗建 D3D11。打开工程后的编辑器沿用本次命令行覆盖，否则读取工程配置。

## 实现边界

`BackendSelection.h` 为无图形 API 依赖的启动策略；实际设备由 SceneRenderer 初始化、清理并报告就绪。选择 D3D12 且禁止回退仍会真正尝试原生设备，不能在 CPU 策略层提前伪造失败或成功。设备初始化计划和实际可用状态分别记录。

`RenderBackend.h` 的生命周期类仍是独立占位接口，实际编辑器不通过它渲染；其中 Auto 枚举选择不能当作原生 D3D12 可用证据。启动选择器本身不实现资源／命令；实际 RHI 的范围见下文。运行时热切后端与设备丢失后的自动恢复尚未实现，启动回退不代表支持这两项能力。

构建后可运行 `scripts/Test-BackendStartup.ps1 -Executable <本次构建的 RestArtEngine.exe>`，在隔离示例副本中验证严格 D3D11／Auto、原生 D3D12 开关回退策略、混合模式和非法参数。原生案例要求实际选择 D3D12、尝试原生初始化、没有 D3D11 device，并产生 GPU 截图；不支持 D3D12 的机器不能把回退结果算作原生通过。严格 D3D11 与 Auto 的光栅截图逐字节比较；原生 D3D12 图像对照需按具体场景与允许误差另行检查。脚本中的混合案例关闭硬件执行；真实 DXR 另用 `--hardware-rt-self-test --backend d3d11Dxr` 验证。故障回退验证须使用明确的设备失败场景，不能由“允许回退但设备成功”推断失败路径已通过。

## D3D11 能否做光追

光线追踪算法可以由普通 GPU Shader 计算，因此 D3D11 可以实现自行编写的射线求交与遍历。本引擎现有解析／三角形光追运行在 D3D11 像素 Shader 中；D3D11 的 Compute Shader 也可用于通用计算，但不等于本引擎已经以 Compute 实现此路径。[微软 Compute Shader 说明](https://learn.microsoft.com/en-us/windows/win32/direct3d11/direct3d-11-advanced-stages-compute-shader)。

D3D11 没有 DXR 接口；DXR 是 D3D12 的光追 API。我们混合模式中的硬件光追由 D3D12 RayQuery 与加速结构执行，D3D11 消费共享结果。严格 D3D11 不使用这条硬件通道；原生 D3D12 当前只接通光栅，后续可在同一原生设备上接入可选 DXR。[微软 D3D12 Raytracing 文档](https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-12-raytracing)。

## 实际 RHI 执行范围

`include/RestArtEngine/Renderer/Rhi.h` 是当前资源与绘制契约；`src/Renderer/D3D11/D3D11Rhi.*` 与 `src/Renderer/D3D12/D3D12Rhi.*` 分别实现两个后端。公共接口不包含平台类型，资源用有类型的 RAII 句柄持有，后端检查设备归属、用途、附件格式和绑定范围。平台互操作仅用于窗口、UI、回读及尚未迁移的特性，不把 native context 暴露给共享场景 Pass。

已接入的主场景路径：

- 网格顶点／索引／UV、常量缓冲、场景颜色／深度／G-buffer／阴影数组的创建与上传。
- Shader 模块和图形管线：顶点布局、剔除、镜像绕序、深度测试／写入、颜色与深度格式；热编译在候选阶段完成支持的材质管线创建，失败保留上一代。
- 阴影每级深度 Pass，Forward 背景与几何、G-buffer 与 Deferred 光照。Pass 持有实际附件和采样句柄，执行 Load/Clear/Store、冲突校验和解绑；Forward 几何以 Load 保留背景及深度。
- 命令层实际绑定、上传和 Draw，并提供累计执行统计 `SceneRenderer::rhiStatistics()`。统计只覆盖通过 RHI 的工作，不能当作整帧所有 API 调用数量。

每次常量上传必须让此前提交的 Draw 保留其数据，直到 GPU 消费完毕。D3D11 后端使用有序 `UpdateSubresource`；D3D12 为更新保留上传分配和资源引用，提交后等待 fence 完成再回收，不能重复覆盖尚未消费的 GPU 地址。Pass 结束释放附件和绘制绑定；新 Pass 重新绑定，不依赖 native 调用后的状态缓存。`DontCare` 允许后端保留或丢弃，不能假设初始化或内容保留。

窗口／设备、ImGui、加载图片、截图及 Inspector 资源复制/读回按实际后端执行；D3D11 的 Outline、DOF、解析光追结构化缓冲及 DXR 共享桥仍保留独立实现，未作为原生 D3D12 已支持能力。旧 `IRenderBackend` 生命周期骨架不参与上述实际 RHI 执行。

D3D12 后端根据实际附件／采样／复制／UI／呈现用途记录资源状态转换与同步。共享 Pass 声明已绑定真实资源，但尚未统一覆盖所有上传、copy、UI 和 present 的图依赖；没有完整 Render Graph 编译、瞬态别名或多队列调度。后端执行资源屏障不能等同于已经实现通用 Render Graph。

验证入口：`RhiTests` 使用 D3D11 WARP 实际绘制并 staging 回读；`D3D12RhiTests` 仅链接原生 D3D12 后端，离屏回读验证逐 Draw 常量版本、单层阴影数组采样、Load/Clear、无效绑定和无 present 的 UI 退役。后者需要可用 D3D12 设备（硬件或 WARP），设备不可用直接失败，不将跳过算作通过。

`--native-d3d12-self-test --project <隔离示例路径> --capture-dir <输出目录>` 强制原生 D3D12 且禁止回退，验证场景/预览像素、参数变化、热编译失败保留、resize、GC、独立 UI 窗口与主窗最小化提交，生成 JSON 与 BMP。该专项不会把未迁移功能标为通过，也没有副窗口 GUI 像素验收。旧 `--editor-self-test`、相机/材质等专项仍依赖 D3D11 原生读回，不能用于原生12。D3D11 场景回归使用 `scripts/Validate-Render.ps1`，启动策略使用后端矩阵，见 [渲染开发规范](RenderingDevelopment.md)；各后端结果不能相互代替。

`--inspector-self-test --backend <d3d11|d3d12> --project <隔离示例路径> --capture-dir <输出目录>` 面向所选后端的详细捕获、离线载荷、Pass 前后/阴影层、冻结与失败保留、实际 GPU 计时。它使用公共读回接口，不将 D3D12 descriptor 强转为 D3D11 SRV。该专项的实际执行结果、原生 UI 视觉验收和 RenderDoc 外部抓帧分别判断。

D3D11 资源附有调试名称，RHI Pass 在可用的 `ID3DUserDefinedAnnotation` 接口上产生配对事件。无调试接口或无效标签不会影响渲染。事件供外部图形调试工具使用，不代表已实现命令回放，也不代替各模式的 RenderDoc 实测。
