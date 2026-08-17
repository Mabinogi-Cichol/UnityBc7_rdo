param(
    [string]$UnityDir = "",   # 手动指定 Unity Editor 目录（跳过自动检测）
    [float]$Lambda = 1.0,     # 默认 RDO lambda
    [switch]$StatusOnly,      # 只显示状态，不进入菜单
    [string]$ShellDll = ""    # 手动指定壳 DLL 路径（默认按 脚本同目录 -> out\ 自动查找）
)

$ErrorActionPreference = "Stop"

# 引入共享检测模块（Find-UnityEditorDirs）
. (Join-Path $PSScriptRoot "unity_dirs.ps1")

# 壳 DLL 定位（按优先级）：
#   1. -ShellDll 参数显式指定；
#   2. 脚本同目录（release 包布局）；
#   3. 仓库 out\ 目录（源码树开发布局：tools\build.ps1 的构建产物）。
function Resolve-ShellDll {
    $candidates = @()
    if ($ShellDll) { $candidates += $ShellDll }
    $candidates += (Join-Path $PSScriptRoot "compress_bc7e.dll")
    $candidates += (Join-Path (Split-Path -Parent $PSScriptRoot) "out\compress_bc7e.dll")
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return $c }
    }
    return $null
}
$shellDll = Resolve-ShellDll

# ---------- 辅助函数 ----------

function Test-Admin {
    return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent())
        .IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-Sha256([string]$Path) {
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

function Show-Diagnostics([string]$DllPath) {
    Write-Host ""
    Write-Host "----------------- 诊断信息 -----------------"
    Write-Host "当前用户     : $env:USERDOMAIN\$env:USERNAME"
    Write-Host "是否管理员   : $(Test-Admin)"
    if (Test-Path -LiteralPath $DllPath) {
        $item = Get-Item -LiteralPath $DllPath
        Write-Host "目标属性     : $($item.Attributes)  IsReadOnly=$($item.IsReadOnly)"
        try {
            $acl = Get-Acl -LiteralPath $DllPath
            Write-Host "所有者       : $($acl.Owner)"
            $adminAce = $acl.Access | Where-Object { $_.IdentityReference -match 'Administrators' } | Select-Object -First 1
            if ($adminAce) {
                Write-Host "管理员 ACL   : $($adminAce.FileSystemRights)（$($adminAce.AccessControlType)）"
            }
        } catch {
            Write-Host "读取 ACL 失败: $($_.Exception.Message)"
        }
        Write-Host "独占打开目标 : $(if (Test-FileLocked $DllPath) {'失败（被占用或权限不足）'} else {'成功（未被占用）'})"
    }
    $unityProc = Get-Process -Name "Unity" -ErrorAction SilentlyContinue
    Write-Host "Unity 编辑器 : $(if ($unityProc) { ($unityProc.Id -join ',') } else { '未运行' })"
    $hubCount = @(Get-Process | Where-Object { $_.ProcessName -like 'Unity Hub*' }).Count
    Write-Host "Unity Hub    : $hubCount 个进程"
    Write-Host "---------------------------------------------"
}

function Invoke-FixPermissions([string]$DllPath) {
    Write-Host ""
    Write-Host ">>> 修复目标文件权限（takeown + icacls）<<<"
    if (-not (Test-Admin)) {
        Write-Host "需要管理员权限，无法修复。"
        return $false
    }
    if (-not (Confirm-Question "将把目标文件的所有者改为 Administrators 并授予完全控制，继续？")) {
        Write-Host "已取消。"
        return $false
    }
    & takeown.exe /F $DllPath
    if ($LASTEXITCODE -ne 0) {
        Write-Host "takeown 失败（错误码 $LASTEXITCODE）"
        return $false
    }
    & icacls.exe $DllPath /grant "*S-1-5-32-544:(F)"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "icacls 失败（错误码 $LASTEXITCODE）"
        return $false
    }
    Write-Host "权限修复完成。"
    return $true
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

# 文件操作统一失败处理：诊断 + 权限修复重试 + 访问被拒提示
function Invoke-OperationWithRetry {
    param(
        [string]$FixTarget,      # 权限修复目标文件路径
        [scriptblock]$Operation, # 要执行的文件操作
        [string]$FailMessage     # 失败提示前缀
    )
    try {
        & $Operation
        return $true
    } catch {
        Write-Host "${FailMessage}: $($_.Exception.Message)"
    }
    Show-Diagnostics $FixTarget
    if (Confirm-Question "是否尝试修复文件权限后重试？") {
        if (Invoke-FixPermissions $FixTarget) {
            try {
                & $Operation
                return $true
            } catch {
                Write-Host "${FailMessage}: $($_.Exception.Message)"
            }
        }
    }
    Show-AccessDeniedHints
    return $false
}

function Build-ConfigContent([float]$LambdaValue) {
    return "lambda=$LambdaValue`nlookback=128`nsmooth_scale=15`nmax_rms_increase_ratio=10`ntwo_matches=1`ndebug=0"
}

# 从现有配置文件读出 lambda（读不到返回 $null）
function Get-ConfigLambda([string]$ConfigPath) {
    if (Test-Path -LiteralPath $ConfigPath) {
        $line = Get-Content -LiteralPath $ConfigPath -ErrorAction SilentlyContinue |
            Where-Object { $_ -match '^lambda\s*=' } | Select-Object -First 1
        if ($line) {
            $v = 0.0
            if ([float]::TryParse(($line -split '=', 2)[1].Trim(), [ref]$v)) {
                return $v
            }
        }
    }
    return $null
}

# ---------- 状态判断与目录选择 ----------

function Get-TargetState([string]$target) {
    $orig = Join-Path $target "compress_bc7e_original.dll"
    $dll  = Join-Path $target "compress_bc7e.dll"
    if (Test-Path -LiteralPath $orig) { return "replaced" }
    if (Test-Path -LiteralPath $dll)  { return "original" }
    return "missing"
}

function Select-UnityDir {
    if ($UnityDir) {
        $dir = $UnityDir.TrimEnd('\')
        if (-not (Test-Path -LiteralPath (Join-Path $dir "compress_bc7e.dll"))) {
            Write-Host "错误：-UnityDir 指定的目录中未找到 compress_bc7e.dll：$dir"
            exit 1
        }
        return $dir
    }
    $dirs = @(Find-UnityEditorDirs)
    if ($dirs.Count -eq 0) {
        Write-Host "未自动检测到 Unity 安装目录（搜索范围 C:\Program Files\Unity，含 Hub）。"
        Write-Host "请用 -UnityDir 参数手动指定 Editor 目录，例如：-UnityDir `"C:\Program Files\Unity\Unity 2022.3.62f1\Editor`""
        exit 1
    }
    if ($dirs.Count -eq 1) {
        $d = $dirs[0]
        $state = if ($d.Replaced) { "已替换" } else { "未替换" }
        Write-Host "检测到 Unity 安装: $($d.Dir)（$state）"
        return $d.Dir
    }
    Write-Host "检测到多个 Unity 安装，请选择："
    for ($i = 0; $i -lt $dirs.Count; $i++) {
        $state = if ($dirs[$i].Replaced) { "已替换" } else { "未替换" }
        Write-Host ("  {0}. {1}（{2}）" -f ($i + 1), $dirs[$i].Dir, $state)
    }
    while ($true) {
        $n = Read-Host "请选择编号"
        $idx = 0
        if ([int]::TryParse($n, [ref]$idx) -and $idx -ge 1 -and $idx -le $dirs.Count) {
            return $dirs[$idx - 1].Dir
        }
        Write-Host "输入无效，请输入 1-$($dirs.Count) 之间的编号。"
    }
}

function Show-DirStatus([string]$target) {
    $dll  = Join-Path $target "compress_bc7e.dll"
    $orig = Join-Path $target "compress_bc7e_original.dll"
    $cfg  = Join-Path $target "bc7_rdo_config.txt"

    Write-Host ""
    Write-Host "================= 当前状态 ================="
    Write-Host "Unity 目录    : $target"

    $dllHash  = Get-Sha256 $dll
    $origHash = Get-Sha256 $orig
    if (-not $dllHash) {
        Write-Host "compress_bc7e.dll : 缺失"
    } elseif ($origHash) {
        Write-Host "compress_bc7e.dll : 已替换为壳 DLL（RDO 生效中）"
    } else {
        Write-Host "compress_bc7e.dll : 未替换（原版）"
    }
    Write-Host ("SHA256 前16位 compress_bc7e.dll           : {0}" -f $(if ($dllHash) { $dllHash.Substring(0, 16) } else { "-" }))
    Write-Host ("SHA256 前16位 compress_bc7e_original.dll  : {0}" -f $(if ($origHash) { $origHash.Substring(0, 16) } else { "-" }))

    if (Test-Path -LiteralPath $cfg) {
        Write-Host "配置文件      : 存在（$cfg）"
        Get-Content -LiteralPath $cfg -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "    $_" }
    } else {
        Write-Host "配置文件      : 不存在（$cfg）"
    }
    Write-Host "Unity 正在运行 : $(if (Test-UnityRunning) { '是' } else { '否' })"
    Write-Host "是否管理员     : $(if (Test-Admin) { '是' } else { '否' })"
    Write-Host "============================================="
}

# ---------- 替换 / 回滚 / 更新 / 配置 ----------

function Invoke-Replace([string]$target) {
    Write-Host ""
    Write-Host ">>> 替换 DLL <<<"
    $dll  = Join-Path $target "compress_bc7e.dll"
    $orig = Join-Path $target "compress_bc7e_original.dll"
    $cfg  = Join-Path $target "bc7_rdo_config.txt"

    # 1. 替换前检测：已有备份说明该 Unity 已替换过
    if (Test-Path -LiteralPath $orig) {
        Write-Host "已检测到 compress_bc7e_original.dll，说明该 Unity 已替换过。请使用菜单中的回滚/更新选项。"
        return
    }

    # 2. 安全守卫：已是本项目壳 DLL 但缺少备份，无法安全替换/回滚
    $dllHash   = Get-Sha256 $dll
    $shellHash = Get-Sha256 $shellDll
    if ($dllHash -and $shellHash -and $dllHash -eq $shellHash) {
        Write-Host "该 DLL 已是本项目壳 DLL，但缺少 compress_bc7e_original.dll 备份，无法安全替换/回滚。请先手动恢复原 DLL。"
        return
    }

    # 3. 检查壳 DLL 存在
    if (-not $shellHash) {
        Write-Host "未找到壳 DLL（查找顺序：脚本同目录 -> 仓库 out\ -> -ShellDll 参数）。"
        Write-Host "请先运行 tools\build.ps1 构建，或把 compress_bc7e.dll 与本脚本放在同一目录，或用 -ShellDll 参数指定。"
        return
    }
    Write-Host "壳 DLL 来源 : $shellDll（SHA256 前16位 $($shellHash.Substring(0,16))）"

    # 4. 权限 / Unity 运行状态 / 最终确认
    if (-not (Test-Admin)) {
        Write-Host "需要管理员权限，请以管理员身份运行本脚本。"
        return
    }
    if (Test-UnityRunning) {
        Write-Host "检测到 Unity 正在运行，替换前请先关闭编辑器！"
        if (-not (Confirm-Question "仍然继续？")) { return }
    }
    if (-not (Test-Path -LiteralPath $dll)) {
        Write-Host "未找到 Unity 原 DLL: $dll"
        return
    }
    if (-not (Confirm-Question "确认替换？原 DLL 将就地改名为 compress_bc7e_original.dll，并装入本项目壳 DLL。")) {
        Write-Host "已取消。"
        return
    }

    # 5. 执行：原 DLL 就地改名（保证版本精确匹配）
    if (-not (Invoke-OperationWithRetry -FixTarget $dll -FailMessage "重命名原 DLL 失败" -Operation { Rename-Item -LiteralPath $dll -NewName "compress_bc7e_original.dll" })) {
        return
    }

    # 复制壳 DLL
    $ok = Invoke-OperationWithRetry -FixTarget $dll -FailMessage "复制壳 DLL 失败" -Operation { Copy-Item -LiteralPath $shellDll -Destination $dll -Force }
    if (-not $ok) {
        # 尝试恢复原 DLL
        if (Test-Path -LiteralPath $orig) {
            Write-Host "尝试恢复原 DLL..."
            try {
                Rename-Item -LiteralPath $orig -NewName "compress_bc7e.dll"
                Write-Host "已恢复原 DLL。"
            } catch {
                Write-Host "恢复失败，请手动处理: $($_.Exception.Message)"
            }
        }
        return
    }

    # 写入配置
    $lam = Read-Lambda $Lambda
    $ok = Invoke-OperationWithRetry -FixTarget $cfg -FailMessage "写入配置文件失败" -Operation { Set-Content -LiteralPath $cfg -Value (Build-ConfigContent $lam) -Encoding utf8 }
    if (-not $ok) {
        Write-Host "壳 DLL 已装入，但配置文件写入失败。可稍后用菜单的“修改 RDO 配置”选项补写。"
        return
    }

    # 6. 完成提示
    Write-Host "替换完成。请完全退出并重启 Unity 后重新导入验证；修改 lambda 只需编辑同目录 bc7_rdo_config.txt（自动生效）。"
}

function Invoke-Rollback([string]$target) {
    Write-Host ""
    Write-Host ">>> 回滚 DLL <<<"
    $dll  = Join-Path $target "compress_bc7e.dll"
    $orig = Join-Path $target "compress_bc7e_original.dll"
    $cfg  = Join-Path $target "bc7_rdo_config.txt"

    # 1. 未替换过则无需回滚
    if (-not (Test-Path -LiteralPath $orig)) {
        Write-Host "该目录未替换过，无需回滚。"
        return
    }
    # 2. 检查壳 DLL 存在
    if (-not (Test-Path -LiteralPath $dll)) {
        Write-Host "未找到 compress_bc7e.dll: $dll"
        return
    }
    if (-not (Test-Admin)) {
        Write-Host "需要管理员权限，请以管理员身份运行本脚本。"
        return
    }
    if (Test-UnityRunning) {
        Write-Host "检测到 Unity 正在运行，回滚前请先关闭编辑器！"
        if (-not (Confirm-Question "仍然继续？")) { return }
    }
    if (-not (Confirm-Question "确认回滚？将删除壳 DLL，并把 compress_bc7e_original.dll 改名为 compress_bc7e.dll。")) {
        Write-Host "已取消。"
        return
    }

    # 3. 执行回滚
    if (-not (Invoke-OperationWithRetry -FixTarget $dll -FailMessage "删除壳 DLL 失败" -Operation { Remove-Item -LiteralPath $dll -Force })) { return }
    if (-not (Invoke-OperationWithRetry -FixTarget $orig -FailMessage "恢复原 DLL 失败" -Operation { Rename-Item -LiteralPath $orig -NewName "compress_bc7e.dll" })) { return }
    if (Test-Path -LiteralPath $cfg) {
        if (-not (Invoke-OperationWithRetry -FixTarget $cfg -FailMessage "删除配置文件失败" -Operation { Remove-Item -LiteralPath $cfg -Force })) {
            Write-Host "配置清理失败，可手动删除: $cfg"
        }
    }

    # 4. 完成提示
    Write-Host "已回滚，恢复原 DLL。配置与壳 DLL 已移除。"
}

function Invoke-Update([string]$target) {
    Write-Host ""
    Write-Host ">>> 更新壳 DLL 与配置 <<<"
    $dll  = Join-Path $target "compress_bc7e.dll"
    $orig = Join-Path $target "compress_bc7e_original.dll"
    $cfg  = Join-Path $target "bc7_rdo_config.txt"

    # 要求已替换
    if (-not (Test-Path -LiteralPath $orig)) {
        Write-Host "该目录未替换过（缺少 compress_bc7e_original.dll），请先执行替换。"
        return
    }
    if (-not (Get-Sha256 $shellDll)) {
        Write-Host "未找到壳 DLL（查找顺序：脚本同目录 -> 仓库 out\ -> -ShellDll 参数）。"
        Write-Host "请先运行 tools\build.ps1 构建，或把 compress_bc7e.dll 与本脚本放在同一目录，或用 -ShellDll 参数指定。"
        return
    }
    Write-Host "壳 DLL 来源 : $shellDll"
    if (-not (Test-Admin)) {
        Write-Host "需要管理员权限，请以管理员身份运行本脚本。"
        return
    }
    if (Test-UnityRunning) {
        Write-Host "检测到 Unity 正在运行，更新前请先关闭编辑器！"
        if (-not (Confirm-Question "仍然继续？")) { return }
    }
    if (-not (Confirm-Question "确认覆盖装入壳 DLL 并重写配置？")) {
        Write-Host "已取消。"
        return
    }

    # 覆盖复制壳 DLL
    if (-not (Invoke-OperationWithRetry -FixTarget $dll -FailMessage "复制壳 DLL 失败" -Operation { Copy-Item -LiteralPath $shellDll -Destination $dll -Force })) { return }

    # 显示现有配置后重写
    $existing = Get-ConfigLambda $cfg
    $defaultLam = if ($null -ne $existing) { $existing } else { $Lambda }
    if (Test-Path -LiteralPath $cfg) {
        Write-Host "现有配置内容："
        Get-Content -LiteralPath $cfg -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "    $_" }
    }
    $lam = Read-Lambda $defaultLam
    if (-not (Invoke-OperationWithRetry -FixTarget $cfg -FailMessage "写入配置文件失败" -Operation { Set-Content -LiteralPath $cfg -Value (Build-ConfigContent $lam) -Encoding utf8 })) { return }

    Write-Host "更新完成（lambda=$lam）。请完全退出并重启 Unity 后重新导入验证。"
}

function Invoke-WriteConfig([string]$target) {
    Write-Host ""
    Write-Host ">>> 修改 RDO 配置 <<<"
    $cfg = Join-Path $target "bc7_rdo_config.txt"

    if (Test-Path -LiteralPath $cfg) {
        Write-Host "现有配置内容："
        Get-Content -LiteralPath $cfg -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "    $_" }
    } else {
        Write-Host "当前无配置文件（$cfg）。"
    }

    $existing = Get-ConfigLambda $cfg
    $defaultLam = if ($null -ne $existing) { $existing } else { $Lambda }
    $lam = Read-Lambda $defaultLam

    if (-not (Invoke-OperationWithRetry -FixTarget $cfg -FailMessage "写入配置文件失败" -Operation { Set-Content -LiteralPath $cfg -Value (Build-ConfigContent $lam) -Encoding utf8 })) { return }

    Write-Host "已写入 $cfg（lambda=$lam）。修改自动生效，重启 Unity 后重新导入验证。"
}

# ---------- 菜单 ----------

function Show-Menu([string]$target, [string]$state) {
    if ($state -eq 'replaced') {
        $label = "已替换，RDO 生效中"
    } else {
        $label = "未替换"
    }
    Write-Host ""
    Write-Host "===== Unity BC7 RDO 工具 ====="
    Write-Host "目标: $target（$label）"
    if ($state -eq 'replaced') {
        Write-Host "  1) 回滚（恢复原 DLL，需管理员）"
        Write-Host "  2) 更新（重新装入壳 DLL 与配置，需管理员）"
        Write-Host "  3) 修改 RDO 配置"
        Write-Host "  4) 显示状态"
    } else {
        Write-Host "  1) 替换（装入壳 DLL + 配置，需管理员）"
        Write-Host "  2) 显示状态"
    }
    Write-Host "  0) 退出"
}

# ---------- 主流程 ----------

# 1. 仅显示状态：遍历所有检测到的目录（无 Unity 时打印提示）
if ($StatusOnly) {
    if ($UnityDir) {
        $dir = $UnityDir.TrimEnd('\')
        if (Test-Path -LiteralPath (Join-Path $dir "compress_bc7e.dll")) {
            Show-DirStatus $dir
        } else {
            Write-Host "错误：-UnityDir 指定的目录中未找到 compress_bc7e.dll：$dir"
            exit 1
        }
    } else {
        $dirs = @(Find-UnityEditorDirs)
        if ($dirs.Count -eq 0) {
            Write-Host "未自动检测到 Unity 安装目录（搜索范围 C:\Program Files\Unity，含 Hub）。"
        } else {
            foreach ($d in $dirs) {
                Show-DirStatus $d.Dir
            }
        }
    }
    exit 0
}

# 2. 非管理员时提示提权重启（透传 -UnityDir / -Lambda，参数用引号包裹）
if (-not (Test-Admin)) {
    Write-Host "当前不是管理员。替换/回滚 DLL 需要管理员权限。"
    if (Confirm-Question "是否以管理员身份重新启动本脚本？") {
        $argList = @(
            '-NoExit',
            '-ExecutionPolicy', 'Bypass',
            '-File', "`"$PSCommandPath`""
        )
        if ($UnityDir) {
            $argList += '-UnityDir', "`"$UnityDir`""
        }
        if ($ShellDll) {
            $argList += '-ShellDll', "`"$ShellDll`""
        }
        $argList += '-Lambda', "`"$Lambda`""
        Start-Process -FilePath (Get-Process -Id $PID).Path -Verb RunAs -ArgumentList $argList
        exit
    }
    Write-Host "继续以当前权限运行（替换/回滚时会提示权限不足）。"
}

# 3. 选择目标 Unity 目录
$target = Select-UnityDir

# 3.5 显示壳 DLL 来源（构建产物在 out\，缺失时提示构建）
if ($shellDll) {
    Write-Host "壳 DLL 来源 : $shellDll"
} else {
    Write-Host "警告：未找到壳 DLL（查找顺序：脚本同目录 -> 仓库 out\ -> -ShellDll 参数）。"
    Write-Host "      替换/更新前请先运行 tools\build.ps1 构建，或用 -ShellDll 参数指定。"
}

# 4. 菜单循环（按状态路由）
while ($true) {
    $state = Get-TargetState $target
    Show-Menu $target $state
    $choice = Read-Host "请选择"
    switch ($choice) {
        '0' { Write-Host "再见。"; exit }
        '1' {
            if ($state -eq 'replaced') { Invoke-Rollback $target } else { Invoke-Replace $target }
        }
        '2' {
            if ($state -eq 'replaced') { Invoke-Update $target } else { Show-DirStatus $target }
        }
        '3' {
            if ($state -eq 'replaced') { Invoke-WriteConfig $target } else { Write-Host "无效选项，请重新输入。" }
        }
        '4' {
            if ($state -eq 'replaced') { Show-DirStatus $target } else { Write-Host "无效选项，请重新输入。" }
        }
        default { Write-Host "无效选项，请重新输入。" }
    }
}
