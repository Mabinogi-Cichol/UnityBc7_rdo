# P3：两点 Hook、上下文运行时与编码退化

- 日期：2026-10-10（Asia/Shanghai）
- 接口：`unity.threaded-copy-for-each-callback.v1`
- 状态：P3 源码接入与阶段测试完成；项目部署、真实 Unity 调度／编码和跨版本完整验收按 P4／P5 跟踪。
- 关联：[设计](rgbm-hook-portability-design.md)、[接口证据](rgbm-hook-portability-interface.md)、[P2 协调器](rgbm-hook-portability-detours.md)、[施工记录](rgbm-hook-portability-construction.md)

## 1. 生产接入

`rgbm_hook_install.cpp` 读取当前 exe、复核 P1 缓存及全部 11 个加载入口，才通过 P2 协调器统一安装 Compress 和 Process 两个 Hook。缓存不成为信任根；没有版本 RVA 表。协调器依赖使用主 Hook 所在目录的绝对路径加载，缺失依赖不使主 Hook DLL 加载失败。

压缩入口完整转发九个参数，输出保持可写 `uint8_t*`。它根据认证接口的原生 usage 判定建立 TLS：usage 2／5／9 为 RGBM，其余为 Ordinary。该判定只在完整 v1 usage 契约通过后参与生产编码。Process 首参为 rows，JobData 是不透明输入指针，运行时不读取字段或像素地址。

`rgbm_hook_runtime.cpp` 提供 64 个预编译静态 callback。每次非零行调度取得一个槽，保存原回调与三态上下文，使用包装回调调用原 Process。Worker 保存旧 TLS，建立本次上下文，原样转发 `(copied_job, begin, end)`，通过 `__finally` 恢复 TLS 和活跃计数。原 Process 正常返回的同步 join 契约成立后才释放槽；槽不会按 JobData 地址关联，支持地址复用和嵌套。

`bc7_hook_dll.cpp` 已移除固定 RVA、手工复制私有入口 prologue、像素标记向量、JobData 字段偏移及 lambda Hook。普通 BC7 导出桩 Hook 和配置缓存并发修复保持既有实现。

## 2. 状态与编码决策

| 条件 | 编码行为 |
| --- | --- |
| 配置 `unity_rgbm_context_hooks=0` | 显式关闭的既有普通纹理路径 |
| 设置为 1，状态尚未 Ready | Unity 原生 BC7；不调用 Oodle／ERT |
| Ready，但本次上下文 Unknown | Unity 原生 BC7；不调用 Oodle／ERT |
| Ready、Ordinary | 原有普通 Oodle lambda／ERT 参数 |
| Ready、RGBM | `rgbm_oodle_lambda=-1` 继承普通值，非负值覆盖；ERT 根据 `rgbm_rdo` 使用 RGBM 度量 |
| 所选 Oodle lambda 为 0 | 原有原生 BC7 快路径，不加载 Oodle，不接 ERT |
| Oodle 不可用／编码失败 | 保持已有 BC7 原始编码加 ERT 的退化路径 |

状态为 Disabled／Resolving／Validating／Installing／Ready／Unavailable／Faulted。发布 Ready 必须以 Installing 为前置状态；安装期间发生槽耗尽不能被随后安装成功覆盖。槽耗尽使整个进程 sticky Unavailable，已有槽、直接转发的回调及带有旧 RGBM TLS 的 Worker 都读取 Unknown；正常 join 后仍可回收槽，但不自动恢复分类。

初始化一次，只在 DllMain 之外执行。P3 提供 `Bc7RgbmInitialize(const wchar_t*)` 与 `Bc7RgbmGetState()` 两个 C 导出，状态数值按头文件枚举固定。初始化传入缓存绝对路径；P4 负责生成缓存、准备依赖、传参及逐进程状态检查。轮询线程也支持注入前设置的 `BC7_RGBM_BINDING_CACHE`；缺少缓存时保持 Unavailable。运行中开启配置会安全退化，不借此绕过一次性安装限制。

## 3. 异常与寿命边界

压缩入口与 Worker 回调在 C++／SEH 异常展开时恢复 TLS。Process 若异常离开，无法证明 join；隔离槽不能延长 Unity 复制在栈上的 JobData 寿命，因此生产实现不继续展开或复用槽，直接 `TerminateProcess`。若正常返回但槽仍有活跃回调，也视为契约破坏并终止。没有进程退出时的 Hook 清退或 DllMain 等待，Unity Hook 驻留到进程退出。

| 退出码 | 含义 |
| --- | --- |
| `0xE0427001` | Process 异常退出，join 无法确认 |
| `0xE0427002` | join 后槽状态非法／仍有活跃回调 |
| `0xE0427003` | 无有效作用域的包装回调被调用 |
| `0xE0427004` | 必需原函数指针为空 |
| `0xE0427005` | 非零行调度回调为空 |
| `0xE0427006` | 安装协调器返回 Faulted／严重恢复错误 |

未找到绑定、认证拒绝、缺失依赖、事务拒绝且恢复完成属于 Unavailable，继续原生编码。严重恢复失败直接非零终止，不能伪装成已恢复的 Unavailable。上述故障路径不持有日志／分配器锁做清理。

## 4. 验证与证据范围

构建 `tools/build.ps1` 产生主 Hook、正式协调器及 gate，并暂存到 `out/editor-plugin/Bc7Rdo`；P3 结束时尚未向用户项目部署，后续已按 P4／P5 部署。开发测试使用 `tools/build_rgbm_runtime.ps1` 和 `tests/test_rgbm_runtime.py`，全部日志位于忽略的 `out/rgbm-portability/runtime`。

- 16 个并发调用者重复使用输入地址，含普通／RGBM 混合与嵌套普通任务：2,561 个任务、2,561 次回调，参数转发、可写输出、复制 payload、旧 TLS 恢复和正常槽回收通过。
- 三态及各非 Ready 状态、显式关闭、RGBM lambda 继承／零／覆盖、容量耗尽和 sticky 原生退化通过。直接受控调用验证 C++／SEH 包装回调与压缩入口 C++ 异常后的 TLS 恢复。
- 独立子进程验证 C++／SEH 调度异常及违背 join 的故障退出码。C++ 子进程设外层 handler，确保触发异常展开与 finally，而不是把 CRT 无 handler 的直接 abort 当作测试通过。
- 使用同一生产运行时编译的测试 exe，对 f1／f2 的真实 process／dispatcher 隔离副本各执行 149 个任务、299 次回调；复制 payload、上下文、Worker TLS 恢复各 299 次一致，148 次 schedule／complete／clear 一致，槽回收为 0。外部调度器由测试宿主替代，**不属于真实 Unity JobQueue 验收**。
- 新生产 DLL 的 `tests/test_hook.py` 通过既有原生转发、Oodle 零 lambda、ERT 生效，以及 Unknown 在 Oodle／ERT 非零 lambda 下均与原生 BC7 逐字节一致的回归。64 线程首次读取配置的既有崩溃回归重复 120 个独立进程，全部退出 0。
- 当前 f2 Editor（PID 41644）临时探针先验证 11 个实际加载绑定，再使用确认不存在的协调 DLL 目录：state=Unavailable、缺失依赖错误 126、原生退化决策正确、Unity 入口字节未变。探针卸载、环境变量恢复、Editor 未编译／更新。该实验验证真实宿主中的拒绝路径，不安装生产语义 Hook。

主要证据：`summary.json`、`clone-f1.json`、`clone-f2.json`、`live-unavailable-f2.json`、`cleanup-f2.json`、`artifact-sha256.json`、`build-production.log`、`build-tests.log`、`test-runtime.log`、`test-hook.log`。临时 UnityMCP 驱动保存为 `tests/unity/run_runtime_unavailable_probe.csx`。

P3 结束时用户工程仍运行旧生产 DLL，此前 f2 的 `not hooked` 尚未在项目中修复。后续 [P4](rgbm-hook-portability-deployment.md) 已接入加载器、缓存准备与 Worker 传播并部署；[P5](rgbm-hook-portability-acceptance.md) 已完成实际 usage、编码参数、同一插件的 f1／f2 Editor／batchmode／AssetImportWorker 及晚启动 Worker 编码矩阵。P3 的隔离副本或负向探针本身不代替这些真实验收。
