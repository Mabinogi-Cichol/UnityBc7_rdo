param(
    [string]$UnityVersion = "2022.3.62f1",
    [float]$Lambda = 1.0,
    [string]$ConfigFile = "",
    [switch]$StatusOnly
)

$ErrorActionPreference = "Stop"

$root       = Split-Path -Parent $PSScriptRoot
$unityDll   = "C:\Program Files\Unity\Unity $UnityVersion\Editor\compress_bc7e.dll"
$backupDll  = Join-Path $root "vendor\compress_bc7e_original.dll"
$shellDll   = Join-Path $root "out\compress_bc7e.dll"
$unityDir     = Split-Path -Parent $unityDll
$fallbackOrig = Join-Path $unityDir "compress_bc7e_original.dll"
$fallbackCfg  = Join-Path $unityDir "bc7_rdo_config.txt"
if (-not $ConfigFile) {
    $ConfigFile = Join-Path $root "bc7_rdo_config.txt"
}

function Test-Admin {
    return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent())
        .IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-Sha256($Path) {
    if (Test-Path -LiteralPath $Path) {
        return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
    }
    return $null
}

function Read-Lambda([float]$Default) {
    while ($true) {
        $input = Read-Host "RDO lambda（默认 $Default，直接回车使用默认值）"
        if ([string]::IsNullOrWhiteSpace($input)) {
            return $Default
        }
        $v = 0.0
        if ([float]::TryParse($input, [ref]$v)) {
            return $v
        }
        Write-Host "输入无效，请输入数字，例如 1.0"
    }
}

function Confirm-Question([string]$Question) {
    $answer = Read-Host "$Question [y/N]"
    return $answer -in @('y', 'Y', 'yes', 'YES')
}

function Test-UnityRunning {
    return $null -ne (Get-Process -Name "Unity" -ErrorAction SilentlyContinue)
}

function Test-FileLocked([string]$Path) {
    try {
        $fs = [System.IO.File]::Open($Path, 'Open', 'ReadWrite', 'None')
        $fs.Close()
        return $false
    } catch {
        return $true
    }
}

function Show-Status {
    Write-Host ""
    Write-Host "================= 当前状态 ================="

    $unityHash  = Get-Sha256 $unityDll
    $shellHash  = Get-Sha256 $shellDll
    $backupHash = Get-Sha256 $backupDll

    if (-not $unityHash) {
        Write-Host "Unity DLL   : 未找到 $unityDll"
    } elseif ($unityHash -eq $shellHash) {
        Write-Host "Unity DLL   : 已替换为壳 DLL（RDO 生效中）"
    } elseif ($backupHash -and $unityHash -eq $backupHash) {
        Write-Host "Unity DLL   : 原版（未替换）"
    } else {
        Write-Host "Unity DLL   : 未知版本（既不是本项目壳 DLL，也不是备份的原版）"
    }

    if ($backupHash) {
        Write-Host "原 DLL 备份 : 存在（$backupDll）"
    } else {
        Write-Host "原 DLL 备份 : 缺失"
    }
    Write-Host "壳 DLL 产物 : $shellDll"

    $fbOrigHash = Get-Sha256 $fallbackOrig
    if ($fbOrigHash -and $fbOrigHash -eq $backupHash) {
        Write-Host "Unity 兜底  : 原 DLL 副本已就位（$fallbackOrig）"
    } elseif ($fbOrigHash) {
        Write-Host "Unity 兜底  : 原 DLL 副本存在但内容与备份不一致！($fallbackOrig)"
    } else {
        Write-Host "Unity 兜底  : 原 DLL 副本缺失（$fallbackOrig）—— 壳 DLL 将依赖环境变量"
    }
    if (Test-Path -LiteralPath $fallbackCfg) {
        Write-Host "Unity 配置  : 已就位（$fallbackCfg）"
        Get-Content -LiteralPath $fallbackCfg | ForEach-Object { Write-Host "    $_" }
    } else {
        Write-Host "Unity 配置  : 缺失（$fallbackCfg）"
    }
    Write-Host ""

    Write-Host "环境变量（可选：兜底文件就位时无需设置）:"
    foreach ($name in @('BC7E_ORIGINAL_DLL', 'BC7_RDO_CONFIG', 'BC7_RDO_LAMBDA')) {
        $val = [Environment]::GetEnvironmentVariable($name, 'User')
        if ($val) {
            Write-Host "$name = $val"
        } else {
            Write-Host "$name = （未设置）"
        }
    }
    Write-Host ""
    if (Test-Path -LiteralPath $ConfigFile) {
        Write-Host "配置文件     : $ConfigFile"
        Get-Content -LiteralPath $ConfigFile | ForEach-Object { Write-Host "    $_" }
    } else {
        Write-Host "配置文件     : 不存在（$ConfigFile）"
    }
    Write-Host "============================================="
}

function Show-Diagnostics {
    Write-Host ""
    Write-Host "----------------- 诊断信息 -----------------"
    Write-Host "当前用户     : $env:USERDOMAIN\$env:USERNAME"
    Write-Host "是否管理员   : $(Test-Admin)"
    if (Test-Path -LiteralPath $unityDll) {
        $item = Get-Item -LiteralPath $unityDll
        Write-Host "目标属性     : $($item.Attributes)  IsReadOnly=$($item.IsReadOnly)"
        try {
            $acl = Get-Acl -LiteralPath $unityDll
            Write-Host "所有者       : $($acl.Owner)"
            $adminAce = $acl.Access | Where-Object { $_.IdentityReference -match 'Administrators' } | Select-Object -First 1
            if ($adminAce) {
                Write-Host "管理员 ACL   : $($adminAce.FileSystemRights)（$($adminAce.AccessControlType)）"
            }
        } catch {
            Write-Host "读取 ACL 失败: $($_.Exception.Message)"
        }
        Write-Host "独占打开目标 : $(if (Test-FileLocked $unityDll) {'失败（被占用或权限不足）'} else {'成功（未被占用）'})"
    }
    $unityProc = Get-Process -Name "Unity" -ErrorAction SilentlyContinue
    Write-Host "Unity 编辑器 : $(if ($unityProc) { ($unityProc.Id -join ',') } else { '未运行' })"
    $hubCount = @(Get-Process | Where-Object { $_.ProcessName -like 'Unity Hub*' }).Count
    Write-Host "Unity Hub    : $hubCount 个进程"
    Write-Host "---------------------------------------------"
}

function Invoke-FixPermissions {
    Write-Host ""
    Write-Host ">>> 修复目标文件权限（takeown + icacls）<<<"
    if (-not (Test-Admin)) {
        Write-Host "需要管理员权限，无法修复。"
        return $false
    }
    if (-not (Confirm-Question "将把 compress_bc7e.dll 的所有者改为 Administrators 并授予完全控制，继续？")) {
        Write-Host "已取消。"
        return $false
    }
    & takeown.exe /F $unityDll
    if ($LASTEXITCODE -ne 0) {
        Write-Host "takeown 失败（错误码 $LASTEXITCODE）"
        return $false
    }
    & icacls.exe $unityDll /grant "*S-1-5-32-544:(F)"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "icacls 失败（错误码 $LASTEXITCODE）"
        return $false
    }
    Write-Host "权限修复完成。"
    return $true
}

function Copy-ShellToUnity {
    try {
        Copy-Item -LiteralPath $shellDll -Destination $unityDll -Force
        return $true
    } catch {
        Write-Host "复制失败: $($_.Exception.Message)"
        return $false
    }
}

function Copy-BackupToUnity {
    try {
        Copy-Item -LiteralPath $backupDll -Destination $unityDll -Force
        return $true
    } catch {
        Write-Host "复制失败: $($_.Exception.Message)"
        return $false
    }
}

# 安装兜底文件：原 DLL 副本 + 配置文件放入 Unity 安装目录。
# 壳 DLL 的查找顺序是 环境变量 -> 与自身同目录 -> 工作目录，
# 因此只要这两个文件就位，就完全不依赖环境变量。
function Install-FallbackFiles {
    param([float]$LambdaValue = $Lambda)

    $ok = $true
    if (-not (Test-Path -LiteralPath $backupDll)) {
        Write-Host "缺少原 DLL 副本：$backupDll（请先运行 tools\build.ps1 恢复）"
        return $false
    }
    try {
        Copy-Item -LiteralPath $backupDll -Destination $fallbackOrig -Force
        Write-Host "原 DLL 副本 -> $fallbackOrig"
    } catch {
        Write-Host "复制原 DLL 副本失败: $($_.Exception.Message)"
        $ok = $false
    }

    $lam = Read-Lambda $LambdaValue
    $cfg = "lambda=$lam`nlookback=128`smooth_scale=15`nmax_rms_increase_ratio=10`ntwo_matches=1`ndebug=0"
    try {
        Set-Content -LiteralPath $fallbackCfg -Value $cfg -Encoding utf8
        Write-Host "配置文件   -> $fallbackCfg（lambda=$lam）"
    } catch {
        Write-Host "写入配置文件失败: $($_.Exception.Message)"
        $ok = $false
    }
    return $ok
}

function Remove-FallbackFiles {
    foreach ($f in @($fallbackOrig, $fallbackCfg)) {
        if (Test-Path -LiteralPath $f) {
            try {
                Remove-Item -LiteralPath $f -Force
                Write-Host "已移除兜底文件: $f"
            } catch {
                Write-Host "移除兜底文件失败（请手动删除）: $f"
            }
        }
    }
}

function Finish-Replace {
    Write-Host "已替换 -> $unityDll"
    if (Install-FallbackFiles) {
        if ([Environment]::GetEnvironmentVariable('BC7E_ORIGINAL_DLL', 'User') -and
            (Confirm-Question "兜底文件已就位，不再依赖环境变量。是否清除用户级环境变量（BC7E_ORIGINAL_DLL / BC7_RDO_CONFIG / BC7_RDO_LAMBDA）？")) {
            foreach ($n in @('BC7E_ORIGINAL_DLL', 'BC7_RDO_CONFIG', 'BC7_RDO_LAMBDA')) {
                [Environment]::SetEnvironmentVariable($n, $null, 'User')
            }
            Write-Host "已清除环境变量。"
        }
    }
    Write-Host "完成。请完全退出并重启 Unity 后重新导入验证。"
}

function Show-AccessDeniedHints {
    Write-Host ""
    Write-Host "如果重试仍失败，请依次检查："
    Write-Host "  1) 确认是以管理员身份运行（右键 PowerShell/Windows Terminal -> 以管理员身份运行）；"
    Write-Host "  2) 打开 Windows 安全中心 -> 病毒和威胁防护 -> 勒索软件防护 ->"
    Write-Host "     受控文件夹访问 -> 允许应用通过受控文件夹访问，把 PowerShell 加入允许列表"
    Write-Host "     （或临时关闭受控文件夹访问后重试）；"
    Write-Host "  3) 关闭 Unity 编辑器与 Unity Hub 后重试；"
    Write-Host "  4) 若安装了三方安全软件（360/火绒等），把本项目目录和 PowerShell 加入白名单。"
}

function Invoke-Replace {
    Write-Host ""
    Write-Host ">>> 替换 DLL <<<"
    if (-not (Test-Admin)) {
        Write-Host "需要管理员权限，请以管理员身份运行本脚本。"
        return
    }
    if (-not (Test-Path -LiteralPath $unityDll)) {
        Write-Host "未找到 Unity 原 DLL: $unityDll"
        return
    }
    if (-not (Test-Path -LiteralPath $shellDll)) {
        Write-Host "未找到壳 DLL: $shellDll，请先运行 tools\build.ps1"
        return
    }
    if (Test-UnityRunning) {
        Write-Host "检测到 Unity 正在运行，替换前请先关闭编辑器！"
        if (-not (Confirm-Question "仍然继续？")) { return }
    }
    if (-not (Confirm-Question "确认替换？（原 DLL 将备份到 $backupDll）")) {
        Write-Host "已取消。"
        return
    }

    try {
        Copy-Item -LiteralPath $unityDll -Destination $backupDll -Force
        Write-Host "已备份原 DLL -> $backupDll"
    } catch {
        Write-Host "备份失败: $($_.Exception.Message)"
        Show-Diagnostics
        return
    }

    if (Copy-ShellToUnity) {
        Finish-Replace
        return
    }

    Show-Diagnostics
    if (Confirm-Question "是否尝试修复文件权限后重试？") {
        if (Invoke-FixPermissions) {
            if (Copy-ShellToUnity) {
                Finish-Replace
                return
            }
        }
    }
    Show-AccessDeniedHints
}

function Invoke-SetEnv {
    Write-Host ""
    Write-Host ">>> 设置环境变量（用户级，无需管理员）<<<"
    $lam = Read-Lambda $Lambda

    [Environment]::SetEnvironmentVariable('BC7E_ORIGINAL_DLL', $backupDll, 'User')
    [Environment]::SetEnvironmentVariable('BC7_RDO_CONFIG', $ConfigFile, 'User')
    [Environment]::SetEnvironmentVariable('BC7_RDO_LAMBDA', [string]$lam, 'User')

    Write-Host "BC7E_ORIGINAL_DLL = $backupDll"
    Write-Host "BC7_RDO_CONFIG    = $ConfigFile"
    Write-Host "BC7_RDO_LAMBDA    = $lam"

    if (-not (Test-Path -LiteralPath $ConfigFile)) {
        if (Confirm-Question "配置文件不存在，是否现在生成？") {
            Invoke-WriteConfig -LambdaValue $lam
        }
    }
    Write-Host "完成。环境变量优先级高于文件，设置后需完全重启 Unity 才生效；"
    Write-Host "也可以不设置——选项 1/6 会把兜底文件装入 Unity 目录（推荐，免环境变量）。"
}

function Invoke-WriteConfig {
    param([float]$LambdaValue = $Lambda)
    Write-Host ""
    Write-Host ">>> 生成/更新配置文件 $ConfigFile <<<"
    if ((Test-Path -LiteralPath $ConfigFile) -and
        -not (Confirm-Question "文件已存在，覆盖？")) {
        Write-Host "已取消。"
        return
    }
    $lam = Read-Lambda $LambdaValue
    @"
lambda=$lam
lookback=128
smooth_scale=15
max_rms_increase_ratio=10
two_matches=1
debug=0
"@ | Set-Content -LiteralPath $ConfigFile -Encoding utf8
    Write-Host "已写入 $ConfigFile"
}

function Invoke-Rollback {
    Write-Host ""
    Write-Host ">>> 回滚 DLL <<<"
    if (-not (Test-Admin)) {
        Write-Host "需要管理员权限，请以管理员身份运行本脚本。"
        return
    }
    if (-not (Test-Path -LiteralPath $backupDll)) {
        Write-Host "备份不存在：$backupDll，无法回滚。"
        return
    }
    if (-not (Test-Path -LiteralPath $unityDll)) {
        Write-Host "未找到 Unity DLL: $unityDll"
        return
    }
    if (Test-UnityRunning) {
        Write-Host "检测到 Unity 正在运行，回滚前请先关闭编辑器！"
        if (-not (Confirm-Question "仍然继续？")) { return }
    }
    if (-not (Confirm-Question "确认用备份覆盖 Unity 目录中的 DLL？")) {
        Write-Host "已取消。"
        return
    }

    if (Copy-BackupToUnity) {
        Write-Host "已回滚 -> $unityDll"
        Remove-FallbackFiles
        return
    }

    Show-Diagnostics
    if (Confirm-Question "是否尝试修复文件权限后重试？") {
        if (Invoke-FixPermissions) {
            if (Copy-BackupToUnity) {
                Write-Host "已回滚 -> $unityDll"
                Remove-FallbackFiles
                return
            }
        }
    }
    Show-AccessDeniedHints
}

function Invoke-InstallFallback {
    Write-Host ""
    Write-Host ">>> 安装兜底文件（原 DLL 副本 + 配置文件到 Unity 目录）<<<"
    if (-not (Test-Admin)) {
        Write-Host "需要管理员权限（写入 Unity 安装目录），请以管理员身份运行本脚本。"
        return
    }
    if (Test-UnityRunning) {
        Write-Host "检测到 Unity 正在运行，建议先关闭编辑器！"
        if (-not (Confirm-Question "仍然继续？")) { return }
    }
    if (Install-FallbackFiles) {
        Write-Host "完成。壳 DLL 会优先读取 Unity 目录下的原 DLL 副本与配置文件，不再依赖环境变量。"
        Write-Host "请完全退出并重启 Unity 后重新导入验证。"
    }
}

function Show-Menu {
    Write-Host ""
    Write-Host "===== Unity BC7 RDO 工具 ====="
    Write-Host "  1) 替换 DLL（备份原 DLL + 装入壳 DLL + 安装兜底文件，需管理员）"
    Write-Host "  2) 设置环境变量（可选：兜底文件就位时无需设置）"
    Write-Host "  3) 回滚 DLL（从备份恢复原 DLL，移除兜底文件，需管理员）"
    Write-Host "  4) 生成/更新 bc7_rdo_config.txt"
    Write-Host "  5) 显示当前状态"
    Write-Host "  6) 安装兜底文件（原 DLL 副本 + 配置到 Unity 目录，免环境变量，需管理员）"
    Write-Host "  0) 退出"
}

# 非管理员时，菜单模式自动请求提权（仅查看状态不需要）
if (-not $StatusOnly) {
    if (-not (Test-Admin)) {
        Write-Host "当前不是管理员。替换/回滚 DLL 需要管理员权限。"
        if (Confirm-Question "是否以管理员身份重新启动本脚本？") {
            $argList = @(
                '-NoExit',
                '-ExecutionPolicy', 'Bypass',
                '-File', "`"$PSCommandPath`"",
                '-UnityVersion', "`"$UnityVersion`"",
                '-Lambda', $Lambda,
                '-ConfigFile', "`"$ConfigFile`""
            )
            Start-Process -FilePath (Get-Process -Id $PID).Path -Verb RunAs -ArgumentList $argList
            exit
        }
        Write-Host "继续以当前权限运行（替换/回滚时会提示权限不足）。"
    }
}

if ($StatusOnly) {
    Show-Status
    exit
}

while ($true) {
    Show-Menu
    $choice = Read-Host "请选择"
    switch ($choice) {
        '1' { Invoke-Replace }
        '2' { Invoke-SetEnv }
        '3' { Invoke-Rollback }
        '4' { Invoke-WriteConfig }
        '5' { Show-Status }
        '6' { Invoke-InstallFallback }
        '0' { Write-Host "再见。"; exit }
        default { Write-Host "无效选项，请重新输入。" }
    }
}
