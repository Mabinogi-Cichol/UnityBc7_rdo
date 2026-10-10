# Feature 施工记录：RGBM Hook 跨 Unity 小版本定位

- Feature ID：`rgbm-hook-portability`
- 建立日期：2026-10-09（Asia/Shanghai）
- 更新日期：2026-10-10（Asia/Shanghai）
- 配套文档：[设计与施工计划](rgbm-hook-portability-design.md)
- 当前状态：P0–P6 已完成，v0.5.0 已正式发布并核对线上资产；支持／质量／性能边界保留，无 VS 测试按用户要求取消
- 源码基线：`7cd4ded`，`fix: synchronize BC7 RDO config cache access`

## 1. 记录规则

本文件记录实际发生的施工、测试和决策；阶段拆分与目标行为以设计文档为准。每条记录应包含实际改动、证据路径、验证结果、未完成项和下一步。失败／中止的验证保留事实，不能改写为通过；计划中的模块名称不表示已有实现。

只有满足对应验收条件后才能将阶段标为完成。生成的 `.exe`、Unity PDB、缓存和日志只作为本地证据，不提交专有文件，也不把运行缓存作为人工版本适配表维护。

## 2. 基线状态

| 项目 | 当前状态 | 证据／说明 |
| --- | --- | --- |
| 配置缓存并发修复 | 施工时已核对安全基线 | `git diff --exit-code 7cd4ded -- src/rdo_common.h` 无差异；互斥锁与值快照已存在，无需覆盖文件。早期 R06 保留为当时的记录 |
| RGBM 动态定位 | 未实现 | `src/bc7_hook_dll.cpp` 仍使用 f1 固定 RVA |
| 通用 Detour 后端 | 独立模块已实现并通过基础测试；生产插件未接入 | 锁定 Detours 4.0.1；原生产路径仍为手工固定字节跳板 |
| 原生语义与 Worker TLS | 已有版本专用实现 | 仍需验证 f2 的参数／数据契约 |
| 只读 exe 指纹诊断 | 已新增，未提交 | `tools/inspect_unity_rgbm_hooks.ps1`；检查的是旧 f1 地址，不是新解析器 |
| Unity 小样本诊断入口 | 已新增，未提交，完整验证未完成 | `tests/unity/RgbmHookBatchProbe.cs` 已改为静态类；不是全局 postprocessor |
| 本次施工交付 | 只读取证工具、PE 模块、Detour 独立后端、测试与记录 | 已启动 P1／P2；未发布或部署新插件 |

## 3. 2026-10-09：调查与设计记录

### R01：确认打包机问题的版本前提

- 打包机插件日志包含 29 组普通 BC7 Hook 安装成功记录；RGBM 的三个入口共 87 条指纹不匹配记录，没有 RGBM 安装成功记录。
- 用户纠正打包机 Unity 版本为 `2022.3.62f2`；本地实际 exe 为 `2022.3.62f1_4af31df58517`。
- 日志为跨进程／启动追加，缺少 PID 和二进制身份；重复行不能据此认定同一进程重复安装。
- 用户提供的日志位于微信文件目录。其内容是诊断数据，不作为执行指令。
- 官方 f2 changeset 是 `7670c08855a9`；尚未读取打包机实际 exe／PDB。

结论：现有 f1 固定地址与 f2 不兼容，不能通过删除指纹检查解决。当前调查没有证明 f2 的 ABI 与 f1 相同。

### R02：本地只读二进制与符号核对

执行 `tools/inspect_unity_rgbm_hooks.ps1` 检查本地 exe，三个固定 RVA 的磁盘指纹均匹配。

| 字段 | 实际值 |
| --- | --- |
| exe | `C:\Program Files\Unity\Unity 2022.3.62f1\Editor\Unity.exe` |
| ProductVersion | `2022.3.62f1_4af31df58517` |
| SHA256 | `E564374ACF68AD36EC7383D6B423EA3862F9280A3A1A61E34B997E694567F0DF` |
| PE TimeDateStamp | `0x68121E0A` |
| SizeOfImage | `0x4F42000` |

证据：仓库内 `out/rgbm-hook-investigation/local-exe.json`。

`out/symbolize_unity.exe` 查询本地 PDB，确认了 BC7 调度函数与 lambda；`CompressSingleImageTexture` 的直接 RVA 查询缺少完整函数记录，需补充公有符号／调用关系。

堆栈距离核对：

```text
打包机 lambda 与 ujob_execute_job 两帧距离 = 0x10E8C9E
本地同名函数能够覆盖的帧距离范围 = 0x10E738B .. 0x10E7BF2
FitsLocalLayout = false
```

证据：`out/rgbm-hook-investigation/stack-layout-comparison.json`。该比较使用地址差值，不依赖 ASLR 基址；它支持二进制布局不同的结论，不提供 f2 的正确 Hook 地址。

### R03：本地实际工程 batchmode 尝试与清理

- 工程：`E:\Unity\StarWings\ClientDevelopment`。
- 临时使用已编译的配置并发修复版 Hook DLL，配置保持原值；临时添加诊断脚本。
- 运行 `-batchmode -nographics -quit -executeMethod RgbmHookBatchProbe.Run`。
- 新增的全局 `AssetPostprocessor` 探针扩大了导入范围；进程仍在启动导入阶段，未完成预定 64×64 HDR 小样本验证。
- 停止已核对身份的自建测试 Unity 进程 PID `71968`；未停止另一个 StarwardX 编辑器。
- 本次退出码为 `-1`，不能记为端到端测试通过。

实际完成的验证：本次 native 日志显示三个 RGBM Hook 均安装成功，并观察到 `TextureUsageMode=2` 的原生 RGBM 调用；完整小样本编码与 Worker 参数分流未验收。

清理核对：

```json
{
  "UnityExitCode": -1,
  "DllRestored": true,
  "ConfigUnchanged": true,
  "ProbeRemoved": true,
  "FixtureRemoved": true
}
```

证据：`out/rgbm-hook-investigation/result.json`、`unity-batch.log`、`hook-batch.log`。随后诊断源码已移除 `AssetPostprocessor` 继承和全局纹理回调，改为静态入口；修改后的探针尚未重跑。

后续测试策略：先用独立最小工程，避免修改真实工程的全局导入依赖；同一实际工程只做最终单资源验收。

### R04：f2 二进制获取状态

- 本机安装目录未发现 2022.3.62f2。
- 已核对官方发布页与 Windows 安装包 HTTP 元数据，安装包长度为 `3754403072` 字节。
- 尚未下载、提取或安装 f2，尚未取得其 exe／PDB，也没有定位出 f2 的三个入口。
- 后续可从打包机取得实际二进制，或在仓库忽略的分析目录提取官方包；不得覆盖已安装 f1 或通过改实际工程版本完成取样。

### R05：本次计划交付

- 形成设计与施工记录两个独立 Markdown 文档。
- 确定 PDB 优先、受限扫描后备、接口独立验证和通用 Detour 后端的方向。
- 写明 Job 身份关联是需证明的实验方向，不预先视为可用。
- 拟定上下文不可用时的原生 BC7 退化行为、缓存策略和主进程／Worker 验收矩阵。
- 本次仅交付计划，不把上述设计标为已经实现。

### R06：计划交付前的工作区核对

- 当前 HEAD 仍为 `7cd4ded`，但 `src/rdo_common.h` 有既有未提交改动：`watched_config` 返回值被改为共享引用，互斥锁被移除，`apply_rdo` 也改为引用读取。
- 该状态与此前恢复操作后的状态不同；本次文档任务保留源码现状，不自动覆盖修改。
- 后续实施以恢复提交中的并发安全实现为前置条件，不能把当前工作区当作已修复的运行基线。
- 本次新增的两份 Markdown 文档完成内容核对；未执行插件构建或 Unity 测试。

### R07：双版本二进制、PDB 与候选定位

用户提供了 f1／f2 的实际路径。使用本次新增的同一个 `bc7_rgbm_resolver.exe`，读取磁盘 PE 的 CodeView 并调用 `loadAndValidateDataFromPdb`，两个版本的实际 PDB 均验证通过。

| 字段 | f1 | f2 |
| --- | --- | --- |
| exe | `C:\Program Files\Unity\Unity 2022.3.62f1\Editor\Unity.exe` | `C:\Program Files\Unity\Hub\Editor\2022.3.62f2\Editor\Unity.exe` |
| ProductVersion | `2022.3.62f1_4af31df58517` | `2022.3.62f2_7670c08855a9` |
| SHA256 | `E564374ACF68AD36EC7383D6B423EA3862F9280A3A1A61E34B997E694567F0DF` | `316507F85AEAEDF602966BE741E0C2C69D12D44850610FCF3A4AC0A164316BA3` |
| PE TimeDateStamp | `0x68121E0A` | `0x68D6729D` |
| SizeOfImage | `0x4F42000` | `0x4F42000` |
| PDB GUID／Age | `E3C342B1-C4BD-4A96-9877-DA2A65382F6D` / `1` | `2AA59BCD-876C-4043-9A63-0A513ED92101` / `1` |
| PDB | exe 同目录 `unity_x64.pdb` | exe 同目录 `unity_x64.pdb` |
| CompressSingleImageTexture | `0x1EDC1A0`，长度 `0xB62` | `0x1EDC250`，长度 `0xB62` |
| ThreadedProcessImage | `0x1ED6540`，长度 `0x117` | `0x1ED65F0`，长度 `0x117` |
| BC7 lambda | `0x1ED9710`，长度 `0x202` | `0x1ED97C0`，长度 `0x202` |

这些 RVA 是工具输出的证据，不是业务代码内的新版本地址表。两版三个入口均移动 `0xB0`。

- 压缩入口通过公有符号定位；装饰名与完整解码签名两版相同，均为九个参数，第三个参数为 `TextureUsageMode`。
- ThreadedProcessImage 通过函数记录定位。公开 PDB 缺少完整类型；不能以地址正确直接宣布 ABI 已认证。
- 压缩函数通过 R8 传入的是叶转发函数，转发体重排参数后 tail-jump 到 lambda；两种调用使用共享调度实现，其中一种为 BC6。解析器继续核对 lambda 调用了 `compress_bc7e.dll!bc7elib_compress_blocks`，从而唯一选择 BC7。没有使用 lambda 哈希名。
- 叶转发体不是 `.pdata` 中的独立函数记录；不能要求所有候选都必须有 unwind 记录。带明确参数重排的完整转发体和调用关系构成候选证据，尚未构成通用自动接口认证。
- 两版 `TextureUtil_CUSTOM_IsRGBMUsageMode` 均从匹配 PDB 公有符号定位到 `0x76B010`，长度 `0x17D`；实际反汇编显示 `cmp usage,9`、`mov eax,0x224`、`bt eax,usage` 及布尔返回。这补充了 bit 2／5／9 的原生判定证据。
- 两版调度体均把输入 JobData 复制到栈上，并把复制体传给调度器，返回前等待 fence。因此**原设计中的调度／回调对象指针相同方案不成立**；下一步应验证回调包装的完整生命周期，或实现有证据的布局兼容描述。尚未选择或接入最终方案。

证据：`out/rgbm-portability/f1.json`、`f2.json`、`f1-disassembly.txt`、`f2-disassembly.txt`。exe／PDB 为用户本机安装，未复制入仓库或 release。

### R08：P1 基础施工与拒绝路径

新增 `src/rgbm_hook_bindings.{h,cpp}`、`tools/rgbm_resolver/`、`tools/build_rgbm_tools.ps1`。

- PE 只读解析校验 x64、节区边界／重叠、CodeView、函数边界与 SHA256；失败后清空旧解析状态。
- DIA 路径由明确的 `--dia` 参数提供，采用免 COM 注册加载；没有开发机 VS 的硬编码绝对路径。当前只支持本地明确存在的 DIA，不随包分发它；分发许可与干净机器部署仍未验收。
- 匹配 PDB 才开始语义查询；错 PDB／无 PDB 不提供语义依据。函数与公有符号分开记录证据质量。
- Detours 指令解码器用于调用边界取证；lambda 沿调用者、参数转发与 BC7 导入关系确定。
- 修复了取证中发现的导入表边界问题：不能把第二个 DLL 的 IAT 当成第一个 DLL 的名字表远端项。已增加双 DLL 合成回归用例。
- JSON 报告通过同目录临时文件原子发布。它明确是 `artifact_kind=evidence_report`、`installable=false`、`state=Unavailable`；不是可安装绑定缓存。
- **尚未实现**无 PDB 特征扫描、完整 ABI／语义自动认证、缓存 schema 的运行时读取与校验。P1 不标为完成。

验证：工具构建退出 `0`；合成 PE 测试退出 `0`；同一 resolver 的 f1／f2 候选提取均退出 `0`。实际错 PDB／缺失 PDB 均按约定退出 `3` 且不产生候选，输出覆盖输入的参数拒绝退出 `2`；集成测试整体退出 `0`。`0` 仅表示候选提取或对应测试完成，不表示 Hook 可安装。

日志与测试摘要：`out/rgbm-portability/build.log`、`test-bindings.log`、`test-resolver.log`、`resolver-tests/test-summary.json`。

### R09：P2 独立 Detour 后端与并发边界

- 新增 `src/native_detour_backend.{h,cpp}` 与 `tests/test_native_detour.cpp`。
- 引入未修改的 Microsoft Detours 4.0.1 源码，锁定 commit `e4bfd6b03e50de46b47abfbd1e46b384f0c5f833`。MIT 许可证保留于 `vendor/detours/LICENSE.md`；来源记录见 `UPSTREAM.md`。
- MSVC x64 静态库构建使用 `/O2 /EHsc /std:c++17 /MT`；该库尚未链接进生产 Hook DLL。
- 后端先全量校验入口与原始指针，拒绝常见未知 detour，准备全部跳板；随后枚举并登记进程已有线程，在一个事务中安装。第三个 Attach 失败会 Abort，并核对前两个入口字节与原指针恢复。
- 独立测试覆盖 RIP-relative、相对 call、短条件分支、预检失败、第三个 Attach 失败、8 个并发调用线程、32 轮安装／撤销与逐次字节恢复，最终退出 `0`。
- 首次并发撤销测试退出 `0xC0000005`。测试在仍有活跃 Hook 调用时释放跳板，存在旧跳板指针已装入寄存器的风险。改为调用结束后再撤销，并把这一宿主前提写入后端接口；后续测试通过。不能把此修改描述成“任意并发撤销已经安全”。
- 安装事务要求宿主阻止新目标调用线程的创建；撤销还要求等待所有目标／Hook／跳板调用结束。**Unity 中这两个协调条件尚未证明**，因此后端暂不接入现有注入时序。线程快照本身不提供生命周期屏障。
- 系统级 Abort／Commit 恢复失败、线程生命周期竞争及 Unity 的协调机制尚未验收。P2 不标为完成。

日志：`out/rgbm-portability/test-detour.log`、`test-detour-errors.log`。初次失败事实保留在本记录，后续日志为最终执行结果。

### R10：首轮施工最终核对

- 最终构建退出 `0`；最终合成 PE／导入表测试、实际 exe／PDB 集成测试、native Detour 测试均退出 `0`。
- 同一解析器的两版候选提取通过；错 PDB／缺失 PDB 按规定拒绝。所有报告仍明确不可安装。
- `git diff --exit-code 7cd4ded -- src/rdo_common.h` 无源码差异；没有撤回配置互斥锁／值快照。
- 导入的 Detours 16 个上游文件逐字节与锁定 commit 的下载源一致；另新增本地来源说明 `UPSTREAM.md`。
- 本地 resolver 最终 SHA256：`16F8BAF5263D604AAA53F9EECB65DC4E2DF764C4C49887D0B06B1DC05EB0E8C1`。全部施工工具和双版本报告的校验值见 `out/rgbm-portability/artifact-sha256.json`，它不是 release 清单。
- 没有启动 Unity、部署 DLL、替换安装文件或修改工程资源；本次没有发布或推送。生产 Hook DLL 与 loader 尚未接入这些新模块。
- CLI 命令、返回码与使用前提见 [取证工具说明](../../tools/rgbm_resolver/README.md)。P0／P1／P2 均仍有阶段缺口，不能将完整 feature 标为完成。

### R11：P0 完整接口与关联决策

- 扩充 resolver 报告的 process 调用点、dispatcher、BC7 forwarder、fence 依赖和公有符号签名。两版 `CompleteFenceInternal` 都调用 `JobQueue::WaitForJobGroupID`，随后记录 fence 同步并清零。
- 压缩入口完整九参数签名一致；第八个参数为可写输出指针。BC7 process 参数是 rows／输入 JobData／三参数 callback，首参不是 texture format。JobData 复制与 chunk 转发、等待／清理顺序、零行路径已记录。
- `verify_p0_contract.py` 再次校验 exe SHA、匹配 PDB、唯一候选、process／dispatcher／forwarder 的已勘探完整形态、原生 RGBM usage 位图和具名 wait 调用。两版静态证明退出 `0`。此工具是开发证明，不是通用生产 ABI 认证器。
- 最终选择压缩入口 + BC7 process 两点 Hook，通过预编译 callback 身份关联作用域，opaque JobData 原样透传，移除源像素字段读取和 lambda Hook 的需求。异常、槽容量、Unity 线程屏障等生产边界逐项归入后续阶段。
- 完整接口差异、参数／生命周期、方法与局限见 [P0 接口报告](rgbm-hook-portability-interface.md)。设计的事务要求改为“接口所需的全部入口”，首个接口为两个；独立 Detour 测试仍保留第三个 Attach 失败的通用事务案例。

证据：`out/rgbm-portability/p0-tests/f1-evidence.json`、`f2-evidence.json`、`p0-interface-proof.json`、`f1-p0-disassembly.txt`、`f2-p0-disassembly.txt`。

### R12：两版实际 native 调度体的隔离实验

- 新增 `tests/p0/callback_scope.h`、`test_callback_clone.cpp`、`tests/test_rgbm_p0.py`，构建脚本增加同一份 `test_callback_clone.exe`。
- 从两版实际 exe 提取经身份和形态校验的 process／dispatcher，复制到测试进程；只替换外部 Schedule／Complete／Clear 调用，保持实际 JobData 复制、dispatcher 参数映射及正常等待路径。没有启动 Unity 或执行其 JobQueue。
- 每版 149 个任务、299 次回调；299 次复制 payload、上下文和 Worker TLS 恢复核对一致，148 次调度／等待／清理计数一致。覆盖三态、零行、8 个并发调用者、嵌套、输入地址重复使用、join 后作用域复用；正常结束槽数为 0。
- 64 槽耗尽拒绝；未完成作用域及直接 C++ 回调异常产生的两个槽保留隔离，迟到回调 Unknown，隔离身份不复用，旧 TLS 恢复。异常测试未穿过复制的 native 函数体；隔离不解决 Unity 栈上 JobData 的异常生存期。
- 错误 SHA、f1 RVA 套到 f2、报告覆盖输入在执行前拒绝。最终矩阵整体退出 `0`，报告明确 `unity_integration=false`、`external_scheduler_substituted=true`、`production_plugin_tested=false`。

日志／报告：`out/rgbm-portability/test-p0.log`、`p0-tests/*-callback-clone.json`、`p0-tests/matrix-summary.json`。64 槽只是 P0 原型容量，不是生产支持上限。

### R13：DIA 依赖路线和私有部署实验

- resolver 改为明确路径 `LoadLibraryEx`、DLL 本身的 `DllGetClassObject`／`IClassFactory` 创建 DIA；报告记录实际 `dia_loaded_path`，对象释放后卸载模块。没有使用全局 COM 类激活／注册。
- 本地 release DIA 版本 `14.51.36257.0`、Microsoft 签名有效；SHA256 `55E1CE73E8FFD54C88A75C0FDCB11A56DF4BA0F80949CF585741855E9E4181AA`。官方 VS 2026 可再分发列表明确列出 amd64 `msdia140.dll`；最终分发受适用许可条件约束，来源及条件见接口报告。
- 新增 `tools/stage_rgbm_resolver.ps1`，只暂存 resolver、原样 DIA、Detours MIT 许可、依赖说明和 SHA 清单；没有复制 Unity／PDB／Oodle，不携带符号服务器依赖。
- 新增 `tests/test_rgbm_resolver_portable.py`。首次测试宿主因受限进程环境没有 `SystemRoot` 变量而退出 `1`，尚未执行 resolver；改为从 Windows API 获取系统目录。最终 PATH 仅 System32 的 f1／f2 私有加载均退出 `0`，报告确认 DLL 来自暂存包；缺 DLL 退出 `3`、无候选。整体退出 `0`。
- 已有 VS 机器上的私有依赖实验证明了加载路线，**不代表真正无 VS 机器已验收**；后者保留在 P4。

证据：`out/rgbm-portability/portable/dependency-manifest.json`、`portable-tests/summary.json`、`test-portable.log`。

### R14：P0 关口核对与本轮结束状态

| P0 条件 | 核对结果 |
| --- | --- |
| 安全源码基线一致 | `git diff --exit-code 7cd4ded -- src/rdo_common.h src/bc7_hook_dll.cpp src/editor_plugin/hook_loader.cs` 退出 `0` |
| 两版实际 exe／匹配 PDB | R07 身份采集，R11／R13 最终工具再次校验 |
| 入口唯一且接口证据可解释 | R11 完整签名、调用转发、复制、等待与 usage 语义；接口差异报告已交付 |
| 关联方案结论 | 原对象身份方案否定；选择同步作用域 callback 包装，两版隔离实验通过 |
| 依赖分发路线与未知项 | 私有 release DIA 路线及许可依据、暂存实验证据；P1–P5 未知项逐项列明 |

- **P0 标为完成**，表示证据、接口勘探与方案选择关口完成。P1／P2 仍在施工，完整 feature 未完成。
- 本轮最终构建、合成 PE 测试、exe／PDB 拒绝回归、双版本 callback clone 矩阵、静态证明、私有 DIA 实验退出码均为 `0`。Native Detour 后端未改动，沿用 R09／R10 的独立测试结果及宿主协调限制。
- resolver `0.2.0-evidence` SHA256：`B4478729B4147960CEA4825B070FCEE76DCA86D608D98B9C39BD1043657B9CAA`；clone 测试 exe SHA256：`C025B0C93FD6612735CDB4678E3274478FF2F75A304564423284C7A6564EECEC`。本轮清单为 `out/rgbm-portability/p0-artifact-sha256.json`；R10 的旧 hash 保留为首轮历史。
- 本轮没有启动 Unity、部署插件、改动安装目录／实际资源、发布或推送；生产 Hook DLL／loader 源码未接入。所有暂存产物、PDB 相关报告和日志留在忽略目录。

### R15：2026-10-10 / 用户 f2 工程接入前基线

- 用户授权使用 `E:\Unity\StarWings\ClientDevelopment` 接入与验证。UnityMCP 实例 `ClientDevelopment@b816e5fb3b1cec00`、主 Editor PID 41644，实际 exe 为用户提供的 f2 `7670c08855a9`。
- 通过 Editor 内进程／模块查询确认已加载项目 `Assets/Plugins/Bc7Rdo/bc7_rdo_hook.dll` 和 f2 安装的 `compress_bc7e.dll`。现有日志记录普通导出 Hook 成功，RGBM 三个固定 RVA 指纹不匹配、`not hooked`；Console 有旧 loader 主进程和 Worker 注入记录。
- 当前配置为 `encoder=oodle`、`oodle_lambda=30`、`rgbm_oodle_lambda=10`、`unity_rgbm_context_hooks=1`；保留原值。工程版本文件与 MCP 均确认 f2，未依照旧编译指南切回 f1。
- 本轮没有修改用户工程资源、配置、loader 或生产 DLL。后续只读验证直接使用已开放的 Editor，避免把重启／域重载当成安装安全屏障。

### R16：2026-10-10 / P1 首个接口认证、扫描和真实缓存

- 新增 `rgbm_hook_contract.{h,cpp}`、`rgbm_hook_profile.h`、`rgbm_binding_cache.cpp`、`bc7_rgbm_bindings.exe` 和准备脚本。扩展 resolver `--cache`，版本为 `0.3.0-evidence`，匹配 PDB 且原生契约通过才发布 `.bindings`。
- 首个描述从两个独立通过 P0 证明的报告建立，不含版本／RVA 表。DIA、受限扫描和缓存读取复用核心形态与调用语义边认证，覆盖 11 个角色。底层等待体也加入认证，wait 尾跳指向其他函数时拒绝。
- 首个接口允许地址／RIP 位移变化及最多 16 个入口前单字节 NOP；未知保存寄存器形式／编译器产物保守拒绝。完整范围与方法见 [P1 报告](rgbm-hook-portability-cache.md)，不把它称为任意 x64 ABI 的通用求解器。
- 缓存 schema 1 记录接口、完整 exe／CodeView 身份、RVA／长度／入口摘要和正文 checksum，严格解析并从当前 exe 重建契约。重算 checksum 的伪造字段不能绕过重建校验；JSON evidence report 仍不可执行。
- 扫描不使用 DIA、PDB 名称、lambda 名称或历史地址，限制在有效可执行 `.pdata` 范围，候选歧义拒绝。准备脚本在存在 PDB 时优先匹配路径，错误 PDB 硬拒绝；缺 PDB 时才选择受限扫描。
- 首次扫描退出 `0xC0000005`：普通 Detours 解码器的 CopyFF 会跟随磁盘副本里的间接内存地址。按上游源码定位原因后增加长度适配器，在私有副本中临时改为同寻址长度的 FF /0、恢复原字节，并在非法解码时拒绝；未改 Detours 上游。新增不可读指针／前缀／字节恢复回归通过。

### R17：2026-10-10 / P1 双版本与拒绝矩阵

- 最终构建退出 `0`；20 个缓存／扫描场景整体退出 `0`。同一工具的两版 PDB 路径、无 PDB 扫描和缓存复核均通过；同一 exe 的两条路线缓存字节相同。
- 错 PDB、不匹配 exe、坏 checksum／旧 schema／未知描述／重复角色、重算 checksum 的错误 RVA／长度／入口摘要、截断、未知参数传递、usage 位图变化、wait 尾跳目标变化、候选歧义均拒绝。输出覆盖输入另按退出 `2` 拒绝。
- 入口前 NOP 的非执行合成 exe 扫描通过，旧缓存因 SHA 改变拒绝。歧义夹具首轮未正确重定位复制体的外部相对引用，尚未形成第二个有效候选；修正夹具引用后得到预期拒绝。所有合成 exe 均未执行。
- 准备流程实测首次 `CacheHit=false`、后续 `true`，均 `HookInstalled=false`；暂存新增 bindings 工具。既有 PE／resolver 拒绝、P0 callback clone、私有 DIA 和新磁盘解码测试均退出 `0`。
- 证据：`out/rgbm-portability/p1-tests/summary.json`、`prepare-summary.json`、`test-cache.log`、`test-disk-decoder.log`、`build-p1.log`；所有 Unity／PDB／合成二进制与运行缓存留在忽略目录。

### R18：2026-10-10 / 用户 Editor 内缓存读取与映像一致性验证

- 新增开发专用 `tests/unity/rgbm_binding_probe.cpp`，临时 DLL 复用上述 native 缓存读取／加载映像预检，不创建线程、不改页保护、不写目标代码。
- 通过 UnityMCP 在 PID 41644 临时加载它。最终 f2 缓存的 **11 个角色完整函数体** 与实际已加载映像一致；f1 缓存以 `cache_binary_identity_mismatch` 拒绝，损坏缓存以 `cache_checksum_mismatch` 拒绝。报告均 `hook_installed=false`、`patched=false`。
- 探针 DLL 已卸载；两个临时进程环境变量恢复 null。Editor 未编译、未导入，MCP ready。没有复制或覆盖项目生产 DLL；旧 `not hooked` 基线尚未修复。
- P1 首个接口的定位／认证／扫描／缓存交付与拒绝关口通过，标为完成。P2 尚需 Unity 安装期线程协调和撤销清退／失败恢复；MCP idle 不是这个屏障，不将未经证明的 `true` 传给后端。P3／P4／P5 继续按原阶段验证。
- 证据：`out/rgbm-portability/p1-tests/live-f2.json`、`live-f1.json`、`live-bad-checksum.json`；最终二进制与报告的 SHA256 见 `out/rgbm-portability/p1-artifact-sha256.json`。本轮最终验证包含已加载 CodeView GUID／Age 复核，结束后再次确认探针已卸载、环境变量已恢复、Editor 未编译或更新。生产 `bc7_hook_dll.cpp`、loader 和并发配置基线相对 `7cd4ded` 仍无差异。

### R19：2026-10-10 / P2 安装协调、生命周期和恢复状态

- 新增 `detour_coordinator`、无 CRT `detour_loader_gate`、专用分配器与事务 API 适配。协调器通过真实 DLL 初始化回调建立新线程执行约束，而不是将 MCP idle 转成布尔承诺。既有线程全量收集、登记及控制上下文预检；访问失败、容量不足、错槽／入口／批次均拒绝。
- 保留原样上游 Detours 4.0.1。事务只用协调 DLL 的 VirtualAlloc 分配器，避免暂停后依赖 CRT 堆锁；记录上游忽略的页保护、上下文和恢复线程错误。正常安装／撤销也复核页面保护。
- Unity 采用驻留至进程退出的生命周期；模块引用由协调器持有，原槽和生成代码仍由宿主保证寿命。合作宿主能证明所有调用停止时才可撤销；没有把 Hook 活跃计数为零当作清退证明。
- 恢复错误为 sticky `Faulted`，保留引用并禁止后续事务；非零 error 与 recovery_error 都必须被调用者处理。自查修正撤销失败的复核基准：完整恢复到此前已安装状态，不能误报为已恢复原入口或 Unavailable。
- 构建与接口使用见 [P2 报告](rgbm-hook-portability-detours.md)。正式协调器不导出测试故障 API；gate 只有协调器 commit 导入，上游源文件未修改。

### R20：2026-10-10 / P2 独立矩阵与当前 f2 Editor 验证

- 正式和测试构建、四个独立测试进程最终真实退出码均 0。两个安装者竞争只有一个成功；native 矩阵 32 轮，8 个持续调用线程、持续新建线程、32 个快照后确定性线程创建均通过；RIP-relative、相对 call、短条件分支结果正确。
- 失败路径包括全量预检、第三个 Attach、暂停后的节点分配、控制上下文检查、容量及缺失 gate；撤销第二个 Detach 分配失败保持完整安装，随后成功移除。可恢复失败的代码、槽、保护复核通过；恢复页保护失败和线程恢复错误报告均为 Faulted。后者是实际 ResumeThread 后的报告注入，不能称为真实 OS 恢复失败复现。
- 通过 UnityMCP 临时加载正式协调器及开发探针，在 PID 41644、实际 `2022.3.62f2` 的 ClientDevelopment 完成 8 轮安装／撤销。探针仅修改自己生成的代码，Unity 入口未修改；最终报告 passed=true、bad_calls=0、code_restored=true、hooks_remaining=false，实际计数见 JSON。
- 环境变量恢复 null，协调器／gate／探针模块均卸载，Editor 未编译、未更新。项目插件、配置、Unity 安装目录和资源未修改；生产源码相对 `7cd4ded` 仍无差异，旧 `not hooked` 尚待运行时接入修复。
- 证据：`out/rgbm-portability/coordinator/test/summary.json`、`live-owned-code-f2.json`、`cleanup-f2.json`、`artifact-sha256.json`；构建／测试日志 `build-coordinator-test.log`、`test-coordinator.log`、`build-coordinator.log`。MCP 驱动保存为 `tests/unity/run_detour_coordinator_probe.csx`。
- P2 后端关口通过。生产故障退出策略、两点语义 Hook 和实际 Job／编码分流仍按 P3／P4／P5 验收，不以自有代码测试代替功能完成。

### R21：2026-10-10 / P3 两点 Hook 与三态编码接入

- 新增 `rgbm_hook_runtime.{h,cpp}` 与 `rgbm_hook_install.cpp`，当前 exe／缓存／全部 11 个加载入口复核后，使用 P2 协调器完整安装 Compress 和 Process 两个 Hook。初始化与状态提供 C 导出，P4 负责准备缓存与逐进程调用；主 DLL 动态加载同目录协调器，缺失依赖保持原生编码。
- 九参数压缩入口保持可写输出，Process 首参 rows、不透明 JobData、64 个静态回调槽、Worker 旧 TLS 保存／恢复。只在认证的正常同步 join 后释放槽；容量耗尽 sticky Unavailable，未知上下文不进入 Oodle／ERT。
- 移除主 Hook 中固定 RVA、私有入口手工 prologue 跳板、像素标记容器、JobData 偏移和 lambda Hook。普通 BC7 导出桩与既有配置缓存并发修复保留。
- 压缩入口和包装回调使用 finally 处理 C++／SEH 展开；Process 异常退出、join 契约破坏或协调器 Faulted 时非零终止进程，不用槽隔离掩盖 JobData 栈寿命问题。退出码与阶段边界见 [P3 报告](rgbm-hook-portability-runtime.md)。
- 构建脚本接入运行时、P1 认证器、正式协调器和 gate，开发产物暂存于 ignored out。未部署用户工程、修改 Unity 安装目录或提交／发布。

### R22：2026-10-10 / P3 原生矩阵、编码回归及 f2 拒绝路径

- 正式生产 DLL 编译成功。16 个并发调用者、地址复用与嵌套普通任务共 2,561 次回调，九参数转发、可写输出、复制 payload、TLS 恢复、正常槽回收、三态／lambda 决策、容量耗尽退化通过。
- 同一生产运行时测试 exe 对 f1／f2 实际 process／dispatcher 隔离副本各执行 149 个任务、299 次回调；上下文、复制 payload、TLS 恢复各 299 次一致，148 次 schedule／complete／clear 一致，正常槽数 0。外部 scheduler 由测试宿主代替，不等于真实 Unity JobQueue 验收。
- 独立故障子进程验证 C++／SEH 未 join 异常退出及活跃回调下提前返回的 join 破坏，分别为 `0xE0427001`／`0xE0427002`。C++ 设置外层捕获点以触发真正的异常展开，未把 CRT 直接 abort 当作通过。
- 新生产 DLL 的 `test_hook.py` 通过原生转发、Oodle 零 lambda、ERT 生效；Unknown 在 Oodle／ERT 非零 lambda 下均与原生 BC7 逐字节相同。既有 64 线程配置首次读取回归重复 120 个独立进程，全部真实退出 0。
- 当前 f2 PID 41644 内的临时探针验证 11 个绑定后使用确认不存在的依赖目录，得到 Unavailable、`coordinator_load_failed:126`、原生退化决策正确且 Unity 入口未变。探针／协调器未驻留，环境恢复，Editor 未编译／更新。保存 MCP 驱动，未向现有旧 Hook 叠加新的私有 Hook。
- 证据位于 `out/rgbm-portability/runtime`：`summary.json`、`clone-f1.json`、`clone-f2.json`、`live-unavailable-f2.json`、`cleanup-f2.json`、`artifact-sha256.json`、构建及测试日志。P3 阶段通过；用户项目中的旧 `not hooked` 尚待 P4 部署／重启及 P5 实际编码验收。

### R23：2026-10-10 / P4 缓存、每进程初始化及实际工程部署

- 新增 managed 缓存准备、native 命令行引导与逐进程原子报告；按实际 exe SHA256 区分缓存，主进程准备后加载，Worker 仅读取并重新认证。准备失败保持原生退化，未知二进制不能沿用父进程地址。
- 暂存包内 resolver／认证器／release DIA 到 Unity 忽略的 `RgbmBindings~`；主 Hook、协调器及 gate 限 Windows x64 Editor-only、非 preloaded。注入按 PID＋创建时间、真实 exe 和模块路径确认，远端 LoadLibrary 地址独立解析，超时保留正在使用的远端路径。
- 用户关闭工程并授权 batchmode 后部署到 ClientDevelopment，旧插件保存在 `out/rgbm-portability/deployment/installed-before-20261010-151346`。配置 SHA256 保持 `A446D940E9806C2F0058B603AB7564F736A4898873FDCB4609E8305C362AF1F6`；保留既有 Oodle 依赖，不修改 Unity 安装目录。
- 首轮返回 1：原函数槽未初始化，P2 按契约拒绝 error 13。修复 P3 接入错误后主进程 PID 54180 状态 Ready、返回 0；失败报告保留，未关闭认证或线程覆盖。
- 并行导入首轮真实 Worker 使用 `-adb2 -name AssetImportWorkerN -srvPort`，初版只检查另一种标记，未注入；保留这轮主进程成功／Worker 未通过的事实。识别修复后 PID 49468 主进程和 PID 2596、47368 两个实际 Worker 均 Ready，存活进程创建 FILETIME 一致，返回 0。
- 故意破坏本插件生成的缓存后，managed 准备自动重新认证并恢复；恢复缓存与备份 SHA256 同为 `E5B965FD83BF3589B1DFD47892FF9B25EF96D1C0D5C24B866AAF1E570F61D19B`。系统目录 PATH 下运行，新 DLL 的 `test_hook.py` 返回 0。此机器装有 VS，不能把这些测试称为真正无 VS 验收。
- 独立最小 f2 工程主进程 PID 8224 及 Worker PID 45632、50908 同样 Ready、返回 0。两个工程的 BC7 均提前加载；另用生产 DLL 的独立宿主 `test_hook.py --late-module` 验证先 Hook／后 BC7 顺序，返回 0。后者为 Disabled 引导与普通导出 Hook 验证，不等于 Unity 内语义 Hook Ready 的严格晚加载场景。
- 完整加载流程、批次报告、晚加载／清理边界见 [P4 部署记录](rgbm-hook-portability-deployment.md)。本阶段没有提交或发布；Ready 不替代 P5 实际编码矩阵。
- 最终清理临时验收脚本／meta 和测试资源，再以正式插件 f2 batchmode 编译，返回 0，PID 53856 Ready；所有 Unity 进程已退出。配置未变、五个已安装代码／二进制文件与暂存 SHA256 一致，`summary.json` 和 `artifact-sha256.json` 已落盘。

### R24：2026-10-10 / 用户调整部署验收范围

- 用户明确打包机安装 VS，不需要无 VS 环境测试。取消该验收项，不再作为 P4 或 feature 完成的阻塞项；历史记录中的待测项属于当时范围，以本次调整为准。
- Hook／进程注入本身使用 Windows API，不依赖 VS 安装。主 DLL 和协调器采用 `/MT`；现有 PE 导入检查显示主 DLL 只静态依赖系统 `KERNEL32`／`SHELL32`／`bcrypt`。
- PDB 解析通过包内 release DIA 的 `DllGetClassObject`，不查找 VS 目录或依赖全局 COM 注册。保留当前依赖暂存与认证方案，取消测试不需要修改二进制或工程。
- 同步设计、部署文档和当前验收 summary；真实测试结果保留，没有将未执行的无 VS 测试改写为通过。

### R25：2026-10-10 / 按原设计核对 P4 完成范围

- 原设计 P4 要求缓存准备顺序、晚加载 Worker、DIA 依赖和包内容检查。实际 f2 主进程 Ready 后，由测试启动的两个真实 Worker 均独立认证并 Ready；两工程、损坏缓存恢复、导入设置、最终编译及清理已有证据。
- R24 已按用户要求取消无 VS 环境测试，调整后的 P4 关口通过。
- 此前将“Unity 内语义 Hook Ready 先于 BC7 DLL 加载”额外场景列为 P4 阻塞，扩大了原设计的验收范围。本次更正阶段状态；不改变或补写测试结果，该场景仍作为未实测的覆盖边界保留。
- P4 标为已完成；f1／f2 Editor／batchmode／Worker 的实际 RGBM／普通编码矩阵仍由 P5 完成，不以 P4 安装成功替代编码验收。

### R26：2026-10-10 / P5 驱动、跨版本及真实编码矩阵

- 新增临时 `tests/unity/RgbmP5Validation.cs`、启动工具 `tools/run_rgbm_p5.ps1` 和完整证据检查 `tests/verify_rgbm_p5.py`。没有新增 AssetPostprocessor；隔离工程使用 6 对普通 PNG／真实 HDR 副本，用户工程只使用 1 对，每张 64×64 BC7、含全部 mip。
- 为实际事件增加 PID／TID、ERT context、metric、λ、启用状态、smooth_scale 和 max_rms_ratio。P5 未改变编码算法、配置缓存、接口认证或上下文方案。
- 先拒绝错误的 RefreshImportMode 枚举／Lightmap API 名称、默认 High 产生 FullHDR、父进程未保存的 Normal 设置及错误编码路由。启动前保存隔离设置，以独立进程运行主路径／Worker 路径；普通 Editor 等到两 Worker idle、connecting=0 后才提交队列。
- 同一生产快照在 f1／f2 的普通 Editor、batchmode、真实 AssetImportWorker，以及无 Oodle DLL 的两版回退工程运行。实际 usage=2；Oodle 30／10、零 λ、-1 继承、ERT 4／0.25、RGBM metric 与额外参数、Oodle 缺失及 Unknown 原生回退均逐条断言。

### R27：2026-10-10 / P5 共享日志竞争修复与完整复测

- 第一轮 14 次 Unity 运行的参数和输出检查通过，采集 19,717 条事件；汇总进一步核对块数时发现 CRT 共享追加日志在不同 Worker 中发生 seek／write 竞争，部分事件缺失。第一轮报告、日志和 DLL 保存为 `out/rgbm-portability/acceptance/before-append-fix`，没有当作最终完整计数。
- `hook_log` 改为整行构造、`FILE_APPEND_DATA` 与单次 `WriteFile`，保留进程内线程互斥。重新构建并冻结最终插件，主 DLL SHA256 为 `C25C24F72BFA1F8BE0A8D840CE5C23E7DBF8CFDF3A8C501CE82022FA26F403AB`。
- 最终 14 次 Unity 运行全部真实退出 0：110 组参数／路径、1,140 份 BC7 输出、20,520 次编码调用、391,020 个块。每组日志块数等于实际输出，570 组主进程／Worker 输出完全相同，包括非零 RDO。每个 Worker 验收都由两个实际 Worker 编码，并独立复核 exe、PID／创建时间和 Ready。
- 最终 DLL 的既有原生立即／晚加载回归分别退出 0；配置首次读取回归重复 120 个独立进程、每进程 64 线程，全部退出 0，耗时 21.993 秒。完整原生／Unity 回归均使用修复后的最终 DLL。

### R28：2026-10-10 / P5 用户工程恢复与阶段完成

- `tools/run_rgbm_p5_user.ps1` 在 ClientDevelopment 关闭时完整备份生产文件、部署最终快照并执行主进程／Worker 各 9 组单资源验收。最终复测前备份为 `user-before-20261010-173212`，原 P4 插件另保存在 `p4-plugin-before-p5`。
- 配置 SHA256 仍为 `A446D940E9806C2F0058B603AB7564F736A4898873FDCB4609E8305C362AF1F6`；配置时间恢复，原 HDR、meta、EditorSettings 和 ProjectSettings 哈希均未变。测试副本、驱动及其 meta 全部删除。
- 正式插件最终 f2 编译 PID 56324，exit 0，native Ready、hook_installed=true。所有本次验收进程关闭；用户另外打开的 StarwardX PID 27876 保持运行。
- [P5 验收报告](rgbm-hook-portability-acceptance.md) 记录精确 PID、参数、输出与耗时边界；本地 `summary.json` 和 `artifact-sha256.json` 已生成。P5 已完成，P6 待整理使用说明、交付和发行内容，没有提交或发布。
- 解码误差为相对原生 BC7 的差异，不代表源 HDR 或视觉质量验收；阶段耗时含导入／证据采集，未给出纯 Hook 性能保证。额外 Unity 内先 Ready 后首次加载 BC7 场景仍未实测，不扩大为 P5 阻塞。

### R29：2026-10-10 / C# 合并与 DLL 后续 TODO

- 按用户要求，将独立 `rgbm_bindings_loader.cs` 的 `RgbmBindingsLoader` 合入 `src/editor_plugin/hook_loader.cs`，保留类名、调用接口及缓存／认证逻辑；删除独立源文件。`Process` 类型使用完整命名空间，避免与 Unity 的 `Debug` 命名空间冲突。
- `tools/build.ps1` 和 `tools/deploy_rgbm_portability.ps1` 改为只分发一个生产 C# 脚本，并清理旧 helper 及其 `.meta`；更新插件说明和升级步骤。DLL、解析 EXE、DIA 均未重建或合并。
- 合并脚本在全新隔离工程通过 f1／f2 主进程和真实 Worker 共 4 次 batchmode 编码验收，全部 exit 0。主进程 PID 为 f1 26464／f2 65376，Worker 验收主 PID 为 f1 42668／f2 5664，每轮由两个实际 Worker 编码。36 组参数／路径、432 份输出、7,776 次调用、148,176 个块，日志块覆盖与实际输出一致；216 组同版主进程／Worker 输出完全相同，运行时报告均 Ready。使用既有 P5 的普通／RGBM、Oodle 30／10、零 λ、继承、ERT、metric 与额外参数、Unknown 原生回退断言。未重跑完整 14 轮 P5、Oodle 缺失和原生并发矩阵，因为原生字节未变。
- 初次隔离 f1 验证因暂存默认配置 `unity_rgbm_context_hooks=0` 被驱动拒绝，已保存 `disabled-setup-rejected.json`；仅修改隔离工程测试配置后通过，未将其计为通过轮次。
- 实际 ClientDevelopment 工程关闭时部署，升级前完整插件备份为 `out/rgbm-portability/deployment/installed-before-20261010-183858`。旧 helper／meta 已删除，原配置、HDR／meta、EditorSettings、ProjectSettings 哈希均未变，测试驱动和资源未残留。部署后的 f2 编译 PID 66308 exit 0；最终注释同步后 PID 46408 exit 0，匹配 PID／创建时间的报告 Ready、hook_installed=true。源码、暂存与已安装 loader SHA256 均为 `6042A546CA90056B61844EA381C04F734ADFB3C10E99B8703AF44693A369B0AE`。
- 已安装及暂存的 8 份原生 DLL／EXE 与 P5 冻结快照逐文件哈希相同，主 DLL 仍为 `C25C24F72BFA1F8BE0A8D840CE5C23E7DBF8CFDF3A8C501CE82022FA26F403AB`。本次进程均已退出，用户另外打开的 StarwardX PID 27876 保持运行。
- 新证据独立保存在 `out/rgbm-portability/loader-merge`：`matrix-summary.json`、`matrix-artifact-sha256.json`、`deployment-verification.json`、最终编译与运行时报告。保留原 P5 双脚本历史快照和验收记录，未覆盖其身份。
- DLL 合并另登记为 [TODO 设计](rgbm-hook-dll-consolidation-design.md)／[施工记录](rgbm-hook-dll-consolidation-construction.md)，尚未开始；拟将 coordinator 并入主 DLL，保留独立 gate 与绑定工具。此次未提交、打包或发布，P6 状态不变。

### R30：2026-10-10 / P6 发行整理启动

- 用户授权开始 P6 发布，版本确定为 v0.5.0；Git 按功能模块分批提交，使用英文 conventional prefix 与中文正文。依赖／安装后端、绑定认证、运行时、编辑器、验收及 DLL 合并 TODO 已独立提交。
- 发行脚本补齐 coordinator／gate、绑定 resolver／认证器、未修改的 release DIA，按文件白名单组包；两个包均加入 bc7enc_rdo 许可，插件包含 Detours 许可和 DIA 分发说明。递归 SHA256 覆盖工具子目录；版本和清理目录边界先校验。新增 `tools/verify_release.py` 检查实际 ZIP 的严格文件集合、x64 PE、全部递归校验与依赖清单。
- 更新插件说明、构建说明、RGBM 当前注入流程、主 README 和中文发行说明，明确默认关闭语义 Hook、启用／升级／卸载方式、Ready 判定、适配边界、质量与性能实测范围。绑定工具依赖清单不再携带开发机 DIA 绝对路径。
- 实际工程回归入口默认使用当前构建插件，可用 `-PluginSource` 指定冻结快照；不再默认将历史双脚本 P5 快照覆盖到已合并版本。失败恢复兼容目标文件此前不存在的情况。这一验证包装脚本调整只做语法检查，未重新运行用户工程完整矩阵。
- 将从本次已提交的干净源码快照重新构建，校验发行包及原生回归，并验证解压后插件的跨版本主进程／Worker 编码。远端 main 基线为 `7cd4ded8bcd15ebe94ced9803c6ab9df0ab4db8c`，v0.5.0 尚不存在。尚未构建／推送／发布，最终结果另记 R31。

### R31：2026-10-10 / P6 构建、复测与正式发布完成

- 从干净提交 `689cdb8eb99d8613a52be12af0192bbc93ab1688` 独立 archive 快照构建 v0.5.0 两个发行包，构建实际 exit 0；主 DLL SHA256 为 `B62CFC531FCCF51D1AA2E76BBB89445EF07552F129E78F7FEE32B540F374AAF3`，不沿用旧 P5 二进制的计数身份。
- 实际 ZIP 白名单、递归 SHA256、x64 PE、完整依赖与许可通过；release DIA `14.51.36257.0` 原样 SHA 与 Valid 签名复核。包内无 Unity／Oodle 二进制、测试资源、运行缓存或本机 DIA 绝对路径。
- 发行 ZIP 解压后完成 f1／f2 主进程、真实 Worker及 Oodle 缺失回退共 8 轮 batchmode，全部退出 0。56 组参数／路径、672 份输出、12,096 次调用、230,496 个块完整覆盖；336 组主进程／Worker 输出一致。原生壳／立即及晚加载／绑定／Detour／运行时／协调器回归、20 个缓存扫描场景与 120×64 线程首次读取配置回归通过。
- 验证包装问题为缺少已有 Capstone 路径及只读本地 Oodle 清理，分别补齐任务环境和清理方式后完成；原始失败日志保留，未伪装成通过。所有验收 Unity 退出，用户原有 StarwardX PID 27876 保留；未替换实际工程生产 DLL。
- Git 使用英文前缀／中文正文按模块提交；公开推送首次被自动审批拒绝，用户随后明确授权具体仓库、main、v0.5.0 标签与 Release 后完成推送。标签指向真实构建提交，正式 Release 非 draft／prerelease。
- [线上 v0.5.0](https://github.com/Mabinogi-Cichol/UnityBc7_rdo/releases/tag/v0.5.0) 的中文说明与本地一致，重新下载两个 ZIP、SHA256 清单及验收 JSON，全资产大小、字节哈希与 GitHub digest 一致。[P6 报告](rgbm-hook-portability-release.md) 保留精确包身份与计数，本地证据在 `out/p6/v0.5.0`。
- P6 与本 feature 已完成，后续 DLL 合并仍为独立 TODO。最终文档提交在发布后补充证据，不改变或移动发行标签／二进制。

## 4. 阶段追踪

| 阶段 | 状态 | 已有证据 | 尚需完成 |
| --- | --- | --- | --- |
| P0：证据和接口勘探 | 已完成 | R07、R11–R14：安全基线；两版完整接口／语义／等待证据；两点 Hook 方案与隔离验证；私有 DIA 路线 | 后续运行时验收按 P1–P5 跟踪，不计入已通过范围 |
| P1：只读解析与缓存 | 首个接口已完成 | R16–R18：统一认证、扫描、严格缓存／复核；20 场景与实际 f2 映像验证 | 新编译形态需新证据／描述；生产 loader 接入属于 P4 |
| P2：通用 Detour 后端 | 已完成 | R19–R20：真实安装 gate、专用分配、模块寿命；32 轮 native 与 8 轮当前 f2 Editor 自有代码事务；失败恢复和 Faulted 状态 | 两点 Unity Hook 的生产接入属于 P3／P4，不支持无清退保证的热卸载 |
| P3：RGBM 运行时 | 已完成 | R21–R22：两点 Hook、三态／64 槽／未知原生退化、明确故障退出；并发／实际代码副本／编码和配置回归；当前 f2 缺失依赖拒绝路径 | 生产加载器／Worker 接入与实际 Unity 调度／编码按 P4／P5 |
| P4：Editor／Worker 与部署 | 已完成（按用户调整后的范围） | R23：包内依赖、自动缓存与恢复、逐进程认证；两个实际工程主进程及各两个晚启动 Worker Ready；独立宿主晚加载；R24 取消无 VS 测试；R25 范围核对 | 跨版本实际编码由 P5 验收；额外 Unity 内先 Ready 后加载 BC7 场景未实测，不作为 P4 关口 |
| P5：跨版本验收 | 已完成 | R26–R28：同一最终插件 14 次 Unity 运行、完整块覆盖、1,140 份输出、20,520 次编码调用；实际工程恢复及 120×64 线程回归 | 支持与质量／性能边界见 P5 报告，交付按 P6 |
| P6：交付 | 已完成 | R30–R31：分模块提交、干净源码构建、两包／递归 SHA、8 轮新二进制验证及线上下载核对 | 后续原生 DLL 合并属于独立 TODO |

## 5. 下一步可执行任务

1. 本 feature 无待施工关口，v0.5.0 已正式发布并核对线上产物。
2. 后续原生 DLL 合并按独立 TODO feature 启动；当前三个原生 DLL 不能直接删减。
3. 后续支持新 Unity 构建或开展源 HDR 视觉质量／纯 Hook 性能验收时，单独记录证据，不扩张当前实测结论。

P5 已完成实际编码及用户工程恢复，P6 已完成独立发行构建／验证及 v0.5.0 线上发布核对。无 VS 环境测试已取消；整个 feature 按调整后的范围完成，质量／性能及适配边界保留。

## 6. 后续记录模板

```markdown
### YYYY-MM-DD：P? / 记录编号

- 工作范围与关联设计段落：
- 实际修改文件／提交：
- 二进制、依赖版本与构建参数：
- 执行命令／环境：
- 真实退出码与验收结果：
- 报告、日志、产物 SHA256：
- 已知不足／未执行验证：
- 清理、恢复与缓存状态：
- 是否满足阶段关口，下一步：
```

## 7. Feature 完成核对

- [x] f1／f2 二进制与匹配 PDB 已采集，接口形态与语义有证据（P0）。
- [x] 同一插件无需人工填写版本专用 RVA 即可定位两版本入口（P5）。
- [x] PDB 主路径与扫描后备路径都有通过及拒绝测试（首个接口，P1）。
- [x] 通用跳板、全量事务和撤销边界通过验证（P2，驻留安装与合作宿主撤销；严重恢复错误显式 Faulted）。
- [x] 固定 Job 字段偏移已移除，运行时不读取 JobData 字段（P3）。
- [x] 分类不可用时的原生 BC7 退化已实现，通过生产编码和运行时拒绝测试（P3）。
- [x] 主 Editor、batchmode、AssetImportWorker 与晚启动 Worker 矩阵通过（P5）。
- [x] RGBM／普通参数分流、零 lambda 和既有并发修复回归通过（P5）。
- [x] 依赖包内容与缓存恢复检查通过（P4）；无 VS 环境测试按用户要求取消（R24）。
- [x] 使用文档、实测报告、支持范围和正式发行产物已更新，线上下载 SHA256 已核对（P6）。
