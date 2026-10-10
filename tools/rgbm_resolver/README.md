# RGBM 跨版本定位与绑定工具

v0.5.0 已通过 `hook_loader.cs` 接入主进程和 Worker。以下“不安装”的边界指只传 `--report` 的取证模式；插件同时传 `--cache`，只有完整接口认证通过才发布运行时绑定。运行时认证器 `bc7_rgbm_bindings.exe` 复核已有缓存；加载流程与支持范围见[插件说明](../../src/editor_plugin/README.md)。

当前工具只读 exe／PDB，生成 `evidence_report`，用于 P0/P1 接口勘探。报告明确包含
`installable=false`、`state=Unavailable`，**不是运行时绑定缓存**。退出码 `0` 只表示
成功提取候选；不能以此安装 Unity 私有 Hook，也不表示整个 feature 验收完成。

## 构建与取证

在仓库根目录运行 `tools/build_rgbm_tools.ps1`，产物位于忽略目录
`out/rgbm-portability/`。需要 MSVC x64 和 DIA SDK 开发头文件／库；采用 `/MT`。
Detours 静态库来自 `vendor/detours/UPSTREAM.md` 锁定的 commit。

```powershell
New-Item -ItemType Directory -Force out | Out-Null
& tools/build_rgbm_tools.ps1 *> out/rgbm-build.log
& out/rgbm-portability/bc7_rgbm_resolver.exe `
  --exe '<实际 Unity.exe>' --pdb '<实际 unity_x64.pdb>' `
  --dia '<本机明确可用的 msdia140.dll>' --report '<输出 JSON>'
```

DIA 运行路径是必填参数，通过私有 DLL 的 `DllGetClassObject` 创建对象。没有开发机 VS
绝对路径的运行时依赖或全局 COM 类注册。`tools/stage_rgbm_resolver.ps1 -DiaDll <路径>`
可原样暂存明确提供的 release DIA 和依赖声明；[适用许可及验证范围](../../docs/features/rgbm-hook-portability-interface.md#6-dia-部署路线)
已记录。Editor 自动调用及 f1／f2 编码验收已完成；无 VS 机器验证按用户要求取消。

定位先校验 exe 的 x64 PE、SHA256、CodeView GUID／Age，再通过
`loadAndValidateDataFromPdb` 验证 PDB。分别查询函数、公有符号并记录类型证据质量。
`CompressSingleImageTexture` 公有符号与 `ThreadedProcessImage<…BC7…JobData,8>`
函数记录通过 `.pdata` 核对边界；回调沿调用点的 R8 地址、具有明确参数重排的叶转发体、
以及 `compress_bc7e.dll!bc7elib_compress_blocks` 导入调用定位，不依赖 lambda 哈希名。
该调用关系提取只构成候选证据；尚未完成寄存器数据流的自动证明。

默认 evidence 模式仍不生成可执行绑定。P1 增加 `--cache <path>`：匹配 PDB、精确公有签名
及首个接口的 native 核心／语义边认证通过才发布 `.bindings`。`bc7_rgbm_bindings.exe`
提供 `--mode scan` 的受限无 PDB 后备及 `--mode verify` 的缓存复核，均保留
`hook_installed=false`。完整范围、格式、调用命令和实测见 [P1 报告](../../docs/features/rgbm-hook-portability-cache.md)。

错误 PDB、候选歧义、未知 ABI／usage、损坏缓存以退出码 `3` 拒绝；未知形态不默认认证。
参数错误退出 `2`，报告写入失败退出 `1`。发布使用同目录临时文件与原子替换。

## 独立验证

```powershell
& out/rgbm-portability/test_rgbm_bindings.exe
& out/rgbm-portability/test_native_detour.exe
python tests/test_rgbm_resolver.py --resolver '<resolver.exe>' --dia '<msdia140.dll>' `
  --f1 '<f1 Editor 目录>' --f2 '<f2 Editor 目录>' --out '<测试输出目录>'
```

`disassemble_report.py` 为开发取证辅助工具，依赖开发环境的 `capstone`；不进入插件包。
输入候选报告后再次校验 exe SHA256，只反汇编报告中动态找到的函数范围。

P0 新增以下开发验证，均不进入插件热路径或发布包：

```powershell
python tests/test_rgbm_p0.py --resolver '<resolver.exe>' --clone '<test_callback_clone.exe>' `
  --dia '<msdia140.dll>' --f1 '<f1 Editor 目录>' --f2 '<f2 Editor 目录>' --out '<输出目录>'
python tools/rgbm_resolver/verify_p0_contract.py --f1 '<f1-evidence.json>' `
  --f2 '<f2-evidence.json>' --out '<静态证明.json>'
python tests/test_rgbm_resolver_portable.py --package '<暂存目录>' --source-dia '<原DLL>' `
  --f1 '<f1 Editor 目录>' --f2 '<f2 Editor 目录>' --out '<输出目录>'
```

静态证明依赖开发环境的 Capstone，核对已勘探的完整函数体形态和原生 usage 判定，
不是允许任意新 prologue 的生产认证器。clone 测试执行经身份校验的实际 process／dispatcher
隔离副本，外部调度器为测试替身；未启动 Unity。回调包装原型位于 `tests/p0/`，三态 TLS、
正常 join 后回收、异常隔离和容量拒绝都属于方法验证，生产接入仍在 P3。

## Detour 后端的使用前提

`native_detour_backend` 尚未接入生产 Hook DLL。安装前必须由宿主确保事务期间不会创建
可调用目标的新线程；后端枚举、登记已有线程，批量准备全部跳板并提交事务。
**撤销还必须等待所有目标／Hook／跳板调用结束**：线程挂起不能修复已装入寄存器但
尚未调用的旧跳板指针。当前独立测试的宿主遵守这两个条件。

独立测试覆盖 RIP-relative、相对 call、短条件分支、全量预检、第三个 Attach 失败的
事务撤销、八个并发调用线程和 32 轮安装／撤销。它不证明 Unity 的线程创建协调条件
已满足，也不涵盖人为模拟的系统撤销失败；这些仍是 P2/P4 的验收缺口。
