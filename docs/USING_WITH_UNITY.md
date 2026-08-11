# 在 Unity 编辑器中启用 BC7 RDO

> ⚠️ 本流程会**替换 Unity 安装目录中的 `compress_bc7e.dll`**，需要管理员权限，
> 且可能涉及 Unity EULA 合规风险。执行前请先备份；本仓库提供一键脚本与回滚方法。

## 原理回顾

壳 DLL（`out/compress_bc7e.dll`）本身不编码，它：

1. 通过 `BC7E_ORIGINAL_DLL`（或同目录 `compress_bc7e_original.dll`）找到原 DLL 并转发压缩；
2. 通过 `BC7_RDO_LAMBDA` 或配置文件开启 RDO 后处理；
3. 配置文件路径可用 `BC7_RDO_CONFIG` 指向项目内文件，避免在 Unity 目录放额外文件。

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
  1) 替换 DLL（备份原 DLL + 装入壳 DLL，需管理员）
  2) 设置环境变量（BC7E_ORIGINAL_DLL / BC7_RDO_CONFIG / BC7_RDO_LAMBDA）
  3) 回滚 DLL（从备份恢复原 DLL，需管理员）
  4) 生成/更新 bc7_rdo_config.txt
  5) 显示当前状态
  0) 退出
```

脚本特性：

- 非管理员启动时会询问是否以管理员身份重启；替换/回滚前会检测 Unity 是否正在运行，并要求二次确认；
- 选项 1 自动完成：备份原 DLL 到 `vendor\compress_bc7e_original.dll` → 用 `out\compress_bc7e.dll` 替换 `Editor\compress_bc7e.dll`；
- 选项 2 设置用户级环境变量（`BC7E_ORIGINAL_DLL`、`BC7_RDO_CONFIG`、`BC7_RDO_LAMBDA`），无需管理员；
- 选项 5 可随时查看 Unity DLL 是否已替换、备份是否存在、环境变量与配置文件内容。

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

# 备份原 DLL 到项目（已存在则跳过）
Copy-Item 'C:\Program Files\Unity\Unity 2022.3.62f1\Editor\compress_bc7e.dll' `
          'D:\ProjectFiles\Git\UnityBc7_rdo\vendor\compress_bc7e_original.dll' -Force

# 替换为壳 DLL
Copy-Item 'D:\ProjectFiles\Git\UnityBc7_rdo\out\compress_bc7e.dll' `
          'C:\Program Files\Unity\Unity 2022.3.62f1\Editor\compress_bc7e.dll' -Force

# 用户级环境变量（只需设置一次）
[Environment]::SetEnvironmentVariable('BC7E_ORIGINAL_DLL',
    'D:\ProjectFiles\Git\UnityBc7_rdo\vendor\compress_bc7e_original.dll', 'User')
[Environment]::SetEnvironmentVariable('BC7_RDO_CONFIG',
    'D:\ProjectFiles\Git\UnityBc7_rdo\bc7_rdo_config.txt', 'User')
[Environment]::SetEnvironmentVariable('BC7_RDO_LAMBDA', '1.0', 'User')
```

项目根目录创建 `bc7_rdo_config.txt`（脚本会自动生成示例）：

```text
lambda=1.0
lookback=128
smooth_scale=15
max_rms_increase_ratio=10
two_matches=1
debug=0
```

重启 Unity 后重新导入/重压缩 BC7 纹理即可。改 λ 只需编辑配置文件或环境变量，
下次导入自动生效（配置文件按修改时间自动重读）。

## 验证与回滚

建议先 `lambda=0` 验证导入结果与替换前一致，再逐步调大 λ。

回滚：

```powershell
Copy-Item 'D:\ProjectFiles\Git\UnityBc7_rdo\vendor\compress_bc7e_original.dll' `
          'C:\Program Files\Unity\Unity 2022.3.62f1\Editor\compress_bc7e.dll' -Force
```

## 常见问题：替换时 Access denied

备份成功但替换时报 `Access to the path ... is denied`，通常是以下原因：

1. **没有真正以管理员身份运行**：备份只需读权限（普通用户也有），所以备份成功不代表有写权限。
   请右键 PowerShell / Windows Terminal → **以管理员身份运行**，再执行脚本；
2. **Windows 受控文件夹访问**（勒索软件防护）拦截了 Program Files 写入：
   Windows 安全中心 → 病毒和威胁防护 → 勒索软件防护 → 受控文件夹访问 →
   把 PowerShell 加入“允许应用通过受控文件夹访问”，或临时关闭后重试；
3. **第三方安全软件**（360、火绒等）拦截：把本项目目录和 PowerShell 加入白名单；
4. **Unity / Unity Hub 仍在运行**：先全部关闭再替换。

升级后的脚本会在复制失败时自动显示诊断信息（当前用户、管理员状态、文件属性、
ACL、是否被占用、Unity 进程），并询问是否执行 `takeown + icacls` 修复权限后重试。

## 注意事项

- 请在 Project Settings 中保持 BC7 Compressor 为 `bc7e`（默认），本 DLL 只影响该路径。
- Unity 升级后可能覆盖/失效，需重新执行。
- 不同开发机需要各自执行一次；构建产物不进版本库。
- RDO 收益体现在 AssetBundle 的 LZ4/LZMA 阶段，BC7 块本身仍固定 16 字节/块。
