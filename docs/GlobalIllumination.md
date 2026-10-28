# 全局光照与渐进式路径追踪

GI 描述场景中的间接光照。引擎用平台无关的 `GlobalIlluminationSettings`／`GlobalIlluminationStatus` 表达请求、采样配置及实际状态，与 Forward／Deferred 管线组织及图形 API 分开。当前可选择 Disabled 和 Progressive path tracing；后者由 Windows DXR 1.1 实现。其他实时 GI 算法及后端尚未实现，不支持时保留项目请求并显示原因。

## 使用

打开 `Project → Project Settings... → Rendering`（F3），将 `GI method` 设为 `Progressive path tracing`，会同时启用硬件光追。关闭硬件开关时保留 GI 请求。普通数值、材质草稿与相机变化自动触发更新，无需 Apply 或保存；`Save project settings`／`Save All` 将设置写入 `.raproject`。

| 配置 | 默认与范围 | 行为 |
| --- | --- | --- |
| Samples per frame | 1，范围 1–16 | 每次主场景提交增加的每像素样本数。 |
| Target samples | 256，范围 1–65536 | 达到目标后保持已有结果；提高目标可继续累积。 |
| Max bounces | 4，范围 1–12 | 最多着色的表面数，包含相机首先看到的表面。1 包含直接光与无遮挡环境贡献；2 开始包含物体间反弹。 |
| Restart accumulation | 手动操作 | 下一次主场景帧重新开始，不修改资产或项目参数。 |

进度条显示当前历史的已提交样本数和目标。初期样本可能有噪点，静止后持续累积；当前没有降噪器。采样批次较大或反弹数较高会增加交互延迟，不能承诺固定帧率或固定时间收敛。

命令行 `--path-tracing` 在本次启动请求硬件路径追踪；`--no-hardware-rt` 可覆盖硬件开关。持久化仍由配置页保存。

## 光传输

`Shaders/Private/PathTracing.hlsli` 实现不透明表面的路径积分：方向光直接光照估计、Lambert 漫反射和 GGX 粗糙镜面混合采样、多次表面反弹、环境逃逸及 Russian roulette。吞吐量按 BRDF、余弦项和混合采样 PDF 更新。间接光来自射线命中及环境，不叠加旧预览的固定环境漫反射项。

材质使用当前工程的 `EvaluateMaterial`，保留颜色、粗糙度、金属度、Grid、双面和只读草稿语义。路径模式将 albedo 限制为 0–1 反射率；发光与透明材质尚未实现。几何法线用于射线偏移和表面有效半球检查，平滑法线用于 BRDF。主射线抖动实现空间采样；样本序列由像素及样本索引决定。

硬件 `Ray-traced reflections` 开关控制间接镜面分量；关闭后仍计算漫反射 GI，直接高光保留。`Ray-traced shadows` 控制方向光遮挡测试，GI 的表面相互遮挡仍由路径求交决定。黑背景只隐藏相机直接看到的环境，不关闭环境照明。

## 累积与失效

显示使用共享 RGBA8 图像，历史使用独立 RGBA32_FLOAT 线性 HDR 图像。每批样本以样本数加权更新均值，之后执行曝光及显示转换；不平均已经色调映射的颜色。首批样本覆盖历史，不读取未初始化资源。

相机、视口尺寸、几何／可见性、材质／法线、光照、背景、反弹数、光追效果开关、成功发布的 Shader 及显式重启会使历史失效。材质预览恢复已提交值同样触发新历史。提高目标或改变每帧采样量可以继续累积；将目标降至当前计数以下时重新开始。曝光只重新解析显示，不重新采样。

非 Lit 调试视图暂停累积，不将 Albedo／Normal 等数据写进 HDR 历史；返回 Lit 时有效历史可以继续。材质球、详情预览和缩略图不消费或重置主场景样本。Shader 候选失败保留旧管线和历史，成功后重新累积。同步及硬件能力要求见 [HardwareRayTracing](HardwareRayTracing.md)。

## 项目格式与验证

可选 `rendering.globalIllumination` 对象：

```json
{
  "method": "pathTracing",
  "samplesPerFrame": 1,
  "targetSamples": 256,
  "maxBounces": 4
}
```

缺失时按 `disabled` 读取；整数范围和算法字符串严格验证。保存保留未知嵌套字段，失败保留原 manifest。设置属于用户请求，不随设备能力自动改写。

运行 `RestArtEngine.exe --gi-self-test --capture-dir artifacts/gi` 验证真实 GPU 画面。验收包含灰色地面对彩色墙的间接响应、低／高样本误差、历史重置、曝光独立、材质草稿、Shader 失败恢复及前向／延迟组合；`--hardware-rt-self-test` 补充项目配置的真实交互和保存重载。设备不可用时必须明确报告跳过，不能把零错误或 dispatch 计数视为 GI 画质证明。

当前范围不包含透明／折射、发光材质、体积散射、纹理重要性采样、环境光重要性采样或降噪。有限反弹、有限样本及现有材质模型共同限制结果；高动态范围环境和低粗糙材质可能需要更多样本。
