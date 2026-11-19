# 抗锯齿

Project Settings → Anti-aliasing 提供 Off、FXAA、TAA。切换与 TAA 当前帧权重调整在下一帧生效；Save Project Settings 将请求保存到 `.raproject`。旧工程没有该字段时默认 FXAA。低层 `SceneSettings` 默认关闭，调用者显式选择方案。

| 方案 | 用途 | 范围与限制 |
|---|---|---|
| Off | 原始像素与诊断对照 | 单采样光栅 |
| FXAA | 单帧边缘平滑 | 前向／延迟、D3D11／原生 D3D12；会影响局部锐度 |
| TAA | 亚像素几何采样与时间稳定 | 光栅相机抖动、深度重投影、历史拒绝、邻域限制；场景编辑保守重置 |

TAA 的 `temporalCurrentWeight` 范围为 0.02–1，默认 0.1。它是当前帧权重下限：启动时按已有帧数提高权重，相机运动时也提高权重以减少拖影。较大值响应更快，较小值保留更多历史。没有物体运动矢量；物体、材质和灯光编辑使历史失效，因此不应把当前实现视为完整动态场景 TAA。

请求值与实际生效方式分开：

- 实时 DXR GI 与独立硬件全图 resolve 请求 TAA 时使用 FXAA。当前 DXR 光照历史仍依赖稳定投影，不能直接接收光栅每帧抖动。
- 渐进路径追踪使用自身随机主射线和累计采样，跳过额外的 FXAA／TAA。
- 材质球、相机和缩略图预览默认使用 FXAA。离屏视口请求 TAA 时使用 FXAA，不消费主场景时间历史。
- Albedo、法线、位置等数据视图保留原始值，不执行 AA。Unlit 可用于观察几何边缘。

界面和 `SceneRenderer::antiAliasingStatus()` 显示请求、实际方式和原因。方案切换、目标尺寸／视图／投影变化、相机跳转和成功 Shader 热编译会使相关历史失效；失败编译保留上一版管线。

抗锯齿使用共享 RHI 全屏 Pass，位于场景颜色／硬件 resolve／景深之后、选中描边与最终捕获之前；编辑器 UI 不经过过滤。FXAA 读取独立源图；TAA 使用两张 RGBA16F 历史图（RGB 颜色、A 深度），再输出最终场景色。当前场景色为 RGBA8 显示域，HDR 时间重建尚未实现。

TAA 的历史位于稳定输出网格，原始光栅采样按 Halton 序列抖动。相机重投影去除采样抖动，3×3 最近表面深度用于轮廓运动和遮挡判定，历史颜色限制在当前邻域范围。历史不可用时使用当前 FXAA 结果，避免第一帧退回明显的原始毛边。深度膨胀和双线性历史仍可能使细节变软；透明度／动态变形运动矢量和高质量 HDR 时间重建属于后续扩展。

MSAA 尚未实现：RHI 需要多采样附件、PSO 采样数及 resolve，延迟管线还需要多采样 G-buffer 的着色契约。Vulkan／OpenGL／非 Windows 平台尚未有运行验证。

验证入口（使用本次构建的完整可执行路径与独立工作目录）：

```powershell
& $renderExe --aa-self-test --backend d3d11 --capture-dir $aaOutput11
& $renderExe --aa-self-test --backend d3d12 --no-backend-fallback --capture-dir $aaOutput12
& $renderExe --aa-self-test --backend d3d11Dxr --capture-dir $aaOutputHybrid
& $renderExe --aa-ui-self-test --backend d3d11 --capture-dir $aaUiOutput
```

测试输出质量参考图、方法对照和 JSON 报告。质量参考为 GPU 高分辨率渲染后降采样，不以“图像发生变化”代替边缘质量验证。

算法背景：[NVIDIA FXAA 说明](https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf)、[Microsoft MiniEngine TemporalBlend](https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/MiniEngine/Core/Shaders/TemporalBlendCS.hlsl)。当前 Shader 为引擎实现，不要求外部 Shader 源码。
