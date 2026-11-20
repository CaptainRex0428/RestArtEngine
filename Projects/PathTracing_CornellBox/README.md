# Path Tracing — Cornell Box

红色左墙、绿色右墙、中性地面／天花板／后墙，以及三颗悬浮排列的银色、缎面和镜面金属球。矩形顶灯是唯一光源，方向光和环境光强度均为零。

## 打开与调整

从引擎 Project Manager 选择 `PathTracing_CornellBox`，或在引擎根目录运行：

```powershell
.\bin\windows-x86_64\Debug\RestArtEngine.exe --project Projects/PathTracing_CornellBox/PathTracing_CornellBox.raproject
```

需要支持 DXR 1.1 的 Windows 显卡及可用 DXC。默认硬件光追、Path tracing、Progressive accumulation；两种间接贡献均开启，4 samples/frame、1024 target samples、8 次表面着色。查看工具栏 GI 和 Project Settings 的实际状态；停止移动相机后图像逐步累积。硬件不可用会给出原因，光栅预览不等价于该照明效果。

Scene 面板可编辑灯位置、尺寸和辐亮度。两个半轴在场景文件中定义，叉积指向单面发光侧。初始视角也保存在场景：`Store current view` 记录当前视角，Save Scene 保存；导航不会自动改写书签，`Restore view` 回到它。

## 场景内容

| 内容 | 设置 |
| --- | --- |
| 房间 | 5.2 × 4 × 5.2，前方开口；5 个 Plane Actor |
| 球体 | 半径 0.6，中心高度 2，3 个 Sphere Actor |
| 材质 | 六个项目 `.rasset` 及缩略图；金属球粗糙度 0.07／0.29／0.025 |
| 顶灯 | 中心 `(0, 3.96, -0.35)`，尺寸 `1.3 × 1.0`，向下，线性辐亮度 `(15,15,15)`；默认曝光 0.38 |
| 网格 | Engine 内置 Plane／Sphere GUID，无项目 Builtin 副本 |

顶灯是 Scene 照明参数中的解析发光面，支持相机／反射可见性、随机面积和 BRDF 采样的 MIS，当前仅用于渐进式 Path Tracing。它不是通用发光材质或可选中的 Mesh Actor。球体是不透明金属，没有玻璃折射；项目使用标准材质 Shader。

## 可复现生成与截图

工程运行不依赖生成器。维护者可选择构建工具，并生成到新的空目录：

```powershell
cmake --build --preset windows-debug --target GenerateCornellBox
.\build\cmake-vs2022\Debug\GenerateCornellBox.exe . <新的空目录>
```

工具拒绝非空目录，使用引擎的资产写入、缩略图和场景序列化 API，并检查实际重载。

固定帧截图（显式输出目录，与交互式 Capture 保存选框无关）：

```powershell
.\bin\windows-x86_64\Debug\RestArtEngine.exe --project Projects/PathTracing_CornellBox/PathTracing_CornellBox.raproject --no-ui --frames 256 --capture-dir artifacts/cornell-example
```

结束时打印实际 GI 模式、累计样本与 DXR dispatch。确认 `pathTracing`、`progressive` 和目标样本达到后再评估画面；耗时取决于 GPU、分辨率和后台状态。
