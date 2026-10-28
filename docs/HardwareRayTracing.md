# 硬件光线追踪

项目通过 `Project → Project Settings... → Rendering` 配置硬件光追，F3 打开配置窗口。`Hardware ray tracing` 是总开关，反射／阴影可独立开关；`GI method` 在硬件预览与渐进式路径追踪之间选择，采样配置和历史规则见 [GlobalIllumination](GlobalIllumination.md)。参数自动更新；`Save project settings` 或 `Save All` 将请求值写入当前 `.raproject`。关闭程序时未保存配置参与现有保存提示。

配置页分别显示设备支持、请求值、实际状态及不可用原因。不支持的设备保留项目请求值，使用现有渲染路径；运行失败保留光栅画面，关闭再开启可重试。解析式反射移到同页的 `Compatibility preview`，是会话设置，不能代表硬件光追。

## 项目格式

工程版本 1／2／3 均可省略以下对象，默认关闭硬件光追，两个效果默认开启：

```json
"rendering": {
  "hardwareRayTracing": false,
  "hardwareRayTracingReflections": true,
  "hardwareRayTracingShadows": true
}
```

字段必须为布尔值。保存只更新对应键，保留其他项目字段和未知渲染键，经同目录临时文件完整写入后原子替换；失败保留原 manifest。保存的是用户意图，不能根据当前 GPU 自动改写。

## 渲染实现与边界

Windows 实现位于 `src/Renderer/D3D12/HardwareRayTracing.*`，在现有 DX11 设备的同一适配器上创建 DX12 设备。需要 DXR 1.1、Shader Model 6.5、DX11 共享 fence 接口和可写共享图像格式。DXC 的 `dxcompiler.dll` 与匹配的 `dxil.dll` 随程序放置；CMake 从 Windows SDK 自动复制，也可配置 `DXC_RUNTIME_DIRECTORY`。Premake 构建调用 `scripts/Copy-DxcRuntime.ps1`；CMake install 同步分发 DLL。缺失时配置页报告原因。

全部可见有效网格，包括内置图元和导入模型，转换为世界空间三角形，构建 GPU BLAS／TLAS。几何内容变化重建加速结构，材质或平滑法线变化更新各自数据。双面材质、镜像缩放和只读材质草稿沿用场景契约；阴影射线从受光点指向光源，按相反面规则筛选单面遮挡物。

`Shaders/Private/HardwareRayTracing.hlsl` 的 `CSMain` 通过 DXR `RayQuery` 计算主可见性。关闭 GI 时使用一次反射与阴影的逐帧预览；路径模式使用 `PathTracing.hlsli` 的多次反弹与 Lambert／GGX 采样。两者共用工程材质、曝光、显示转换和 Debug 模式，主射线遵守当前相机近／远裁剪面。材质编辑器、资产详情和缩略图继续使用光栅工具预览。

硬件图像通过共享 RGBA8 纹理及双向 fence 交回 DX11，在选中描边和 UI 前复制到当前视口。现有光栅深度与 G-buffer 继续供编辑器及资源检查使用；它们是光栅中间结果，最终 Scene Color 包含硬件输出。CPU 在重用命令 allocator 和上传资源前等待上一份 DX12 提交，队列间使用 Signal／Wait 排序；超时及设备移除返回诊断。

这是 DX11 编辑器与 DX12 光追通道的组合，尚未实现完整 DX12 场景后端切换。Vulkan、OpenGL、Linux、macOS 的硬件光追尚未实现。已支持不透明表面渐进式路径追踪，尚无降噪、透明／alpha-test 几何或蒙皮加速结构。硬件模式的阴影开关决定方向光硬阴影；Scene 面板的 PCF／PCSS／CSM／GTAO／Contact Shadow 是光栅模式设置。

## Shader 与数据契约

关闭 GI 的硬件预览沿用程序化环境漫反射及单方向反射，不计算物体间漫反射颜色溢出，也不随等待收敛。渐进式路径追踪则计算表面间反弹和粗糙反射，积累线性 HDR 样本。实际 DXR dispatch、开关像素差异及零验证错误仅证明通道运行，GI 画质还需通过颜色传递与收敛验证。

| 绑定 | 内容 |
| --- | --- |
| b0 | 共用 `SceneConstants`，144 字节。 |
| b5 | `HardwareConstants`：逆 ViewProjection、输出与效果参数、样本偏移／批次／反弹数／GI 模式，112 字节。 |
| t4 | 共用 `RayTriangle`，80 字节，包含位置、几何法线、颜色与材质标志。 |
| t7 | TLAS。 |
| t8 | 每个三角形三个世界空间平滑顶点法线，float3。 |
| u0 | RGBA8 硬件输出。 |
| u1 | 非共享 RGBA32_FLOAT 线性 HDR 样本均值。 |

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
