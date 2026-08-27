# BC7 RDO Unity Editor Plugin（外挂注入版）

把 BC7 RDO 注入到 Unity Editor 进程，无需修改 Unity 安装目录。

## 用法

1. 把 `out\editor-plugin\Bc7Rdo\` 整个目录复制到工程 `Assets` 下任意位置
   （例如 `Assets\Plugins\Bc7Rdo\` 或 `Assets\Editor\Bc7Rdo\`），目录内包含：
   - `hook_loader.cs`          加载器（[InitializeOnLoad] 自动执行）
   - `Bc7Rdo.Editor.asmdef`    **必须保留**：Editor-only 自定义程序集定义，
     保证脚本引用 UnityEditor.dll 编译通过——否则脚本放进 `Assets\Plugins`
     会被编入 firstpass 程序集（不引用 UnityEditor），打包/重新编译时报
     CS0246 编译错误
   - `bc7_rdo_hook.dll`        原生 hook DLL（inline detour + RDO 后处理）
   - `bc7_rdo_config.txt`      RDO 配置（含编码器切换开关）
   - `oo2tex_win64_2.9.16.dll`、`oo2texrt_win64_2.9.16.dll`
                               Oodle Texture 编码器运行库（可选：仅
                               `encoder=oodle` 时需要。**不在 release 包内**，
                               需手动从 Oodle SDK 拷贝，见"编码器切换"）
2. 重启 / 重新打开工程即自动生效。无需管理员权限。
3. 打包安全：`hook_loader.cs` 内置 AssetPostprocessor，首次导入时自动把
   `bc7_rdo_hook.dll` 与两个 Oodle DLL 的插件导入器设为**仅编辑器平台**，
   不会打进玩家包。

原理：C# 加载器在编辑器域加载时 `LoadLibraryW` 加载 hook DLL；hook DLL 的
DllMain 轮询等待 `compress_bc7e.dll`（Unity 首次导入 BC7 纹理时才加载），
出现后对 `bc7elib_compress_blocks` 导出桩做 inline detour 并做 RDO 后处理。

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
- `oodle_lambda`：**仅 `oodle` 编码器使用**的 RDO 强度（整数 0..100，
  默认 30 = UE 默认档）。与 `lambda` 相互独立，不做换算。
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

删除 `Assets\` 下该插件目录即可，对 Unity 安装与工程其余部分零改动。

## 许可

本项目基于 bc7enc_rdo（richgel999，MIT）。请遵守 Unity 的最终用户许可协议
（EULA），本插件不修改 Unity 任何文件，仅进程内运行时挂接。
