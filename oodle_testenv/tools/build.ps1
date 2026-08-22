# Builds src\oodle_bc7.cpp -> out\oodle_bc7.exe and copies the Oodle DLLs
# next to the exe so the harness can LoadLibrary them at runtime.
# x64 only. Requires a Visual Studio C++ toolchain (found via vswhere).

$ErrorActionPreference = "Stop"

$testenv = Split-Path -Parent $PSScriptRoot   # oodle_testenv
$out     = Join-Path $testenv "out"
$srcDir  = Join-Path $testenv "src"
$include = Join-Path $testenv "include"
$bin     = Join-Path $testenv "bin"

New-Item -ItemType Directory -Force -Path $out | Out-Null

# Locate VS via vswhere (same pattern as the main project's tools\build.ps1)
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

Write-Host "Building oodle_bc7.exe with toolchain at: $vsPath"
$clCmd = "call `"$vcvars`" >nul 2>&1 && cl /nologo /O2 /EHsc /std:c++17 /utf-8 /MT /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I `"$include`" /Fo$out\ `"$srcDir\oodle_bc7.cpp`" /Fe:`"$out\oodle_bc7.exe`" /link /NOLOGO /SUBSYSTEM:CONSOLE"
cmd /c $clCmd
if ($LASTEXITCODE -ne 0) {
    throw "cl failed with exit code $LASTEXITCODE"
}

# Copy the Oodle DLLs next to the exe (needed at runtime by LoadLibrary)
Copy-Item -LiteralPath (Join-Path $bin "oo2tex_win64_2.9.16.dll") -Destination $out -Force
Copy-Item -LiteralPath (Join-Path $bin "oo2texrt_win64_2.9.16.dll") -Destination $out -Force

Write-Host ""
Write-Host "Build OK:"
Get-ChildItem -LiteralPath $out | Select-Object Name, Length
