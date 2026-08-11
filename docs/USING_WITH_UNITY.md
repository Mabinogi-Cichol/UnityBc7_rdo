# 在 Unity 编辑器中启用 BC7 RDO

> ⚠️ 本流程会**替换 Unity 安装目录中的 `compress_bc7e.dll`**，需要管理员权限，
> 且可能涉及 Unity EULA 合规风险。执行前请先备份；本仓库提供一键脚本与回滚方法。

## 原理回顾

壳 DLL（`out/compress_bc7e.dll`）本身不编码，它：

1. 通过 `BC7E_ORIGINAL_DLL`（或**与自身同目录**的 `compress_bc7e_original.dll`）找到原 DLL 并转发压缩；
2. 通过 `BC7_RDO_LAMBDA` 或配置文件开启 RDO 后处理；
3. 配置文件路径可用 `BC7_RDO_CONFIG` 指定，未指定时读取**与自身同目录**的 `bc7_rdo_config.txt`。

**推荐做法：把原 DLL 副本和配置文件直接放进 Unity 安装目录**（即壳 DLL 的同目录），
这样完全不依赖环境变量，也不受"环境变量只在进程启动时读取"的影响。
脚本选项 1/6 会自动完成这两个文件的安装。

## 前置条件

- Unity 2022.3.62f1（已在本机确认 `C:\Program Files\Unity\Unity 2022.3.62f1\Editor\compress_bc7e.dll`）
- 已用 `tools/build.ps1` 构建出 `out\compress_bc7e.dll`
- 项目内已有 `vendor\compress_bc7e_original.dll`（原 DLL 副本，构建脚本会自动恢复）

## 交互式脚本

```powershell
powershell -ExecutionPolicy Bypass -File tools\enable_unity_rdo.ps1
```

运行后出现交互菜单：

```text
===== Unity BC7 RDO 工具 =====
  1) 替换 DLL（备份原 DLL + 装入壳 DLL + 安装兜底文件，需管理员）
  2) 设置环境变量（可选：兜底文件就位时无需设置）
  3) 回滚 DLL（从备份恢复原 DLL，移除兜底文件，需管理员）
  4) 生成/更新 bc7_rdo_config.txt
  5) 显示当前状态
  6) 安装兜底文件（原 DLL 副本 + 配置到 Unity 目录，免环境变量，需管理员）
  0) 退出
```

脚本特性：

- 非管理员启动时会询问是否以管理员身份重启；替换/回滚前会检测 Unity 是否正在运行，并要求二次确认；
- 选项 1 自动完成：备份原 DLL 到 `vendor\compress_bc7e_original.dll` → 用 `out\compress_bc7e.dll` 替换
  `Editor\compress_bc7e.dll` → 把原 DLL 副本与 `bc7_rdo_config.txt` 装入 Unity 目录（兜底，免环境变量）；
- 选项 2 可选设置用户级环境变量（优先级高于文件），仅当需要覆盖 Unity 目录中的配置时才用；
- 选项 6 单独安装兜底文件（壳 DLL 已就位、只需要补文件的场景，如本仓库最初版本已替换过 DLL）；
- 选项 5 会显示 Unity DLL 状态、兜底文件是否就位、环境变量与配置文件内容，方便排查。

只查看状态（不需要菜单）：

```powershell
powershell -ExecutionPolicy Bypass -File tools\enable_unity_rdo.ps1 -StatusOnly
```

可沿用命令行参数覆盖默认值：`-UnityVersion 2022.3.62f1 -Lambda 1.0 -ConfigFile 路径`。

执行前请关闭 Unity 编辑器。

## 手动步骤（不用脚本）

```powershell
# 1. 关闭 Unity
# 2. 管理员 PowerShell
$unityDir = 'C:\Program Files\Unity\Unity 2022.3.62f1\Editor'

# 备份原 DLL 到项目（已存在则跳过）
Copy-Item "$unityDir\compress_bc7e.dll" `
          'D:\ProjectFiles\Git\UnityBc7_rdo\vendor\compress_bc7e_original.dll' -Force

# 替换为壳 DLL
Copy-Item 'D:\ProjectFiles\Git\UnityBc7_rdo\out\compress_bc7e.dll' `
          "$unityDir\compress_bc7e.dll" -Force

# 兜底文件：原 DLL 副本 + 配置文件放进 Unity 目录（推荐，免环境变量）
Copy-Item 'D:\ProjectFiles\Git\UnityBc7_rdo\vendor\compress_bc7e_original.dll' `
          "$unityDir\compress_bc7e_original.dll" -Force

@'
lambda=1.0
lookback=128
smooth_scale=15
max_rms_increase_ratio=10
two_matches=1
debug=0
'@ | Set-Content "$unityDir\bc7_rdo_config.txt" -Encoding utf8
```

> 环境变量（`BC7E_ORIGINAL_DLL` / `BC7_RDO_CONFIG` / `BC7_RDO_LAMBDA`）是**可选的**：
> 只要兜底文件就位，壳 DLL 就能在自身目录找到原 DLL 和配置。
> 注意：环境变量在进程启动时读取，修改后必须**完全退出并重启 Unity** 才生效，
> 这是最初版本最容易踩的坑（DLL 已替换但 Unity 进程看不到环境变量，壳 DLL 输出全零块导致纹理全透明）。

修改 λ 只需编辑 Unity 目录中的 `bc7_rdo_config.txt`，下次导入自动生效（配置文件按修改时间自动重读）。

## 验证与回滚

建议先确认兜底文件就位、`lambda=0` 验证导入结果与替换前一致，再逐步调大 λ：

1. `-StatusOnly` 应显示：`Unity DLL: 已替换为壳 DLL`、`Unity 兜底: 原 DLL 副本已就位`、`Unity 配置: 已就位`；
2. λ=0 时纹理导入结果应与替换前**完全一致**（转发逐字节透传）；
3. 调大 λ 后纹理仍可正常显示（画质略降），AB 体积下降；
4. 若纹理异常（黑图/全透明），优先检查兜底文件是否存在、Unity 是否完全重启。

回滚：

```powershell
# 恢复原 DLL，并移除 Unity 目录中的兜底文件
Copy-Item 'D:\ProjectFiles\Git\UnityBc7_rdo\vendor\compress_bc7e_original.dll' `
          "$unityDir\compress_bc7e.dll" -Force
Remove-Item "$unityDir\compress_bc7e_original.dll", "$unityDir\bc7_rdo_config.txt" -Force
```

脚本选项 3 会自动完成上述操作。

## 常见问题：替换时 Access denied

1. **没有真正以管理员身份运行**：请右键 PowerShell / Windows Terminal → **以管理员身份运行**，再执行脚本；
2. **Windows 受控文件夹访问**（勒索软件防护）拦截了 Program Files 写入：
   Windows 安全中心 → 病毒和威胁防护 → 勒索软件防护 → 受控文件夹访问 →
   把 PowerShell 加入"允许应用通过受控文件夹访问"，或临时关闭后重试；
3. **第三方安全软件**（360、火绒等）拦截：把本项目目录和 PowerShell 加入白名单；
4. **Unity / Unity Hub 仍在运行**：先全部关闭再替换。

脚本会在复制失败时自动显示诊断信息（当前用户、管理员状态、文件属性、
ACL、是否被占用、Unity 进程），并询问是否执行 `takeown + icacls` 修复权限后重试。

## 注意事项

- 请在 Project Settings 中保持 BC7 Compressor 为 `bc7e`（默认），本 DLL 只影响该路径。
- Unity 升级后可能覆盖/失效，需重新执行。
- 不同开发机需要各自执行一次；构建产物不进版本库。
- 回滚时脚本会同时移除 Unity 目录中的兜底文件（原 DLL 副本与配置文件）。
- RDO 收益体现在 AssetBundle 的 LZ4/LZMA 阶段，BC7 块本身仍固定 16 字节/块。
