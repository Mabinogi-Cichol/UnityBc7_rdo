# P2：Detour 安装协调与恢复边界

- 日期：2026-10-10（Asia/Shanghai）
- 状态：P2 后端关口通过；[P3 运行时](rgbm-hook-portability-runtime.md)已接入两点 Hook 与编码退化，项目部署和真实调度／编码仍按 P4／P5 验收。
- 关联：[设计](rgbm-hook-portability-design.md)、[施工记录](rgbm-hook-portability-construction.md)、[P1 绑定](rgbm-hook-portability-cache.md)

## 1. 安装约束与实现

线程快照不能排除快照后的新调用线程。Windows 明确规定，在 DLL 初始化期间，新建线程必须等初始化结束后才开始执行。施工采用独立协调 DLL，加载同目录、无 CRT 的 gate DLL，由后者的 `DLL_PROCESS_ATTACH` 调用已导入的协调器函数；不会由调用者传入一个未经证明的 `thread_creation_quiesced=true`。[CreateThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createthread)

协调器先在 gate 外确认模块路径、目标批次及模块寿命，之后在 gate 内再次全量预检并取得线程快照。打开本进程的线程句柄，校验归属，容量超过 2048 或访问失败即拒绝；退出线程可以跳过，不能跳过无法核实的线程。快照后按正常 Windows 初始化规则创建的线程尚不能调用目标。既有线程由 Detours 登记和暂停，成功登记后先检查 Get／SetThreadContext 能否访问其控制上下文，才提交事务。[DetourUpdateThread](https://github.com/microsoft/Detours/wiki/DetourUpdateThread)

DLL 初始化不能成为等待宿主锁的路径。实现用原子 busy 状态直接拒绝并发事务，不使用 `std::mutex`、HeapLock、COM、日志或线程完成等待。全部句柄在暂停前收集，全部 Attach／Detach 操作和跳板在暂停前准备；事务节点通过协调 DLL 专用的 VirtualAlloc／VirtualFree 分配器处理，暂停后不再依赖 CRT 堆锁。协调 DLL 的分配器不影响宿主或 Hook 模块。gate 只做一次已导入函数调用，无 CRT、无动态查询、无额外线程。测试构建的快照后线程创建专门用于验证竞态，正式构建没有该测试路径。[DllMain 约束](https://learn.microsoft.com/en-us/windows/win32/Dlls/dynamic-link-library-best-practices)

保留上游 Detours 4.0.1、commit `e4bfd6b03e50de46b47abfbd1e46b384f0c5f833` 及 MIT 许可证。上游文件未修改，`detours_transaction_engine.cpp` 在编译时为其两类事务节点使用 nothrow 分配，并包装恢复相关 Windows API，记录上游 Commit／Abort 有意忽略的错误。构建不启用 `DETOUR_DEBUG`。P1 解码器仍使用原始静态库。

这条保证覆盖正常 Windows 线程创建／DLL 初始化规则；没有把线程快照或 MCP idle 当作安全屏障。接口契约、调用签名及完整加载映像仍需由 P1／P3 单独验证，协调器本身只确认批次的入口字节、页面、槽位和线程事务。

## 2. 撤销与模块寿命

正常 Unity 会话不热卸载 Hook。成功安装后，协调器保留自身、Hook 和目标映像的模块引用；一个协调器管理一个安装批次。宿主还必须保留 original 槽与生成代码，直至进程退出或真正完成撤销。持有模块引用不能替代槽位和生成代码的寿命。[GetModuleHandleEx](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandleexw)

`RgbmRemoveDetoursForStoppedHost` 仅供能够阻止并清退所有入口／Hook／trampoline 调用的合作宿主。它要求原批次完全一致，再通过相同 gate 撤销和复核；未提供清退保证时拒绝。Unity idle、活跃 Hook 计数为零或暂停线程，都无法排除已经读取 trampoline 指针或尚未进入 Hook 的调用，因此 Unity 生产接入不调用该撤销 API。进程退出不在 DllMain 中等待调用完成。隔离测试与 Editor 探针能控制自己的调用线程，因而能够使用这一撤销接口。

| 状态 | 含义与调用者要求 |
| --- | --- |
| `Rejected` | 参数、忙状态或合作宿主保证不足；不表示已有 Hook 已移除 |
| `Unavailable` | 本次安装未提交，或事务已撤销并核对原槽、入口字节与保护；按 error 处理原因 |
| `Installed` | 已有全批次仍存在；error=0 表示本次安装成功，error 非零也可表示撤销失败后完整恢复到已安装状态；保持模块、槽和代码寿命 |
| `Removed` | 合作宿主的全批次撤销及原字节／槽／保护复核成功 |
| `Faulted` | 页保护、上下文更新、线程恢复或恢复复核失败；保留引用，禁止继续安装／撤销，不假装已经安全恢复 |

成功与否必须同时检查 state、error 和 recovery_error；例如重复安装返回 `Installed` 加非零 error，表示已有批次仍存在，并非本次重新安装成功。严重恢复失败不能保证进程仍适合继续工作；P3 生产运行时对此直接非零终止，退出码与边界见运行时报告，实际加载器接入由 P4 完成。

## 3. 源码与构建

| 文件 | 职责 |
| --- | --- |
| `src/detour_coordinator.{h,cpp}` | 固定容量 C ABI、两次全量预检、模块引用、线程收集和结果状态 |
| `src/detour_loader_gate.cpp` | 无 CRT 的 DLL 初始化 gate |
| `src/detours_checked_api.{h,cpp}` | 协调器专用分配器及恢复错误记录 |
| `src/detours_transaction_engine.cpp` | 包装原样保留的上游 Detours 事务实现 |
| `tests/test_detour_coordinator.cpp` | 并发、新线程、恢复与故障状态矩阵 |
| `tests/unity/detour_coordinator_probe.cpp` / `run_detour_coordinator_probe.csx` | Editor 内自有代码验证和可重复的 MCP 调用 |

```powershell
& tools/build_rgbm_coordinator.ps1 -Testing *> out/rgbm-portability/build-coordinator-test.log
& tools/test_rgbm_coordinator.ps1 *> out/rgbm-portability/test-coordinator.log
& tools/build_rgbm_coordinator.ps1 *> out/rgbm-portability/build-coordinator.log
```

MSVC x64，协调器 `/MT /O2 /EHsc /std:c++17`；gate `/O2 /GS- /NODEFAULTLIB /ENTRY:RgbmLoaderGateEntry`。正式协调器没有 `RgbmSetDetourTestFaults` 导出；gate 仅导入协调器的一次 commit 调用。产物在忽略目录 `out/rgbm-portability/coordinator/`；测试故障 API 和探针不属于发行包。

旧 `native_detour_backend` 保留为合作宿主的早期隔离基线，其布尔前提仍不允许直接用于 Unity。生产接入应使用新的协调接口。

## 4. 验证结果与边界

独立矩阵以真实退出码 0 完成四个进程场景，汇总为 `coordinator/test/summary.json`：

- 三个目标覆盖 RIP-relative 读取、相对 call、短条件分支；两个安装者竞争时只有一个成功；32 轮事务、8 个持续调用线程及另一个持续新建调用线程的创建者。
- 每轮在 gate 内、快照完成后创建一个确定遗漏于快照的调用线程；32 次均在提交后读到 Hook 结果。
- 入口指纹错误、重复目标、非法 original 槽、已有 gate、第三个 Attach 失败、分配失败（含已暂停首个线程后的失败）、上下文检查失败、线程容量不足、缺失 gate 均拒绝；可恢复的安装失败恢复原字节与 original 槽。撤销第二个 Detach 分配失败则完整恢复到事务前已安装状态，复核入口和 trampoline 槽后再次撤销成功。
- 页保护恢复故障和线程恢复错误报告均为 sticky `Faulted`，后续操作拒绝；测试进程保留 Hook 到退出。线程恢复故障测试执行真实 ResumeThread 后注入错误报告，验证状态策略，不声称复现了 OS 实际无法恢复线程。

当前用户 f2 Editor，PID 41644：MCP 临时加载正式协调器与开发探针，完成 8 轮安装／撤销。探针只 Hook 自己 VirtualAlloc 的代码，透传 trampoline 并加一；Unity 压缩入口没有修改。真实统计、错误和恢复状态见 `coordinator/live-owned-code-f2.json`。结果通过，bad_calls=0、原代码复原、无剩余 Hook。之后确认协调器、gate、探针都已卸载，临时环境变量恢复 null，Editor 未编译或导入。

产物与最终报告 SHA256 见 `coordinator/artifact-sha256.json`。这些验证证明协调器的事务和恢复边界，不能代替 Unity 私有 ABI、RGBM usage、Job 语义或实际编码质量验收；用户工程旧插件的 `not hooked` 尚待 P4 部署及 P5 实际编码验收。
