param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root "out"
$srcDir = Join-Path $root "src"
$vendor = Join-Path $root "vendor"
$rdo    = Join-Path $vendor "bc7enc_rdo"

New-Item -ItemType Directory -Force -Path $out | Out-Null

# 确保原 DLL 副本存在（不提交到 git；缺失时从 Unity 安装目录自动恢复）
$origDll = Join-Path $vendor "compress_bc7e_original.dll"
if (-not (Test-Path -LiteralPath $origDll)) {
    $unityDll = $env:UNITY_BC7E_DLL
    if (-not $unityDll) {
        $unityDll = "C:\Program Files\Unity\Unity 2022.3.62f1\Editor\compress_bc7e.dll"
    }
    if (Test-Path -LiteralPath $unityDll) {
        Copy-Item -LiteralPath $unityDll -Destination $origDll -Force
        Write-Host "Restored original DLL copy from: $unityDll"
    } else {
        throw "缺少 vendor\compress_bc7e_original.dll，且未找到 Unity 原 DLL（可用环境变量 UNITY_BC7E_DLL 指定）"
    }
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
