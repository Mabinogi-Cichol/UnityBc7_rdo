# Feature TODO：RGBM Hook DLL 合并

- Feature ID：`rgbm-hook-dll-consolidation`
- 登记日期：2026-10-10（Asia/Shanghai）
- 状态：TODO，仅登记，尚未施工
- 来源：用户要求先合并 C#，将 DLL 合并作为后续 feature 记录
- 配套：[施工记录](rgbm-hook-dll-consolidation-construction.md)
- 基线：[RGBM Hook portability P5](rgbm-hook-portability-acceptance.md)

## 目标

将 `bc7_rgbm_coordinator.dll` 的安装协调、Detours 事务和恢复代码并入 `bc7_rdo_hook.dll`，减少一个需要分发和配置导入器的原生 DLL。继续保留独立、无 CRT 的 `bc7_rgbm_loader_gate.dll`，以及 `RgbmBindings~` 的定位工具与依赖。

目标插件只有原来的 `hook_loader.cs` 一个生产 C# 脚本；其绑定准备已在 portability 后续整理中合并。此次 TODO 不合并解析 EXE／DIA，也不改变编码算法、参数、接口认证或上下文关联。

## 拟调整

1. 将 coordinator／checked API／Detours transaction engine 链接到主 DLL，保持安装窗口内不使用 CRT 堆、日志、宿主锁或线程完成等待。
2. 主 DLL 导出 gate 所需的 `RgbmCommitFromLoaderGate`；gate 改为导入主 DLL。重审模块加载、原函数槽、模块保留和驻留寿命，避免循环加载或提前卸载。
3. 原生运行时直接调用协调接口，移除动态加载 coordinator 的路径；保留全部入口预检、线程覆盖、事务恢复、Unavailable／Faulted 行为。
4. 更新构建、暂存、发行清单、平台导入器、说明和缺失依赖测试。升级时先备份，在目标工程关闭后清理旧 coordinator DLL／meta。

## 验收关口

- 独立 gate 仍通过正常 Windows DLL 初始化规则控制新线程；不将线程快照或 Editor idle 当作安全屏障。
- 原有 P2 的相对寻址、并发／新线程、安装失败、撤销恢复及 Faulted 矩阵通过；原有 P3 上下文／TLS／槽与异常退出测试通过。
- 同一最终插件通过 f1／f2 Editor、batchmode、真实 Worker、RGBM／普通分流、零 λ、继承、ERT、Oodle 缺失、Unknown 原生回退及实际工程验收。
- 配置首次读取 120×64 线程回归通过，原生／Worker 输出和日志块覆盖一致。
- 发行和安装目录没有旧 coordinator 残留，配置与原资源恢复，所有产物身份可追溯。

## 当前边界

这是一项后续结构调整，没有实现、DLL 合并产物或已通过结论。当前仍使用 P5 已验收的三个原生 DLL；不能直接删除 coordinator 或 gate。启动本 feature 后先确认独立模块合并对线程事务、gate 依赖和故障测试的影响，再施工和回归。
