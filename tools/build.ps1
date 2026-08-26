param(
    [string]$Configuration = "Release",
    [string]$UnityDir = ""
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "unity_dirs.ps1")

$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root "out"
$srcDir = Join-Path $root "src"
$vendor = Join-Path $root "vendor"
$rdo    = Join-Path $vendor "bc7enc_rdo"
$testenvInclude = Join-Path $root "oodle_testenv\include"
$testenvBin     = Join-Path $root "oodle_testenv\bin"

New-Item -ItemType Directory -Force -Path $out | Out-Null

# 确保原 DLL 副本存在（不提交到 git；缺失时从 Unity 安装目录自动恢复）
$origDll = Join-Path $vendor "compress_bc7e_original.dll"
if (-not (Test-Path -LiteralPath $origDll)) {
    $srcDll = $null
    if ($UnityDir) {
        # 手动指定 Editor 目录：优先 compress_bc7e_original.dll，否则 compress_bc7e.dll
        $manualOrig = Join-Path $UnityDir "compress_bc7e_original.dll"
        if (Test-Path -LiteralPath $manualOrig) {
            $srcDll = $manualOrig
        } else {
            $manualDll = Join-Path $UnityDir "compress_bc7e.dll"
            if (Test-Path -LiteralPath $manualDll) {
                $srcDll = $manualDll
            }
        }
    } else {
        $dirs = @(Find-UnityEditorDirs)
        # 优先 Replaced=$true 的目录，用其中的 compress_bc7e_original.dll
        # （这样即使该 Unity 已被替换过，也能拿到真正的原 DLL）
        $replaced = $dirs | Where-Object { $_.Replaced } | Select-Object -First 1
        if ($replaced) {
            $candidate = Join-Path $replaced.Dir "compress_bc7e_original.dll"
            if (Test-Path -LiteralPath $candidate) {
                $srcDll = $candidate
            }
        }
        # 都没有时，取第一个目录的 compress_bc7e.dll
        if (-not $srcDll -and $dirs) {
            $first = $dirs | Select-Object -First 1
            $candidate = $first.Dll
            if (Test-Path -LiteralPath $candidate) {
                $srcDll = $candidate
            }
        }
    }
    if (-not $srcDll) {
        throw "缺少 vendor\compress_bc7e_original.dll，且未自动检测到 Unity 安装目录（可用 -UnityDir 参数指定 Editor 目录）"
    }
    Copy-Item -LiteralPath $srcDll -Destination $origDll -Force
    Write-Host "Restored original DLL copy from: $srcDll"
}

# 找一个带 VC 工具链的 Visual Studio / Build Tools 实例
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe not found"
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vsPath) {
    throw "No Visual Studio C++ toolchain found"
}
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found: $vcvars"
}

$sources = @(
    (Join-Path $srcDir "compress_bc7e_rdo.cpp"),
    (Join-Path $rdo "ert.cpp"),
    (Join-Path $rdo "bc7decomp.cpp")
) -join " "

$include = $rdo

Write-Host "Building compress_bc7e.dll with toolchain at: $vsPath"
$clCmd = "call `"$vcvars`" >nul 2>&1 && cl /nologo /O2 /EHsc /std:c++17 /utf-8 /MT /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I `"$include`" /LD /Fo$out\ $sources /Fe:`"$out\compress_bc7e.dll`" /link /NOLOGO"
cmd /c $clCmd
if ($LASTEXITCODE -ne 0) {
    throw "cl failed with exit code $LASTEXITCODE"
}

# ---- 注入版（外挂）组件 ----
# hook DLL：注入 Unity 进程后对 compress_bc7e.dll 导出桩做 inline detour + RDO
$hookSources = @(
    (Join-Path $srcDir "bc7_hook_dll.cpp"),
    (Join-Path $srcDir "oodle_hook.cpp"),
    (Join-Path $rdo "ert.cpp"),
    (Join-Path $rdo "bc7decomp.cpp")
) -join " "

Write-Host "Building bc7_rdo_hook.dll ..."
$clHook = "call `"$vcvars`" >nul 2>&1 && cl /nologo /O2 /EHsc /std:c++17 /utf-8 /MT /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I `"$include`" /I `"$srcDir`" /I `"$testenvInclude`" /LD /Fo$out\ $hookSources /Fe:`"$out\bc7_rdo_hook.dll`" /link /NOLOGO"
cmd /c $clHook
if ($LASTEXITCODE -ne 0) {
    throw "hook DLL build failed with exit code $LASTEXITCODE"
}

# 测试目标：自动化验证"远程注入 -> hook 生效"链路
$targetSrc = Join-Path $srcDir "bc7rdo_test_target.cpp"
Write-Host "Building bc7rdo_test_target.exe ..."
$clTarget = "call `"$vcvars`" >nul 2>&1 && cl /nologo /O2 /EHsc /std:c++17 /utf-8 /MT /D_CRT_SECURE_NO_WARNINGS /I `"$srcDir`" /Fo$out\ $targetSrc /Fe:`"$out\bc7rdo_test_target.exe`" /link /NOLOGO /SUBSYSTEM:CONSOLE"
cmd /c $clTarget
if ($LASTEXITCODE -ne 0) {
    throw "test target build failed with exit code $LASTEXITCODE"
}

# 把原始 DLL 副本放到输出目录，保证壳 DLL 能找到它（项目内副本，绝不触碰 Unity 目录）
Copy-Item -LiteralPath (Join-Path $vendor "compress_bc7e_original.dll") -Destination (Join-Path $out "compress_bc7e_original.dll") -Force

Write-Host ""
Write-Host "Build OK:"
Get-ChildItem -LiteralPath $out | Select-Object Name, Length

# 导出表自检
$dumpbin = Join-Path $vsPath "VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe"
$dumpbinPath = Get-ChildItem -Path (Split-Path $dumpbin -Parent) -Filter dumpbin.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if ($dumpbinPath) {
    & $dumpbinPath /exports (Join-Path $out "compress_bc7e.dll") | Out-File -FilePath (Join-Path $out "exports.txt") -Encoding utf8
    Write-Host "Exports written to out\exports.txt"
}

# ---- Editor Plugin 打包（放进工程 Assets 下即生效，无需注入器） ----
$pluginSrcDir = Join-Path $srcDir "editor_plugin"
$pluginOutDir = Join-Path $out "editor-plugin\Bc7Rdo"
New-Item -ItemType Directory -Force -Path $pluginOutDir | Out-Null

Copy-Item -LiteralPath (Join-Path $pluginSrcDir "hook_loader.cs") -Destination $pluginOutDir -Force
Copy-Item -LiteralPath (Join-Path $pluginSrcDir "Bc7Rdo.Editor.asmdef") -Destination $pluginOutDir -Force
Copy-Item -LiteralPath (Join-Path $out "bc7_rdo_hook.dll") -Destination $pluginOutDir -Force
Copy-Item -LiteralPath (Join-Path $root "bc7_rdo_config.txt") -Destination $pluginOutDir -Force
Copy-Item -LiteralPath (Join-Path $pluginSrcDir "README.md") -Destination (Join-Path $out "editor-plugin") -Force

# ---- Oodle Texture 编码器 DLL（Epic 专有，仅本地暂存，绝不进 release）----
# 这两个 DLL 是 Epic 专有二进制：不随仓库分发（oodle_testenv\.gitignore 已忽略
# bin/），也永远不会被打进 release 包（见 tools\make_release.ps1）。仅当开发者
# 手动把两份 DLL 放进 oodle_testenv\bin\ 时，本地构建才会把它们暂存到
# out\editor-plugin\Bc7Rdo\ 供本地测试/安装；release 发布包用户需自行从 Oodle SDK
# 获取并手动拷贝进插件目录（见 src\editor_plugin\README.md）。
$oodleDlls = @("oo2tex_win64_2.9.16.dll", "oo2texrt_win64_2.9.16.dll")
foreach ($dll in $oodleDlls) {
    $src = Join-Path $testenvBin $dll
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination $pluginOutDir -Force
        Write-Host "Staged (local-only): $dll -> out\editor-plugin\Bc7Rdo\"
    } else {
        Write-Host "WARNING: $dll 不存在于 oodle_testenv\bin\——未暂存到本地输出（仅本地测试用）。"
        Write-Host "         Oodle DLL 是 Epic 专有，release 包本就不包含，需从 Oodle SDK 手动获取；"
        Write-Host "         运行时若 encoder=oodle 而 DLL 缺失，hook 会自动回退默认"
        Write-Host "         bc7enc_rdo/ERT RDO 路径，不会降级为无 RDO 的普通 bc7e。"
    }
}

Write-Host ""
Write-Host "Editor plugin staged: out\editor-plugin\Bc7Rdo\（复制到工程 Assets 下即可）"
Get-ChildItem -LiteralPath $pluginOutDir | Select-Object Name, Length
