# P0 接口报告：RGBM 语义与同步回调作用域

- 日期：2026-10-09（Asia/Shanghai）
- 接口描述 ID：`unity.threaded-copy-for-each-callback.v1`
- 结论：P0 证据与方案关口完成；生产解析认证、Hook 安装和 Unity 编码验收仍待后续阶段。
- 关联：[设计](rgbm-hook-portability-design.md)、[施工记录](rgbm-hook-portability-construction.md)
- 后续进度：2026-10-10 已交付首个接口的 [P1 认证与缓存](rgbm-hook-portability-cache.md)，并在用户的 f2 Editor 内只读验证；本报告保留 P0 当时的边界。

## 1. 二进制和入口差异

两份 exe 都来自用户指定的实际安装目录。各自的 `unity_x64.pdb` 通过 exe CodeView GUID／Age 校验；版本字符串仅用于展示。所有 RVA 由同一 resolver 从匹配 PDB、调用点和导入关系取得，下表是勘探结果，不是运行时地址表。

| 项目 | 2022.3.62f1 | 2022.3.62f2 |
| --- | --- | --- |
| 实际 revision | `4af31df58517` | `7670c08855a9` |
| exe SHA256 | `E564374ACF68AD36EC7383D6B423EA3862F9280A3A1A61E34B997E694567F0DF` | `316507F85AEAEDF602966BE741E0C2C69D12D44850610FCF3A4AC0A164316BA3` |
| PDB GUID／Age | `E3C342B1-C4BD-4A96-9877-DA2A65382F6D`／1 | `2AA59BCD-876C-4043-9A63-0A513ED92101`／1 |
| `CompressSingleImageTexture` | `0x1EDC1A0`，`0xB62` 字节 | `0x1EDC250`，`0xB62` 字节 |
| BC7 `ThreadedProcessImage<JobData,8>` | `0x1ED6540`，`0x117` 字节 | `0x1ED65F0`，`0x117` 字节 |
| 调度器两参数 dispatcher | `0x1ECE7E0`，31 字节 | `0x1ECE890`，31 字节 |
| BC7 三参数 callback forwarder | `0x1ECE960`，16 字节 | `0x1ECEA10`，16 字节 |
| BC7 lambda（仅作链路证据） | `0x1ED9710` | `0x1ED97C0` |
| `ScheduleJobForEachInternal` | `0xDEE1C0` | `0xDEE210` |
| `CompleteFenceInternal` | `0xDED650` | `0xDED6A0` |
| `ClearFenceWithoutSync` | `0xDED640` | `0xDED690` |
| `JobQueue::WaitForJobGroupID` | `0xDEFDF0` | `0xDEFE40` |
| `TextureUtil_CUSTOM_IsRGBMUsageMode` | `0x76B010` | `0x76B010` |

压缩入口、BC7 process、原生 usage 查询及 BC7 回调链均只有一个有效候选。压缩入口公有符号的装饰名／解码签名两版相同；process、dispatcher、forwarder 在屏蔽直接相对调用／跳转目标和 RIP 地址位移后，完整函数体形态相同。这个比较用于证明已勘探的两份接口；P1 还须实现允许入口指令变化的语义认证，不能把这三个完整函数体哈希当作通用兼容规则。

两份 PDB 均未提供可用的私有参数／JobData 类型记录。自动生成的 lambda `()` 显示名不能当作真实零参数原型；本报告的回调参数来自完整转发体与实际调用指令。

## 2. 参数、调度和正常返回契约

压缩入口公有符号解码为：

```cpp
void CompressSingleImageTexture(
    ImageReference&, TextureFormat, TextureUsageMode, int,
    bool, bool, bool, unsigned char* const, Texture const* const);
```

Windows x64 下前四个整数／指针参数位于 RCX、RDX、R8、R9；第三个参数为原生 usage，后三个 bool 与末尾两个指针在栈上传递。输出是可写 `unsigned char*`，不能因名字推测成只读指针。除 usage 外，本阶段不为未命名参数编造业务语义。

process 的实际调用契约为：

```cpp
using Callback = void (*)(void* copied_job, int begin, int end);
void Process(int rows, const void* source_job, Callback callback);
```

RCX 是行数，RDX 是输入 JobData，R8 是回调；**第一个参数不是 texture format**。process 把输入 `0xD8` 字节完整复制到自己栈上的局部结构，附加 rows 和 callback，然后交给调度器。该复制长度和附加位置是取证事实，包装器不需要读取这些字段。

调度调用的公有符号为：

```cpp
void ScheduleJobForEachInternal(
    JobFence&, void (*)(void*, unsigned), void*, int,
    void (*)(void*), JobPriority, int, WorkStealingRange*);
void CompleteFenceInternal(JobFence&, WorkStealMode);
void ClearFenceWithoutSync(JobFence&);
```

本 process 调用的 cleanup 为 null、priority 为 0、batch 为 1、range 为 null。两参数 dispatcher 将 chunk 映射为 `begin=chunk*8`、`end=min(begin+8, rows)`，再以 `(copied_job, begin, end)` 尾调用传入的三参数 callback。BC7 forwarder 将它重排为 `(0, copied_job, begin, end)`，跳到实际 BC7 lambda；lambda 的 BC7 导入调用进一步确认编码链路。

process 的正常路径在 Schedule 后检查 fence，必要时调用 `CompleteFenceInternal(fence,0)`，然后 Clear，最后返回。Complete 的实际函数体调用公有符号 `JobQueue::WaitForJobGroupID`、记录 fence 同步并清零 fence。rows 为 0 的路径不调度任务。栈上 JobData 的生存期和上述同步等待共同支持“正常返回后可释放本次回调作用域”的结论；不能仅凭当前活跃回调数为 0 推断没有排队任务。

## 3. 原生 RGBM 语义

两版原生查询实际包含唯一的 `cmp usage,9`／越界排除、`mov eax,0x224`、`bt eax,usage`／未命中排除、真值赋值序列，确认当前接口将 2、5、9 作为 RGBM usage。该证据由匹配 PDB 的原生入口及磁盘函数体提供，不由纹理文件名、格式或资源清单推导。

后续认证必须同时核对此语义和压缩参数契约。找到地址但无法解释新的 usage 判定时，保持不可用。

## 4. 关联方案结论

原计划的“调度与回调使用同一 JobData 指针”不成立：回调收到 process 栈上的复制体。因此选用两个 Hook：压缩入口建立 usage TLS；BC7 process Hook 捕获该上下文，替换其 callback 参数，然后调用原 process。

替换 callback 是预编译静态包装函数，函数身份对应本次作用域槽。槽保存原 callback 和 `Unknown`／`Ordinary`／`RGBM` 状态；Worker 包装函数进入时保存旧 TLS，设置本次状态，原样转发三个参数，退出时恢复旧 TLS。JobData 全程作为不透明指针传递，不读取源像素地址，不按 JobData／像素地址建立映射，也不再需要 detour BC7 lambda。

正常返回并确认同步 join 后释放槽。原型使用 64 个静态槽来验证方法，容量并非生产承诺。容量耗尽不能伪造 Ordinary；P3 必须让本次路径走原生编码并处理进程可用状态。异常／未认证 join 的路径隔离槽且不复用函数身份，迟到回调保持 Unknown。隔离只解决上下文串用：它不能延长 Unity 自身栈上 JobData 的寿命，也不证明 Unity native 异常展开安全。P3 必须明确此类故障的恢复或终止边界。

两个必需入口应作为完整事务安装；dispatcher、forwarder、lambda、wait 和 usage 查询保留为认证证据，不增加 Hook 点。旧实现的三个 Hook 只是现状，后续事务数量由接口描述确定。

## 5. 隔离验证及范围

`tests/p0/test_callback_clone.cpp` 从已校验 SHA256 的实际 exe 读取 process 和 dispatcher，复制到测试进程执行，仅把三个外部调度／等待／清理调用替换为测试宿主。同一测试 exe 覆盖两版，并在执行前校验实际形态。测试宿主延迟启动 Worker 直至 Complete，再等待所有 Worker；没有启动 Unity、修改安装 exe 或执行 Unity 的 JobQueue。

两版各有 149 个任务、299 次回调，包含零行、混合状态、8 个并发调用者、嵌套普通任务、输入地址重复使用、作用域回收。299 次均核对复制后的不透明 payload、正确上下文和 Worker 旧 TLS 恢复；148 次调度／等待／清理计数一致，正常结束后槽数为 0。另验证容量耗尽拒绝、未完成作用域隔离及迟到回调 Unknown、函数身份不复用、C++ 包装器异常的 TLS 恢复；2 个异常槽保留隔离。异常测试直接调用包装器，未让异常穿过复制的 native 函数体。

错误 exe SHA、把 f1 RVA 套到 f2、输出覆盖输入均在执行前拒绝。静态证明和测试矩阵退出码均为 0。报告保留 `installable=false` 或 `unity_integration=false`；这些结果不等于生产 detour、真实调度器、Worker 编码或图像质量已验收。

证据位于忽略目录 `out/rgbm-portability/`：`p0-interface-proof.json`、`p0-tests/*-evidence.json`、`p0-tests/*-callback-clone.json`、`p0-tests/matrix-summary.json`、`f1-p0-disassembly.txt`、`f2-p0-disassembly.txt`。工具源码和本报告可入库，Unity 二进制／PDB 不入库。

## 6. DIA 部署路线

采用明确路径的私有 `msdia140.dll`，通过 `LoadLibraryEx` 的 DLL 目录／系统目录搜索及 DLL 自身 `DllGetClassObject` 创建 DIA；不使用全局 COM 类注册，也不在运行时寻找开发机 VS。resolver 使用 `/MT`，PDB 由用户本地明确提供、GUID／Age 校验，当前路线不启用符号服务器或携带 `symsrv.dll`。

本次本地暂存 DLL 来自 VS 18 Community 的 release DIA SDK，版本 `14.51.36257.0`，Microsoft 签名有效，SHA256 为 `55E1CE73E8FFD54C88A75C0FDCB11A56DF4BA0F80949CF585741855E9E4181AA`。Microsoft 的 VS 2026 可再分发列表列出 `DIA SDK\bin\amd64\msdia140.dll`，要求有效许可下按适用条款分发未修改的列出文件，preview 版本排除；最终发行须沿用适用许可条件。[Microsoft 可再分发列表及条件](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution#dia-sdk)

`tools/stage_rgbm_resolver.ps1` 已实现本地依赖暂存、原样复制、Detours MIT 许可、依赖声明和 SHA256 清单。仅含 resolver、DIA 和说明文件，不复制 Unity／PDB／Oodle。暂存副本在 PATH 仅有 System32 时，对 f1／f2 完成取证，报告核对实际加载的是包内 DLL；缺 DLL 按退出码 3 拒绝。此为已有 VS 机器上的私有加载实验，真正无 VS 的干净机器验收仍属 P4。

## 7. 后续未知项与 P0 关口

| 后续阶段 | 明确未完成项 |
| --- | --- |
| P1 | 通用寄存器／调用点 ABI 认证、受限无 PDB 扫描、运行缓存 schema 与运行时验证；当前静态证明工具只覆盖已勘探形态 |
| P2 | Unity 安装期线程创建屏障、撤销期活跃调用清退、系统恢复失败边界；线程快照不能代替宿主协调 |
| P3 | 生产作用域槽容量与故障状态处理、两点 Hook 接入、三态到原生 BC7 退化、异常路径、旧逻辑清理 |
| P4 | Editor／晚加载 Worker 的缓存准备顺序、逐进程身份验证、依赖随包发布、真正无 VS 环境 |
| P5 | 同一生产 DLL 的两版 Editor／batchmode／Worker 实际调度、usage／参数、编码输出与性能矩阵 |

P0 要求的安全基线、两版 exe／匹配 PDB、唯一入口与可解释接口差异、关联方案结论、依赖部署路线及未知项清单均已有证据。上述后续验收不作为已完成项，也不影响 P0 的勘探结论。
