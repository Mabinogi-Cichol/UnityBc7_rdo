param([string]$VsPath = '', [switch]$Testing)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'out\rgbm-portability\coordinator'
if ($Testing) { $out = Join-Path $out 'test' }
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force $out, $obj | Out-Null
if (-not $VsPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $VsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
}
if (-not $VsPath) { throw 'Visual Studio C++ toolchain not found' }
$vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
$src = Join-Path $root 'src'
$vendor = Join-Path $root 'vendor\detours\src'
$flags = '/nologo /O2 /EHsc /std:c++17 /utf-8 /MT /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS'
if ($Testing) { $flags += ' /DRGBM_COORDINATOR_TESTING' }
function Compile([string]$command) {
    cmd /d /c "call `"$vcvars`" >nul 2>&1 && $command"
    if ($LASTEXITCODE -ne 0) { throw "Compiler exit code: $LASTEXITCODE" }
}
$sources = @('detour_coordinator.cpp','detours_checked_api.cpp','detours_transaction_engine.cpp') | ForEach-Object { '"' + (Join-Path $src $_) + '"' }
Compile "cl $flags /LD /I `"$src`" /I `"$vendor`" /Fo`"$obj\\`" $($sources -join ' ') `"$vendor\disasm.cpp`" `"$vendor\modules.cpp`" `"$vendor\image.cpp`" /Fe:`"$out\bc7_rgbm_coordinator.dll`" /link /NOLOGO /IMPLIB:`"$out\bc7_rgbm_coordinator.lib`""
Compile "cl /nologo /O2 /GS- /LD /Fo`"$obj\\`" `"$src\detour_loader_gate.cpp`" /Fe:`"$out\bc7_rgbm_loader_gate.dll`" /link /NOLOGO /NODEFAULTLIB /ENTRY:RgbmLoaderGateEntry `"$out\bc7_rgbm_coordinator.lib`" kernel32.lib"
if ($Testing) {
    Compile "cl $flags /I `"$src`" /Fo`"$obj\\`" `"$root\tests\test_detour_coordinator.cpp`" /Fe:`"$out\test_detour_coordinator.exe`" /link /NOLOGO `"$out\bc7_rgbm_coordinator.lib`""
}
else {
    Compile "cl $flags /LD /I `"$src`" /Fo`"$obj\\`" `"$root\tests\unity\detour_coordinator_probe.cpp`" /Fe:`"$out\rgbm_coordinator_probe.dll`" /link /NOLOGO `"$out\bc7_rgbm_coordinator.lib`""
}
Write-Output "Coordinator built in $out; testing=$Testing"
