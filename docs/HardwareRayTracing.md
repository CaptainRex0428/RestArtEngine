# 硬件光线追踪

项目通过 `Project → Project Settings... → Rendering` 配置硬件光追，F3 打开配置窗口。`Hardware ray tracing` 是总开关；`GI method` 选择 Disabled／Path tracing，`GI mode` 独立选择 Progressive accumulation／Realtime，两个间接光贡献开关独立控制漫反射与反射。采样及历史规则见 [GlobalIllumination](GlobalIllumination.md)。参数自动更新；`Save project settings` 或 `Save All` 将请求值写入当前 `.raproject`。关闭程序时未保存配置参与现有保存提示。

配置页分别显示设备支持、请求值、实际状态及不可用原因。不支持的设备保留项目请求值，使用现有渲染路径；运行失败保留光栅画面，关闭再开启可重试。解析式反射移到同页的 `Compatibility preview`，是会话设置，不能代表硬件光追。

## 项目格式

工程版本 1／2／3 均可省略以下对象，默认关闭硬件光追及 GI，两个独立预览效果默认开启：

```json
"rendering": {
  "hardwareRayTracing": false,
  "hardwareRayTracingReflections": true,
  "hardwareRayTracingShadows": true,
  "globalIllumination": {
    "method": "disabled",
    "mode": "progressive",
    "indirectDiffuse": true,
    "indirectSpecular": true,
    "samplesPerFrame": 1,
    "targetSamples": 256,
    "maxBounces": 4
  }
}
```

硬件总开关、预览效果与 GI 贡献字段必须为布尔值。GI 算法为 `disabled`／`pathTracing`，更新模式为 `progressive`／`realtime`，采样和反弹数为范围内整数。旧 GI 对象缺少模式及贡献字段时按渐进、两项贡献均开启读取。保存只更新对应键，保留其他项目字段和未知嵌套键，经同目录临时文件完整写入后原子替换；失败保留原 manifest。保存的是用户意图，不能根据当前 GPU 自动改写。

## 渲染实现与边界

Windows 实现位于 `src/Renderer/D3D12/HardwareRayTracing.*`，在现有 DX11 设备的同一适配器上创建 DX12 设备。需要 DXR 1.1、Shader Model 6.5、DX11 共享 fence 接口和可写共享图像格式。DXC 的 `dxcompiler.dll` 与匹配的 `dxil.dll` 随程序放置；CMake 从 Windows SDK 自动复制，也可配置 `DXC_RUNTIME_DIRECTORY`。Premake 构建调用 `scripts/Copy-DxcRuntime.ps1`；CMake install 同步分发 DLL。缺失时配置页报告原因。

全部可见有效网格，包括内置图元和导入模型，转换为世界空间三角形，构建 GPU BLAS／TLAS。几何内容变化重建加速结构，材质或平滑法线变化更新各自数据。双面材质、镜像缩放和只读材质草稿沿用场景契约；阴影射线从受光点指向光源，按相反面规则筛选单面遮挡物。

`Shaders/Private/HardwareRayTracing.hlsl` 的 `CSMain` 使用 DXR `RayQuery` 和共用工程材质。关闭 GI 时输出一次反射与阴影的逐帧硬件预览；Path tracing 算法通过 `PathTracing.hlsli` 采样 Lambert／GGX 并计算多次反弹。Progressive 模式输出完整画面，Realtime 模式只输出供当前光栅管线使用的线性 HDR 间接光。主射线遵守当前相机近／远裁剪面，实时模式使用像素中心与光栅表面对齐；材质编辑器、资产详情和缩略图继续使用光栅工具预览。

硬件预览和 Progressive 完整图像通过共享 RGBA8 纹理交回 DX11，在选中描边和 UI 前复制到当前视口。Realtime 则共享 RGBA16_FLOAT 间接光纹理，由 Forward／Deferred 光照 Shader 在主表面直接光之后、曝光和显示转换之前读取；直接光、其阴影和背景保留当前光栅设置。该路径替换原近似环境间接光，不重复叠加主表面直接光。现有深度与 G-buffer 仍是光栅中间结果，最终 Scene Color 才包含组合结果。

两条 API 通过双向 fence 排序共享图像读写。CPU 在重用命令 allocator 和上传资源前等待上一份 DX12 提交；队列间使用 Signal／Wait，超时及设备移除返回诊断。内部累积与引导图像保持 FP32，实时共享输出使用 FP16 并限制有限值范围。按视口分配的图像合计 60 字节／像素，4K 约 475 MiB，另有场景及加速结构开销；不能承诺固定 FPS。

这是 Windows 上 DX11 编辑器与同适配器 DX12 DXR 1.1 通道的具体组合，尚未实现完整 DX12 场景后端切换。Vulkan、OpenGL、Linux、macOS 的硬件光追尚未实现。当前不包含透明／alpha-test 几何、蒙皮加速结构或运动重投影。

Realtime 每帧持续采样，以 `History samples` 限制历史权重，达到上限仍继续更新；场景或相机变化使历史整体重置。实时解析采用 5×5 几何／材质边缘感知空间滤波，粗糙度小于 0.12 的近镜面表面不做空间模糊。Progressive 仍按目标样本停止，当前没有该模式的降噪器。

`Ray-traced reflections` 仅用于 GI Disabled；GI 启用后采用独立 `Indirect specular` 请求。`Ray-traced shadows` 决定硬件预览／Progressive 的灯光遮挡，并与每灯 Cast shadows、Scene 的全局 Disabled 共同生效。Realtime 主直接光沿用光栅设置：首个投影方向光可用 PCF／PCSS／CSM，局部及附加方向光的直接阴影需 Analytic rays。GI 两个贡献开关只分类第一接收表面的 BRDF，次级表面保留完整光传输，因此漫反射 GI 仍可出现在镜面里。

## Shader 与数据契约

关闭 GI 的硬件预览使用共享灯光列表及程序化环境近似，不计算物体间漫反射颜色溢出，也不随等待收敛。GI 路径计算表面间反弹和粗糙反射，按更新模式使用完整画面累积或连续间接光历史。实际 DXR dispatch、开关像素差异及零验证错误仅证明通道运行，GI 画质还需通过颜色传递、收敛和实时组合验证。

| 绑定 | 内容 |
| --- | --- |
| b0 | 共用 `SceneConstants`，176 字节。 |
| b5 | `HardwareConstants`：逆 ViewProjection、输出与效果参数、样本批次／反弹数／算法、更新模式／贡献位／解析阶段／独立序列索引、兼容矩形参数与薄透镜参数，208 字节。 |
| b7 | `LightingConstants`：灯光计数／模式、Sky 聚合及 32 个光源；3104 字节，与光栅共享布局且纳入 GI 历史键。 |
| t4 | 共用 `RayTriangle`，80 字节，包含位置、几何法线、颜色与材质标志。 |
| t7 | TLAS。 |
| t8 | 每个三角形三个世界空间平滑顶点法线，float3。 |
| t10 | 可选 UV 顶点属性，两个 float4 打包 UV0–3，由同一网格的实例共享。 |
| t11 | 仅有 UV 三角形的 uint4：三个 UV 顶点索引与通道存在 mask。 |
| u0 | RGBA8 硬件输出。 |
| u1 | 非共享 RGBA32_FLOAT 线性 HDR 样本均值。 |
| u2 | 非共享 RGBA32_FLOAT 主表面法线与射线距离引导。 |
| u3 | 共享 RGBA16_FLOAT 实时间接光输出。 |
| u4 | 非共享 RGBA32_FLOAT 主表面 albedo 与粗糙度引导。 |

DX11 光照入口另使用 b6 的 16 字节 `GlobalIlluminationConstants` 和 t9 的共享间接光 SRV。实时采样完成后执行独立解析 Dispatch，使空间滤波只读取已完成的历史与引导；UAV barrier 及共享 fence 保护阶段和 API 之间的依赖。

开启硬件模式时，Shader Apply／Reload 将全部光栅阶段与硬件计算管线视为一次候选，编译、反射或管线创建失败保留上一版。关闭硬件模式时，合法像素 Shader 材质仍可使用 `ddx`／`ddy`／`fwidth`；若该源码不兼容计算 Shader，光栅更新正常生效，硬件管线被明确标为不可用，不能使用旧硬件源码渲染新材质。修复并重新 Apply 后恢复。运行时数值和材质草稿不重新编译 Shader。

## 验证入口

```powershell
RestArtEngine.exe --hardware-rt-self-test --capture-dir artifacts/hardware-rt
RestArtEngine.exe --gi-self-test --capture-dir artifacts/gi
RestArtEngine.exe --project Projects/Sandbox --path-tracing
RestArtEngine.exe --project Projects/Sandbox --hardware-rt
RestArtEngine.exe --project Projects/Sandbox --no-hardware-rt
```

命令行总开关仅覆盖本次启动值；需要持久化时从配置页保存。自测使用临时工程副本，报告实际 dispatch 数、GPU 像素变化、Shader 成功／失败恢复和配置交互；设备不满足条件时明确记录硬件执行跳过原因，不能把跳过当作实际 DXR 验证。


多材质网格按 section 的 Slot 填入有效材质，Actor override 和只读草稿同样参与。`RayTriangle.color.w` 为 UV 查询索引的位模式（零表示无 UV），不是 alpha；CPU/HLSL 必须用位转换保持一致。四套 UV 在命中点重心插值后进入 `Surface.uv0..uv3` 与 `uvMask`，工程 `EvaluateMaterial` 入口不变。UV 资源上传、反射、根签名和历史签名随整套候选一起更新；Shader 失败仍保留上次有效管线。
