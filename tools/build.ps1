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
$clCmd = "call `"$vcvars`" >nul 2>&1 && cl /nologo /O2 /EHsc /std:c++17 /MT /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I `"$include`" /LD /Fo$out\ $sources /Fe:`"$out\compress_bc7e.dll`" /link /NOLOGO"
cmd /c $clCmd
if ($LASTEXITCODE -ne 0) {
    throw "cl failed with exit code $LASTEXITCODE"
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
