# P4：Editor／Worker 缓存与部署

更新：2026-10-10。本文记录当前施工产物，不代表已发布支持范围。

状态：P4 已完成，按用户要求取消无 VS 环境测试。原设计要求的实际主进程／晚启动 Worker、缓存和依赖部署已通过。

## 加载流程

主 Editor 在加载主 Hook DLL 前，按实际 `Unity.exe` 的 SHA256 准备
`Library/Bc7Rdo/HookBindings/<SHA256>.v1.bindings`。命名 mutex 串行化同一工程／映像的发布。
已有缓存仍用包内认证器完整复核；拒绝后重新解析。有本地 `unity_x64.pdb` 时显式加载
`RgbmBindings~/msdia140.dll`，PDB 或接口认证失败直接拒绝，缺少 PDB 才进入有限扫描。
运行时不使用开发机 VS 路径或 COM 注册；主进程不把 evidence JSON 当作绑定缓存。

native 初始化在 `DllMain` 之外运行，先核对当前 exe、缓存及 11 个加载入口，再原子安装两个语义 Hook。
它不依赖 `compress_bc7e.dll` 已经加载。普通 BC7 导出 Hook 独立等待该模块。
每个进程用自己的命令行工程路径和 exe 身份定位缓存，不依赖父进程预先设置的环境变量。

Worker 监控只接纳当前 Editor 的直接子进程，并校验其工程路径和 Worker 命令行。
f2 实际参数为 `-adb2 -name AssetImportWorkerN -srvPort <port>`；同时兼容显式
`-assetImportWorker` 标记。读取 Windows 进程命令行信息失败时跳过，不能把普通 Unity 子进程当作 Worker。
父进程查询 Worker 的真实 exe 并先准备其缓存，Worker native 层仅读取并独立认证。

注入记录按 PID 与创建 FILETIME 区分，目标 `LoadLibraryW` 地址按其所属模块在远端重新定位。
成功必须核对原生初始化报告的 PID、创建时间、exe SHA256、Hook 模块路径及 `Ready` 状态；
线程的 DWORD 返回值不作为 x64 模块句柄。远端线程超时时保留仍可能使用的路径内存，避免释放后继续执行或重复注入。
安装模块驻留到进程退出，域重载只停止旧监控器；更换 DLL 需要关闭主 Editor 和 Worker。

## 包内容与导入设置

`tools/build.ps1` 构建主 DLL、协调器、gate、认证器和 resolver，暂存私有 release DIA。
开发机可以用 `-DiaDll <release msdia140.dll>` 指定源文件，消费者不需要相同 SDK 路径。
主 DLL／协调器／gate 设为 Windows x64 Editor-only、非 preloaded。
`RgbmBindings~` 由 Unity 忽略，包含解析工具、未改动的 DIA、Detours MIT 许可及依赖来源说明。
Unity exe／PDB 不随包复制。Oodle 专有 DLL 仍仅本地使用；本阶段未制作或发布 release。

`tools/deploy_rgbm_portability.ps1` 要求 Unity 已关闭，保存旧插件备份，保留用户配置和现有 Oodle 文件。
部署目标为 `E:\Unity\StarWings\ClientDevelopment\Assets\Plugins\Bc7Rdo`；原始备份为
`out/rgbm-portability/deployment/installed-before-20261010-151346`。

## 验证记录

- 第一轮 f2 batchmode 编译成功，但返回 1，native 报 `detour_transaction_rejected:13`。
  P3 接入没有把原函数槽初始化为待 Hook 入口，协调器按契约拒绝。补上初始化，保留完整入口／线程验证。
  失败产物保留为 `batch-f2-rejected-slot.json`／`.log`。
- 修复后的实际工程 batchmode PID 54180、状态 `Ready`、返回 0，工具目录忽略及三个原生插件的导入设置通过。
  `compress_bc7e.dll` 此时已加载，这一轮不能作为严格晚加载证据；进程内导入没有 Worker。
- 临时通过 `-refreshImportMode OutOfProcess` 启用并行导入后，真实 Worker 导入了测试 PNG，
  但初版识别只接受 `-assetImportWorker`，未注入实际使用另一种参数形态的 f2 Worker。
  已按实际命令行修复；这轮主进程成功不能算 Worker 安装成功。
- 实际工程修复复测 PID 49468、实际 Worker PID 2596 和 47368 均 `Ready`，创建 FILETIME 与存活进程一致，返回 0。
  缓存被故意破坏后自动重新认证并恢复，恢复文件与备份 SHA256 同为
  `E5B965FD83BF3589B1DFD47892FF9B25EF96D1C0D5C24B866AAF1E570F61D19B`。
- 独立最小 f2 工程 PID 8224、两个实际 Worker PID 45632 和 50908 均 Ready，返回 0。
  两个工程中 BC7 模块均在 managed 测试前已加载，不能声称完成 Unity 内“Ready 先于 BC7 加载”的严格场景。
- 独立宿主运行 `tests/test_hook.py --late-module` 返回 0：先加载 Hook，确认 Disabled 引导完成且 BC7 模块不存在，
  再加载 BC7 并验证导出 Hook、零 lambda、ERT 和 Unknown 退化。此宿主不是 Unity，不声称语义 Hook Ready。
- `tests/test_hook.py` 对新 DLL 退出 0：原生转发、零 lambda、ERT 生效以及 Unknown 的 Oodle／ERT 原生退化通过。
- 删除用户工程中的临时脚本及其 meta 后，正式插件 f2 batchmode 编译返回 0，最终主进程 PID 53856 报 Ready。
  确认 Unity 进程数 0、测试资源目录数 0、已安装的五个代码／二进制文件与暂存包 SHA256 一致。
  用户配置 SHA256 保持 `A446D940E9806C2F0058B603AB7564F736A4898873FDCB4609E8305C362AF1F6`；
  `EditorSettings.asset` 修改时间保持 2026-10-08 14:16:21，未持久改变并行导入设置。

测试驱动为 `tests/unity/RgbmP4Validation.cs`，仅临时部署。创建独立命名的 24 个 PNG，设置两个 Worker，
校验实际存活进程的创建时间和初始化报告，恢复 Worker 数量并删除测试资源。
`tools/run_rgbm_batch.ps1` 使用系统目录 PATH，完整日志落文件，等待真实退出并记录退出码。
测试脚本在用户工程最终清理，用户原有资产和压缩参数保持原样。

## 验收范围与后续边界

2026-10-10 用户明确打包机安装 VS，取消无 VS 环境测试。该测试未执行，也不再作为验收关口；
历史记录中的待测状态按本次范围调整更新。保留包内私有 DIA 方案及系统目录 PATH 的既有测试证据。

Hook／注入本身依赖 Windows API，不要求安装 VS。主 DLL／协调器采用 `/MT`，主 DLL 的现有 PE 导入检查
仅见系统 `KERNEL32`／`SHELL32`／`bcrypt`；DIA 通过包内 DLL 显式加载，不依赖 VS 安装目录或 COM 注册。
部署时需要插件实际运行依赖完整，安装 VS 不是 Hook 的前提。

独立宿主的晚加载验证与实际 Unity 的主进程／晚启动 Worker 安装已分别通过。
当前两个 Unity 工程启动时均提前加载 BC7，严格的 Unity 内先 Ready、再加载 BC7 场景尚无实测证据，继续保留此边界。
该额外场景此前误列为 P4 阻塞；原设计 P4 要求的是晚启动 Worker，已有实际通过证据。
按调整后的范围，P4 完成状态不依赖这一额外场景，也不宣称它已实测通过。

跨 f1／f2 的真实 RGBM／普通编码参数、调度行为、零 lambda、晚启动 Worker 编码矩阵后续已由 [P5](rgbm-hook-portability-acceptance.md) 完成，实际工程保留其最终通过的插件。
本文件的 P4 `Ready` 和导入证据本身不替代 P5；P6 交付仍待施工，完整 feature 尚未完成。

## 本地证据

`out/rgbm-portability/deployment/` 保存构建日志、deployment.json、各 batchmode 的报告／日志／真实退出码、
私有依赖导入检查和最终 SHA256 清单。该目录被 Git 忽略，施工文档保留结论和边界。
`summary.json` 记录最终编译、Ready、清理、已安装文件以及未完成关口；`artifact-sha256.json` 保存产物身份。
P3 被覆盖的开发产物先按原清单存入 `out/rgbm-portability/p3-snapshot`，不回写旧验证结论。

并行导入设置与 API 范围参照 [Unity 2022.3 并行导入文档](https://docs.unity3d.com/2022.3/Documentation/Manual/ParallelImport.html)。
