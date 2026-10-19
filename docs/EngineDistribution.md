# 引擎启动、工程链接与发布

RAEngine 的运行时和工程内容是两个可移动的包。`RAEngine.exe` 是引擎工程管理器和编辑器程序；`.raproject` 是工程入口文件，记录工程自己的 Content、Shader、场景以及所依赖的引擎身份。工程不需要把引擎源码或引擎资产复制进去。

## 两种目录

一个可发布的引擎目录可以是：

```text
RAEngine/
├─ bin/
│  └─ RAEngine.exe             # CMake install package
├─ Engine.raeengine
├─ Shaders/
│  ├─ Private/                  # 公共渲染管线
│  └─ Materials/                # 默认材质模板
└─ Projects/
   └─ Sandbox/                  # 可选案例工程
      ├─ Sandbox.raproject
      ├─ Content/
      └─ Shaders/
```

工程目录只保存自己的数据：

```text
MyProject/
├─ MyProject.raproject
├─ Content/
│  ├─ Scenes/
│  ├─ Materials/
│  └─ Meshes/
└─ Shaders/
   └─ Materials/Standard.hlsli
```

工程目录只保存数据，不包含也不生成 `.sln`。双击 `.raproject` 或从 `RAEngine.exe` 选择它都能打开。

## 构建并启动引擎

在引擎源码目录配置 CMake 后，`RAEngine` 目标生成文件名为 `RAEngine.exe`：

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-debug --target RAEngine
.\build\cmake-vs2022\Debug\RAEngine.exe
```

`RAEngine` 是唯一的构建目标，输出 `RAEngine.exe`。它同时是 Project Manager（launcher）和编辑器。

无参数启动时，程序显示 Project Manager。工程管理器可以创建工程、打开 `.raproject`、列出引擎目录下的 Sandbox 案例，或从 `--project` 参数直接进入工程：

```powershell
# 显示工程管理器
.\RAEngine.exe

# 直接打开工程目录或 manifest
.\RAEngine.exe --project E:\Work\MyProject
.\RAEngine.exe --project E:\Work\MyProject\MyProject.raproject

# 从另一份引擎启动工程管理器
.\RAEngine.exe --engine E:\Engines\RAEngine
```

## 工程与引擎的链接

工程 manifest 使用相对于工程目录的 `engineRoot`，同时保存版本和稳定 hash：

```json
{
  "format": "RAEngine.Project",
  "version": 3,
  "name": "MyProject",
  "engineRoot": "../Engines/RAEngine",
  "engineAssociation": "0.2.0",
  "engineVersion": "0.2.0",
  "engineHash": "dev-0.2.0",
  "contentRoot": "Content",
  "shaderRoot": "Shaders",
  "defaultScene": "Content/Scenes/Default.rascene"
}
```

`engineAssociation` 是命名的版本关联，由启动器通过引擎注册表解析到安装目录；`engineRoot` 是缓存 / 回退路径。工程管理器读取 `Engine.raeengine` 并比较 `engineVersion` 和 `engineHash`。匹配时直接打开；不匹配时用户可以更新绑定到当前引擎、使用工程记录的引擎、浏览选择其它引擎版本，或关闭返回工程管理器。更新绑定只改 manifest（`engineAssociation`、`engineRoot`、版本、hash），不复制或删除工程内容。移动工程或引擎后，应在工程管理器中重新选择引擎，避免手工填写失效路径。

## 创建工程和上传工程

在 Project Manager 中选择 **New Project**，指定父目录和工程名。工程管理器创建 `.raproject`、`Content/`、工程 Shader 和默认场景，并把当前引擎的 association、版本、hash 和相对路径写入 manifest。工程是纯数据，不生成 `.sln`。案例工程位于引擎仓库的 `Projects/Sandbox`，创建新工程时复制其内容，不会把新工程的保存结果写回案例或引擎公共 Shader。

上传工程时只提交工程目录（`.raproject`、`Content/`、工程 `Shaders/`），不提交引擎目录。接收者安装或解压兼容版本的 RAEngine 后，用 `RAEngine.exe --project MyProject/MyProject.raproject` 打开。上传引擎本身时则保留 `RAEngine.exe`、`Engine.raeengine`、`Shaders/` 以及可选的 `Projects/Sandbox/`，不需要包含任何个人工程。

## 发布检查

发布一个可独立使用的引擎时，至少检查：

1. `RAEngine.exe` 能在目标机器启动 Project Manager。
2. `Engine.raeengine` 的版本和 hash 与此次构建一致。
3. `Shaders/Private/Scene.hlsl` 和默认材质模板随包存在。
4. 若分发案例，`Projects/Sandbox/Sandbox.raproject` 的 `engineRoot`、版本和 hash 指向当前引擎。
5. 在干净目录中用 `RAEngine.exe --project` 打开一个外部工程，确认工程内容不会写回引擎目录。

安装包可以只包含上述运行时文件；开发者若需要扩展引擎，再额外提供源码、CMake 配置、头文件和静态库导出。这样引擎能够单独开源和发布，工程可以独立版本管理，并可在多个引擎版本之间切换。


