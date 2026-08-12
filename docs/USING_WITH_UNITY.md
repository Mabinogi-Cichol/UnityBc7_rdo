# 在 Unity 编辑器中启用 BC7 RDO

> ⚠️ 本流程会**替换 Unity 安装目录中的 `compress_bc7e.dll`**，需要管理员权限，
> 且可能涉及 Unity EULA 合规风险。替换行为由脚本完成并支持一键回滚，执行前请先关闭 Unity 编辑器。

## 原理回顾

壳 DLL（`compress_bc7e.dll`）本身不编码，它只读取**与自身同目录**的两个文件：

1. `compress_bc7e_original.dll`——原版 DLL（转发目标，压缩输出逐字节一致）；
2. `bc7_rdo_config.txt`——RDO 配置（lambda>0 时对压缩后的块流做 ERT 率失真后处理）。

**无任何环境变量**。只要这两个文件与壳 DLL 同目录（即 Unity Editor 目录），
转发与 RDO 即可正常工作；配置文件修改后会被自动重读。

## 快速开始

```powershell
# 1. 把 release 压缩包解压到任意目录
# 2. 右键 PowerShell / Windows Terminal -> 以管理员身份运行，执行：
powershell -ExecutionPolicy Bypass -File enable_unity_rdo.ps1
```

脚本自动检测 Unity 安装目录（`C:\Program Files\Unity` 下直接安装与
`Hub\Editor` 两种布局）；安装多个 Unity 版本时按编号选择。

在菜单中选择 **"替换"**，按提示输入 lambda（默认 1.0），脚本自动完成：

1. 把 Unity Editor 目录中的原版 `compress_bc7e.dll` 改名为
   `compress_bc7e_original.dll`（保证版本精确匹配）；
2. 复制壳 DLL 为 `compress_bc7e.dll`；
3. 写入 `bc7_rdo_config.txt`（lambda 为你输入的值，其余为默认参数）。

最后**完全退出并重启 Unity**，重新导入纹理即可。

## 脚本说明

**已替换检测**：目标 Unity Editor 目录存在 `compress_bc7e_original.dll` 即视为已替换。

菜单按目标目录状态显示：

- **未替换状态**菜单：
  - `替换`：执行上述三步替换；
  - `状态`：显示该版本 Unity 的 DLL 与配置文件状态。
- **已替换状态**菜单：
  - `回滚`：删除壳 DLL → 把 `compress_bc7e_original.dll` 改名回
    `compress_bc7e.dll` → 删除 `bc7_rdo_config.txt`；
  - `更新`：用新的壳 DLL 覆盖（先构建出 `out\compress_bc7e.dll` 再执行）；
  - `修改配置`：重新生成 `bc7_rdo_config.txt`（重新输入 lambda）；
  - `状态`：显示当前状态。

只查看所有版本的状态、不进入菜单：

```powershell
powershell -ExecutionPolicy Bypass -File enable_unity_rdo.ps1 -StatusOnly
```

自动检测失败时可用 `-UnityDir` 手动指定 Unity Editor 目录：

```powershell
powershell -ExecutionPolicy Bypass -File enable_unity_rdo.ps1 -UnityDir "C:\Program Files\Unity\Unity 2022.3.62f1\Editor"
```

脚本依赖同目录的 `unity_dirs.ps1`（共享的 Unity 安装目录检测模块），请勿删除或单独移动。

## 手动步骤（不用脚本）

```powershell
# 1. 关闭 Unity
# 2. 管理员 PowerShell
$unityDir = 'C:\Program Files\Unity\Unity 2022.3.62f1\Editor'

# 把原版 DLL 改名为 compress_bc7e_original.dll（版本精确匹配，无需额外备份）
Rename-Item "$unityDir\compress_bc7e.dll" "$unityDir\compress_bc7e_original.dll" -Force

# 复制壳 DLL 为 compress_bc7e.dll
Copy-Item '.\compress_bc7e.dll' "$unityDir\compress_bc7e.dll" -Force

# 写入配置（与 DLL 同目录）
@'
lambda=1.0
lookback=128
smooth_scale=15
max_rms_increase_ratio=10
two_matches=1
debug=0
'@ | Set-Content "$unityDir\bc7_rdo_config.txt" -Encoding utf8
```

回滚：

```powershell
# 删除壳 DLL -> 原 DLL 改名回来 -> 删除配置
Remove-Item "$unityDir\compress_bc7e.dll" -Force
Rename-Item "$unityDir\compress_bc7e_original.dll" "$unityDir\compress_bc7e.dll" -Force
Remove-Item "$unityDir\bc7_rdo_config.txt" -Force
```

## 修改 λ

直接编辑 Unity Editor 目录中的 `bc7_rdo_config.txt`（与壳 DLL 同目录），
保存后自动生效（下次导入即用新值），无需重启 Unity：

```text
lambda=2.0
```

配置参数：

| 参数 | 说明 | 默认 |
| --- | --- | --- |
| `lambda` | RDO 强度，0=关闭 | 1.0（建议 0.25–4.0） |
| `lookback` | 回看窗口字节数 | 128 |
| `smooth_scale` | 平滑块误差放大系数 | 15 |
| `max_rms_increase_ratio` | 允许 RMS 最大增幅 | 10 |
| `two_matches` | 是否尝试两个匹配 | 1 |
| `debug` | 调试输出 | 0 |

## 验证与回滚

建议先用 `lambda=0` 验证导入结果与替换前**完全一致**（转发逐字节透传），再逐步调大 λ：

1. 用 `-StatusOnly` 确认目标版本状态为"已替换"（存在 `compress_bc7e_original.dll`）；
2. 把 `bc7_rdo_config.txt` 中的 lambda 设为 0，重新导入纹理，结果应与替换前完全一致；
3. 调大 λ 后纹理仍可正常显示（画质略降），AssetBundle 体积下降；
4. 若纹理异常（黑图/全透明），检查 `compress_bc7e_original.dll` 与
   `bc7_rdo_config.txt` 是否与壳 DLL 同目录，并确认已完全退出并重启 Unity。

回滚：再次运行 `enable_unity_rdo.ps1`，选 **"回滚"** 即可一键恢复原状
（删除壳 DLL → 原 DLL 改名回来 → 删除配置）。

## 常见问题

1. **没有真正以管理员身份运行**：替换/回滚需要管理员权限，请右键 PowerShell /
   Windows Terminal → **以管理员身份运行**再执行脚本；
2. **Windows 受控文件夹访问**（勒索软件防护）拦截了 Program Files 写入：
   Windows 安全中心 → 病毒和威胁防护 → 勒索软件防护 → 受控文件夹访问 →
   把 PowerShell 加入"允许应用通过受控文件夹访问"，或临时关闭后重试；
3. **第三方安全软件**（360、火绒等）拦截：把 release 目录和 PowerShell 加入白名单；
4. **Unity / Unity Hub 仍在运行**：先全部关闭再替换/回滚；
5. **安装了多个 Unity 版本**：脚本按编号列出所有检测到的版本，选择对应的即可；
   各版本相互独立，需要分别替换/回滚。

## 注意事项

- 请在 Project Settings 中保持 BC7 Compressor 为 `bc7e`（默认），本 DLL 只影响该路径。
- Unity 升级后可能覆盖/失效，需重新执行替换。
- 不同开发机需要各自执行一次；构建产物与 release 包不进版本库。
- 回滚时会同时删除 Unity 目录中的 `bc7_rdo_config.txt`，如需保留配置请先备份。
- RDO 收益体现在 AssetBundle 的 LZ4/LZMA 阶段，BC7 块本身仍固定 16 字节/块。
