# 工程与引擎管理

RAEngine 的可执行引擎和用户工程是两个独立目录。引擎根目录包含 `RAEngine.exe`、`Shaders/Private/`、默认材质模板和 `Engine.raeengine`；工程目录包含 `.raproject`、`Content/`、工程 Shader 和场景。工程不会把用户资产写回引擎目录，因此同一份引擎可以被多个工程复用，也可以把引擎单独发布。`RAEngine.exe` 同时是 Project Manager（launcher）和编辑器，没有单独的 launcher 程序，也没有单独的运行时库。

## 引擎身份

`Engine.raeengine` 保存引擎的发布身份：

```json
{
  "format": "RAEngine.Engine",
  "version": 1,
  "name": "RAEngine",
  "engineVersion": "0.2.0",
  "engineHash": "dev-0.2.0"
}
```

`engineHash` 是发行或构建流程写入的稳定构建标识。工程启动时同时比较版本和 hash；两者都匹配才表示使用完全相同的引擎身份。

## 工程链接

工程 manifest 使用相对工程目录的 `engineRoot` 作为缓存路径，并保存平坦的 `engineAssociation`（命名的版本关联）、`engineVersion` 与 `engineHash`：

```json
{
  "format": "RAEngine.Project",
  "version": 3,
  "name": "MyProject",
  "engineRoot": "../../RAEngine",
  "engineAssociation": "0.2.0",
  "engineVersion": "0.2.0",
  "engineHash": "dev-0.2.0",
  "contentRoot": "Content",
  "shaderRoot": "Shaders",
  "defaultScene": "Content/Scenes/Default.rascene"
}
```

`engineAssociation` 是 UE 式 `EngineAssociation` 的对应物：启动器通过每用户的引擎注册表（`engines.json`，Windows 位于 `%LOCALAPPDATA%\RAEngine\engines.json`）把它解析到某个引擎安装目录；`engineRoot` 则作为缓存 / 回退，保证 `--project` 直接打开时无需注册表。切版本 = 改 `engineAssociation` 并重写 `engineRoot`。

`loadProject` 会验证工程资源路径位于工程目录内，并读取 `engineRoot` 的身份。旧的 v1 / v2 manifest 仍可加载：v1 标记为 `LegacyProject`，首次通过工程管理器打开时可以升级绑定；v2 只是缺少 `engineAssociation`。

## 工程管理器流程

直接启动 `RAEngine.exe` 即可进入 Project Manager。它提供：

- 打开已有工程；
- 从 `Projects/` 打开案例工程；
- 从 Sandbox 模板创建独立工程；
- 在引擎身份不匹配时「使用引用的引擎」「更新到当前引擎」「选择引擎…」（浏览目录或从已发现引擎列表选择）或关闭。

启动时 Project Manager 会把当前运行的引擎登记进引擎注册表；打开工程时若版本 / hash 不匹配，可任选上述操作原子更新工程 manifest 并重新加载。命令行传入 `--project` 时保持直接打开工程的工作流，适合脚本和自动化验证。

如果工程的 `engineRoot` 已失效（引擎被移动或删除），打开时会先用 `engineAssociation` 在引擎注册表中解析到可用引擎，自动重写 `engineRoot` 后恢复；解析不到匹配引擎、或工程 manifest 不可写（无法更新根路径）时，会提示「Cannot open project」并给出原因，不会静默跳过。

工程是纯数据：`.raproject` + `Content/` + 工程 `Shaders/`，由 `RAEngine.exe` 打开，不生成也不携带自己的 `.sln`。编译引擎只打开仓库根目录的 `RAEngine.sln`（一个工程）。`writeProjectBuildFiles` 保留为后续原生 C++ 工程的库能力，当前的数据工程流程不会调用它。

## 发布和迁移建议

发布引擎时为每个构建写入新的 `engineVersion` / `engineHash`，并把 `RAEngine.exe`、`Engine.raeengine` 与公共 Shader 一起分发；如果要提供示例，再包含 `Projects/Sandbox`。移动工程后应检查 `engineRoot` 是否仍能解析到目标引擎；移动引擎时可在工程管理器中重新绑定工程。工程目录可以提交到独立仓库，仓库只保留 manifest、Content、工程 Shader 与场景文件。完整的目录结构和发布检查见 [EngineDistribution.md](EngineDistribution.md)。

