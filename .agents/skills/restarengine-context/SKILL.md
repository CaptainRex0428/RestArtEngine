---
name: restarengine-context
description: "Recover RestArtEngine requirements, current implementation, progress and evidence before resuming work or assigning an engine task. Use for progress synchronization and context handoff; keep domain documents as the source of truth."
---

# RestArtEngine 项目上下文

先读 [DevelopmentStatus.md](../../../docs/DevelopmentStatus.md)，按索引定位领域事实；结构与现有行为以 [Architecture.md](../../../docs/Architecture.md)、[README.md](../../../README.md) 及相关源码为准。仓库 `AGENTS.md` 若存在则读取，它是本机补充入口，不能成为换机器后唯一可用的规则。

## 建立当前基线

- 读取用户本轮目标和已经确认的约束，再查看 `git status --short`、相关 diff 与必要的提交记录。保留用户和其他任务的未提交改动，不默认 HEAD 代表整个工作区。
- 调查期间相关文件发生外部变化时，注明取样范围，重新核对受影响接口与结论；旧分析不能直接授权覆盖新实现。无法得到稳定快照时交付暂定结论与待复核项，不持续追逐变化或阻塞无关工作。
- 按状态索引只读相关领域文档与源码，避免把整套文档装入每个子任务。核对文档日期、证据对应的实现及本轮变化；文档冲突时记录差异，以源码确认现状，以用户要求确认目标。
- 将信息分为：已确认需求、源码已实现、已有运行证据、待设计／未实现。运行证据写明构建、平台、场景和覆盖范围；缺失的本机 artifacts 只保留为历史记录，不能冒充本轮运行结果。
- 对“待验证”和“未实现”分别处理；构建通过不是交互或画面通过，版本枚举不是后端可用。最新用户需求可以改变目标，不能因此重写过去的验证结果。

## 向执行者提供的最小上下文

交付当前目标与验收、事实源链接、相关代码入口、工作区已有改动、已知缺口和需要解决的依赖。只有对下一步有影响的不确定性才升级为问题。

更新进度时由主协调者维护共享索引，专项执行者回报自己领域的变化；行为细节写回原领域文档。不要将进度复制进多个 `SKILL.md`，不要为同步进度重写无关文档或生成未经确认的路线图。
