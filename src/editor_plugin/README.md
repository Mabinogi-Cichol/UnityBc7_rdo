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
   - `bc7_rdo_hook.dll`        原生 hook DLL（inline detour + ERT RDO 后处理）
   - `bc7_rdo_config.txt`      RDO 配置
2. 重启 / 重新打开工程即自动生效。无需管理员权限。
3. 打包安全：`hook_loader.cs` 内置 AssetPostprocessor，首次导入时自动把
   `bc7_rdo_hook.dll` 的插件导入器设为**仅编辑器平台**，不会打进玩家包。

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
lambda=1
lookback=128
smooth_scale=15
max_rms_increase_ratio=10
two_matches=1
debug=0
```

`lambda` 越大 RDO 越激进（更小的 LZ 体积、略降质量）；`lambda=0` 关闭。

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
