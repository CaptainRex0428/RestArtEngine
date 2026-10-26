# 资源 GC 与临时 Capture

资源 GC 按可配置周期回收闲置派生缓存和已登记的过期临时截图。C++ 对象仍按 RAII 生命周期管理；这不是扫描堆中任意指针的追踪式对象 GC，也不会删除 Actor、CPU 资产、材质草稿或撤销历史。

## 配置与操作

编辑器 `UI → Garbage collection...` 打开设置窗口。编辑值先保留为草稿，`Apply & save` 验证并保存后生效；保存失败恢复旧运行设置并保留草稿。`Discard draft` 放弃设置草稿，关闭窗口不提交。

| 设置 | 默认值 | 可用范围 |
| --- | --- | --- |
| Automatic collection | 开启 | 关闭后仍可手动执行 |
| Check interval | 60 秒 | 1–86400 秒 |
| Memory idle time | 60 秒 | 0–604800 秒 |
| Temporary capture retention | 3600 秒 | 1–31536000 秒 |

检查间隔决定多久发起一轮；闲置时间决定哪些内存资源有资格回收；临时截图保留时间按文件修改时间判断。缩短检查间隔后，已累计时间满足新周期则在下一安全点执行。`Collect now` 排队到帧末，仍遵守闲置／保留条件，不强制删除活资源或新截图。

设置默认写入进程工作目录的 `RestArtEngine.gc.json`，与布局和主题偏好同属本机配置，不写进工程场景；可用 `--gc-settings PATH` 指定位置。首次不存在时使用默认设置，不主动写文件。损坏配置保留当前设置并输出诊断；所有时间必须为有效有限数值。

```json
{
  "format": "RAEngine.GarbageCollection",
  "version": 1,
  "enabled": true,
  "intervalSeconds": 60,
  "memoryIdleSeconds": 60,
  "temporaryCaptureMaxAgeSeconds": 3600
}
```

命令行可覆盖本次运行的值，不自动改写配置文件：

```powershell
RestArtEngine.exe --project Projects/Sandbox/Sandbox.raproject --gc-interval 30 --gc-idle 120 --gc-capture-age 3600
RestArtEngine.exe --project Projects/Sandbox/Sandbox.raproject --no-gc
```

最近一次回收结果包含 CPU 字节、GPU 逻辑资源字节估算、对象数、临时截图数／字节、耗时和错误；另显示本次会话累计轮数。释放引擎引用不等于操作系统工作集或物理显存立即下降，不通过清空进程工作集制造“回收量”。

## 内存回收边界

| 对象 | 行为 |
| --- | --- |
| 闲置 mesh 顶点／索引 GPU 缓存 | 最近场景帧未使用且达到闲置时间才释放，CPU MeshAsset 保留；重新显示或撤销删除后按资产重建。 |
| 光追三角缓冲与 SRV | RT 反射和 AnalyticRay 阴影均停用、达到闲置时间后回收；同时清除 hash/count，重新开启可重建。 |
| 字体 atlas CPU 像素 | 成功上传 GPU、UI 帧结束后可释放；保留字体输入、字形信息和 GPU 字体，设备对象重建时可重新生成。 |
| ImGui 闲置窗口／表格临时缓冲 | 按最后活动时间压缩，保留布局与交互状态；再次显示时重新分配。 |
| 加载图 | Ready 的最后一次绘制提交后立即释放，不等待定时 GC。 |
| 截图 readback／CPU 像素 | 原本就是同步截图的局部对象，返回后释放；没有跨帧截图内存队列。 |

活跃场景目标、G-buffer、阴影图、Shader、常量缓冲、Logo、GPU 字体和当前画面使用的缓存不由周期回收。最小化时调度仍运行，但保留最近场景帧的活资源。旧版本 ImGui 自带的压缩计时由适配层关闭，统一走引擎调度；不修改 vendor 源码。

实际调度在 Project Manager 和编辑器的 `present` 之后，以及编辑器最小化的帧间；不只接入尚未承载真实场景的 `Engine::endFrame()` 骨架。UI 只能请求回收，不能在构造 draw list 时释放资源。DX11 由运行时管理尚未完成 GPU 命令的资源生命周期，回收解除相应绑定引用，不做阻塞等待或每轮 Flush。其他后端需要各自完成 fence／延迟销毁契约，本轮没有实现多后端 GC。

## 临时与永久截图

F9、Capture 按钮打开系统“另存为”，用户选择保存的位置，取消不生成文件；这些用户图片、显式 `--capture-dir` 输出及自测证据不参与 GC。单独 `--frames` 不再自动导出截图；需要输出时显式选择 `--capture-dir` 或 `--capture-temp`。自测缺省输出为系统临时目录下的 `RestArtEngine/Validation`。既有 `capture_*.bmp` 没有临时登记，不能只按文件名、年龄或所在 artifacts 目录推断它们是垃圾。

批量临时截图明确使用：

```powershell
RestArtEngine.exe --project Projects/Sandbox/Sandbox.raproject --frames 8 --capture-temp
```

`--capture-temp` 仅把批量运行的最后帧自动截图写入系统临时目录下的 `RestArtEngine/Captures`，不改变运行期间主动 F9 的永久保存语义；它要求 `--frames`，不能与 `--capture-dir` 或自测选项同时使用。输出路径会打印到日志。

生产接口 `GarbageCollector::captureTemporary` 在独占创建的新文件上调用截图写入器，持有父目录和文件句柄直至写完，确认完成后登记。调用者只写借用的 `FILE*`，不关闭、不重新按路径打开。低层路径签发／登记接口仅适用于受控调用，不能替代此写入保护。

清理只访问专用目录的直接子项，按严格登记格式匹配文件名、大小和修改时间。Windows 路径及文件拒绝 symlink、junction、其他 reparse point 和多硬链接；通过目录／文件句柄阻止校验到删除期间的替换或写入，并按句柄删除。未知文件、用户修改后的文件、正在写的文件和未登记文件保留；删除失败记录错误并在后续轮次重试。

每轮最多检查 512 个目录项并保存遍历位置，避免一次扫描无限目录；这不是严格的毫秒级工作预算。跨进程使用唯一名称，完成登记的临时文件共享此缓存目录，按执行回收进程的保留时间判断。进程退出后文件仍可由后续进程回收；崩溃留下但未完成登记的文件保守保留。此机制不主动清理旧 artifacts、screenshots、系统临时目录中的其他内容。

## 实现与验证入口

- `Core/GarbageCollector.h/.cpp`：调度、配置、统计、受管截图存储。
- `SceneRenderer::collectGarbage`、`ImGuiLayer::collectGarbage`：各自拥有并释放缓存。
- `SceneEditor`：设置草稿与手动请求；`app/MinimalApp.cpp`：真实循环接入。
- `tests/GarbageCollectorTests.cpp`：无 GPU 的时间、设置及文件保护回归。
- `app/GarbageCollectionSmoke.cpp`：实际 GPU 缓存回收／重建、字体与设置 UI 回归。

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-debug
ctest --test-dir build/cmake-vs2022 -C Debug --output-on-failure
# 用隔离的测试工程运行，输出不参与自动清理：
build/cmake-vs2022/Debug/RestArtEngine.exe --project <测试工程> --gc-self-test --capture-dir artifacts/gc-validation
```

2026-10-24 已使用独立 `build/gc-vs2022` Debug 构建完成 CPU 回归（1/1）、GC GPU／UI 自测、完整编辑器自测、渲染回归和实际跨进程临时截图回收。D3D11 验证错误为 0；GC 自测确认回收后重建的场景截图一致，并验证设置保存／重载和关闭自动后的手动请求。CLI 回归确认到期受管截图删除，永久截图及同目录未登记文件保留。目录锁与原子发布最终修正后重跑了受影响的 CPU、GC GPU／UI 和 CLI 流程。

本机没有创建 symlink 的权限，目录、截图、登记文件的 3 项 symlink 用例跳过；junction、硬链接、正在写入的文件、目录可写句柄拒绝及原子发布失败保旧用例实际通过。崩溃或内存分配异常留下的未发布 `.rae_gc_*.tmp` 不自动删除，不将其当作已登记截图。

Windows 原子发布在持续持有源文件句柄和目录保护时执行同目录重命名，接口依据为 Microsoft 的 [FILE_RENAME_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information) 和 [NtSetInformationFile](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntsetinformationfile)。具体日志入口与未验证范围见 [开发状态索引](DevelopmentStatus.md)。Windows 句柄保护不代表其他平台已具备同等级文件竞争防护；当前项目仍只构建 Windows/DX11。
