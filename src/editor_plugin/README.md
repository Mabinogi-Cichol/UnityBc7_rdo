# BC7 RDO Unity Editor Plugin（外挂注入版）

把 BC7 RDO 注入到 Unity Editor 进程，无需修改 Unity 安装目录。

## v0.5.0 支持范围与启动

RGBM 语义 Hook 已在 Windows x64 的 Unity **2022.3.62f1／2022.3.62f2** 验证，包括 Editor、batchmode 和真实 AssetImportWorker；两版本使用同一套插件，不填写版本专用 RVA。其他 Editor 构建须通过当前映像的完整定位和接口认证，未认证版本不保证支持。

发布配置仍默认 `unity_rgbm_context_hooks=0`。要启用 RGBM 参数分流，关闭目标工程及 Worker，将该项改为 `1` 后重开工程。安装状态以 `Library/Bc7Rdo/HookBindings/` 下匹配 PID／创建时间的 `*.runtime.json` 的 `Ready`、`hook_installed=true` 为准；仅看到主 DLL 加载或 BC7 导出 Hook 成功，不代表 RGBM 可用。缓存文件本身为 `Library/Bc7Rdo/HookBindings/<exe-SHA256>.v1.bindings`。

解析优先使用当前 Editor 目录匹配的 `unity_x64.pdb`；没有 PDB 时采用受限扫描，错误 PDB、未知 ABI、歧义入口或已有未知 Hook 都拒绝安装。PDB 和 Unity 二进制需从合法安装获取，发行包不含这些文件。启用后语义关联不可用／Unknown 时直接使用 Unity 原生 BC7，避免套用错误的 RGBM 或普通纹理参数；`Faulted` 安装恢复错误让进程非零退出。

Hook 本身无需安装 VS，私有 DIA 也不依赖开发机 VS 路径或 COM 注册；本轮只验证用户已安装 VS 的打包环境，无 VS 环境未实测。`RgbmBindings~` 和 `Licenses~` 是 Unity 忽略目录，升级时须完整保留。解析工具、未修改的 DIA、Detours 和 bc7enc_rdo 许可随包提供。

已有参数、输出和主进程／Worker 一致性验证；解码差异不等同于源 HDR 的视觉质量验收。导入计时包含资源导入和采集，不是纯 Hook 性能保证。严格 Unity 内先 RGBM Ready、再首次加载 BC7 的额外场景尚未实测。DLL 合并是后续 TODO，本版仍需要主 DLL、coordinator 和 gate。

## 用法

1. 把 `out\editor-plugin\Bc7Rdo\` 整个目录复制到工程 `Assets` 下任意位置
   （例如 `Assets\Plugins\Bc7Rdo\` 或 `Assets\Editor\Bc7Rdo\`），目录内包含：
   - `hook_loader.cs`          单一加载脚本，包含 RGBM 绑定准备（[InitializeOnLoad] 自动执行）
   - `Bc7Rdo.Editor.asmdef`    **必须保留**：Editor-only 自定义程序集定义，
     保证脚本引用 UnityEditor.dll 编译通过——否则脚本放进 `Assets\Plugins`
     会被编入 firstpass 程序集（不引用 UnityEditor），打包/重新编译时报
     CS0246 编译错误
   - `bc7_rdo_hook.dll`        原生 hook DLL（inline detour + RDO 后处理）
   - `bc7_rgbm_coordinator.dll`、`bc7_rgbm_loader_gate.dll` 两点 Hook 安装依赖
   - `RgbmBindings~\`          私有 resolver、认证器与 release DIA；整个目录必须保留
   - `bc7_rdo_config.txt`      RDO 配置（含编码器切换开关）
   - `oo2tex_win64_2.9.16.dll`、`oo2texrt_win64_2.9.16.dll`
                               Oodle Texture 编码器运行库（可选：仅
                               `encoder=oodle` 时需要。**不在 release 包内**，
                               需手动从 Oodle SDK 拷贝，见"编码器切换"）
2. 重启 / 重新打开工程即自动生效。无需管理员权限。
   从含独立 `rgbm_bindings_loader.cs` 的版本升级时，先关闭目标工程并删除旧脚本及其 `.meta`，避免重复类型定义；`tools/deploy_rgbm_portability.ps1` 会先备份插件并自动清理这两个旧文件。
3. 打包安全：`hook_loader.cs` 内置 AssetPostprocessor，首次导入时自动把
   主 Hook、协调器、gate 与两个 Oodle DLL 的插件导入器设为**仅 Windows x64 编辑器平台**，
   不会打进玩家包。

原理：C# 加载器先准备当前 exe 的认证缓存，再 `LoadLibraryW` 加载 hook DLL。
DLL 初始化之外的线程先安装 RGBM 语义 Hook，再等待 `compress_bc7e.dll`，
出现后对 `bc7elib_compress_blocks` 导出桩做 inline detour 并做 RDO 后处理。

启用 `unity_rgbm_context_hooks=1` 时，缓存自动写入工程
`Library/Bc7Rdo/HookBindings/<exe SHA256>.v1.bindings`。缓存每次使用都重新认证；
存在匹配 PDB 时使用包内 DIA，缺少 PDB 时使用有限扫描。PDB 或接口不匹配会拒绝安装。
运行时不查找 VS、不注册 COM，不下载 Unity PDB；Hook／注入不要求安装 VS。
部署证据见仓库 `docs/features/rgbm-hook-portability-deployment.md`，当前包仍是施工产物。

每个进程独立核对 exe 和加载入口；晚启动 Worker 由父 Editor 准备该 Worker 的缓存，
Worker 原生层自行认证。`<PID>.<创建 FILETIME>.runtime.json` 记录本次初始化结果；
`Ready` 表示语义 Hook 已安装，不能单独证明 RGBM 编码参数和画质验收完成。
依赖缺失、接口不兼容或上下文未知时保留 Unity 原生 BC7。
更换 DLL 前关闭主 Editor 及其 Worker；已安装的模块驻留到进程退出，重编译脚本不会热卸载。

并行导入：Unity 2022 的批量 reimport 由 AssetImportWorker 子进程执行（不
运行编辑器脚本），插件会自动监控并把这些 worker 也注入 hook DLL（日志见
编辑器 Console 的 `[Bc7Rdo] worker 注入成功 PID=...`）。若需手动触发：
菜单 `Tools/BC7 RDO/Inject Import Workers`。

## 配置

同目录 `bc7_rdo_config.txt` 热更新（hook 每次调用都会检查文件修改时间）：

```
encoder=bc7enc_rdo
lambda=1
oodle_effort=high
oodle_lambda=30
rgbm_oodle_lambda=30
lookback=128
smooth_scale=15
max_rms_increase_ratio=10
two_matches=1
debug=0
trace_bc7_callstacks=0
unity_rgbm_context_hooks=0
rgbm_rdo=1
rgbm_lambda=0.25
rgbm_smooth_scale=30
rgbm_max_rms_increase_ratio=1.5
```

各键含义：

- `encoder`：编码器选择。`bc7enc_rdo`（默认）走 ERT RDO 后处理；
  `oodle` 走 Oodle Texture RDO（需插件目录存在两个 Oodle DLL，见"编码器切换"）。
- `lambda`：**仅 `bc7enc_rdo` 编码器使用**的 RDO 强度（浮点 ERT lambda），
  越大越激进（更小的 LZ 体积、略降质量）；`lambda=0` 关闭。
- `oodle_lambda`：普通 RGBA 图在 `oodle` 编码器下的 RDO 强度（整数 0..100，
  默认 30 = UE 默认档）。设为 `0` 时，RGBA 不加载或调用 Oodle，直接走 Unity
  原生 BC7 编码且不做 ERT 后处理；与 `lambda` 相互独立，不做换算。
- `rgbm_oodle_lambda`：仅 `encoder=oodle` 的已识别 RGBM Job 使用的 Oodle
  RDO 强度（整数 0..100）。默认 `-1`，表示继承 `oodle_lambda`；设置后只影响
  RGBM，不影响普通纹理。
- `oodle_effort`：`low | normal | high`（默认 `high`），仅 Oodle 编码器使用。
- `lookback`、`smooth_scale`、`max_rms_increase_ratio`、`two_matches`：
  ERT 参数，仅 `bc7enc_rdo` 编码器使用。
- `debug`：`1` 时输出 RDO 调用日志（会刷屏，调试完务必改回 `0`）。
- `trace_bc7_callstacks`：仅供定位 Unity 内部调用路径。记录前 N 次（最多 64）
  BC7 调用的模块相对地址栈；`0` 为关闭。典型诊断值为 `1`，完成后改回 `0`。
- `unity_rgbm_context_hooks`：`1` 时启用 Unity **2022.3.62f1** 专用的原生
  RGBM 上下文 hook。它从 `ConvertImageToRGBM` 标记像素源，并在 BC7 Job
  Worker 中恢复该标记；必须在启动 Unity 前设置。hook 会校验两个入口的
  固定入口指纹（12/15 字节），不匹配会拒绝安装；标记只在对应 BC7 Job 的执行期间有效，
  不会因像素内存地址后续复用而误命中。
- `rgbm_rdo`：仅在 `encoder=bc7enc_rdo` 且上述上下文 hook 已成功标记 RGBM
  Job 时生效，默认 `1`。目标格式为 `RGBA_BC7_SRGB`，因此它让 ERT 以 Unity
  实际采样后的 `Linear(sRGB) * M` 三通道误差进行候选块比较、RMS 限制和光滑
  块判断，而不是把 RGBA 当作四个独立通道。`0` 保留普通 RGBA ERT 度量。
- `rgbm_lambda`：RGBM 专用 ERT 强度，默认 `0.25`；仍受全局 `lambda` 总开关
  控制，`lambda=0` 会关闭所有 ERT。
- `rgbm_smooth_scale`：RGBM 光滑区域失真惩罚，默认 `30`。
- `rgbm_max_rms_increase_ratio`：RGBM 单块允许的最大 RMS 增长倍数，默认
  `1.5`，用于阻止少量块出现很强的局部劣化。

## 部署后的 meta 解析错误

`.meta` 通常由 Unity 自动生成；仓库部署脚本会为 coordinator 和 loader gate
预写 Windows Editor／x86_64 设置。v0.5.1 修复了脚本未写入文件末尾换行的问题，
避免启动时出现 `Parser Failure at line 29: Expect ':' between key and value within mapping`。
再次部署时也会给已有文件补齐末尾换行，保留 GUID 和平台设置。

通过旧版部署脚本安装的工程，先关闭 Editor 和 Worker，再使用当前仓库的
`tools/deploy_rgbm_portability.ps1` 重新部署；也可仅在
`bc7_rgbm_coordinator.dll.meta` 和 `bc7_rgbm_loader_gate.dll.meta` 的最后一行后
补一个换行并保存。已有损坏的 `.meta` 需要单独修复，更新 ZIP 中的 DLL 后仍应完成此步骤。
发布 ZIP 不含这两个 `.meta`，全新安装时由 Unity 自动生成。

## 编码器切换

默认编码器是 `bc7enc_rdo`（ERT，开源，随插件分发）。要改用 **Oodle Texture
RDO**，只需把 `bc7_rdo_config.txt` 中的 `encoder` 改为 `oodle`：

- 需要把两个 **Epic 专有** 的 Oodle DLL 放进插件目录：
  `oo2tex_win64_2.9.16.dll` 与 `oo2texrt_win64_2.9.16.dll`。这两个 DLL
  **既不在仓库中，也不在 release 发布包中**，需自行从获得授权的 Oodle SDK
  获取后**手动**放置，二选一：
  1. **本地构建**：把两个 DLL 拷进 `oodle_testenv\bin\`，运行
     `tools\build.ps1` 后它们会被暂存进 `out\editor-plugin\Bc7Rdo\`；
  2. **release 包**：解压插件包并复制进工程 `Assets` 后，直接把两个 DLL
     拷进插件目录（与 `bc7_rdo_hook.dll` 同目录）。
- 切换后 hook 做完整 Oodle RDO 编码（跳过 bc7e+ERT），包括 RGBM 图片也直接
  走 Oodle；RGBM 可由 `rgbm_oodle_lambda` 覆盖通用 `oodle_lambda`，
  `oodle_effort` 仍共享。`rgbm_rdo`、`lambda`、`lookback` 等 ERT 键被忽略。
- 对每类纹理，最终选中的 Oodle lambda 为 `0` 时，都会直接走 Unity 原生 BC7，
  既不使用 Oodle，也不做 ERT 后处理：`oodle_lambda=0` 只影响普通 RGBA；
  `rgbm_oodle_lambda=0` 只影响已识别的 RGBM Job。`rgbm_oodle_lambda=-1` 时
  RGBM 继承 `oodle_lambda`。
- 若 `encoder=oodle` 但 Oodle DLL 缺失或编码失败，插件自动**回退到默认
  `bc7enc_rdo` / ERT RDO 路径**：调用原 bc7e 压缩后接 ERT 后处理，与
  `encoder=bc7enc_rdo` 行为完全一致（**不是**无 RDO 的普通 bc7e），仅打
  一次日志提示，不会报错崩溃。
- 冒烟测试（`Tools/BC7 RDO/Smoke Test`）固定走默认 `bc7enc_rdo` 路径。

## 日志

同目录 `bc7_rdo_hook.log`（装钩结果、debug=1 时的 RDO 调用记录）。
建议把这两个文件加进 `.gitignore`（插件目录下的 `bc7_rdo_config.txt` 与
`bc7_rdo_hook.log` 属于运行期产物，不该提交）。

## 菜单

- `Tools/BC7 RDO/Reload Hook DLL`：重新加载 hook DLL
- `Tools/BC7 RDO/Smoke Test`：端到端冒烟测试（批处理下等价于
  `-executeMethod Bc7Rdo.HookLoader.SmokeTest`）
- `Tools/BC7 RDO/Inject Import Workers`：手动扫描并注入并行导入 worker

## 卸载

先关闭目标 Editor 及 Worker，再删除 `Assets\` 下插件目录。原生 Hook 驻留到进程退出；菜单 Reload 和脚本域重载不提供原生热卸载，更换 DLL 也需要关闭工程。

## 许可

本项目基于 bc7enc_rdo（richgel999，MIT）。请遵守 Unity 的最终用户许可协议
（EULA），本插件不修改 Unity 任何文件，仅进程内运行时挂接。
