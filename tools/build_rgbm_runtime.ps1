param([string]$VsPath = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'out\rgbm-portability\runtime'
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force $out, $obj | Out-Null
if (-not $VsPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $VsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
}
if (-not $VsPath) { throw 'Visual Studio C++ toolchain not found' }
$vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
$src = Join-Path $root 'src'
$detours = Join-Path $root 'vendor\detours\src'
$flags = '/nologo /O2 /EHsc /std:c++17 /utf-8 /MT /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DRGBM_RUNTIME_TESTING'
function Compile([string]$command) {
    cmd /d /c "call `"$vcvars`" >nul 2>&1 && $command"
    if ($LASTEXITCODE -ne 0) { throw "Compiler exit code: $LASTEXITCODE" }
}
Compile "cl $flags /I `"$src`" /Fo`"$obj\\`" `"$root\tests\test_rgbm_runtime.cpp`" `"$src\rgbm_hook_runtime.cpp`" /Fe:`"$out\test_rgbm_runtime.exe`" /link /NOLOGO"
Compile "cl $flags /I `"$src`" /Fo`"$obj\\`" `"$root\tests\test_hook_concurrency.cpp`" /Fe:`"$out\test_hook_concurrency.exe`" /link /NOLOGO"
Compile "cl $flags /I `"$src`" /I `"$detours`" /Fo`"$obj\\`" `"$root\tests\p0\test_callback_clone.cpp`" `"$src\rgbm_hook_runtime.cpp`" `"$src\rgbm_hook_bindings.cpp`" /Fe:`"$out\test_runtime_clone.exe`" /link /NOLOGO `"$root\out\rgbm-portability\detours.lib`""
$formalFlags = $flags.Replace(' /DRGBM_RUNTIME_TESTING', '')
Compile "cl $formalFlags /LD /I `"$src`" /I `"$detours`" /Fo`"$obj\\`" `"$root\tests\unity\rgbm_runtime_unavailable_probe.cpp`" `"$src\rgbm_hook_runtime.cpp`" `"$src\rgbm_hook_install.cpp`" `"$src\rgbm_hook_bootstrap.cpp`" `"$src\rgbm_hook_bindings.cpp`" `"$src\rgbm_hook_contract.cpp`" `"$src\rgbm_binding_cache.cpp`" /Fe:`"$out\rgbm_runtime_unavailable_probe.dll`" /link /NOLOGO `"$root\out\rgbm-portability\detours.lib`""
Write-Output "Runtime tests built in $out"
