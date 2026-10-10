param([string]$VsPath = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'out\rgbm-portability'
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force $out, $obj | Out-Null
if (-not $VsPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $VsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
}
if (-not $VsPath) { throw 'Visual Studio C++ toolchain not found' }
$vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
$dia = Join-Path $VsPath 'DIA SDK'
if (-not (Test-Path -LiteralPath (Join-Path $dia 'include\dia2.h'))) { throw 'DIA SDK development headers required' }
$detours = Join-Path $root 'vendor\detours\src'
$src = Join-Path $root 'src'
$flags = '/nologo /O2 /EHsc /std:c++17 /utf-8 /MT /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS'
function Invoke-Compiler([string]$command) {
    # No log polling: caller redirects this script's combined output to a file.
    cmd /d /c "call `"$vcvars`" >nul 2>&1 && $command"
    if ($LASTEXITCODE -ne 0) { throw "Compiler exit code: $LASTEXITCODE" }
}
$upstream = @('detours.cpp','modules.cpp','disasm.cpp','image.cpp','creatwth.cpp') | ForEach-Object { '"' + (Join-Path $detours $_) + '"' }
Invoke-Compiler "cl $flags /I `"$detours`" /c /Fo`"$obj\\`" $($upstream -join ' ')"
$objects = @('detours','modules','disasm','image','creatwth') | ForEach-Object { '"' + (Join-Path $obj ($_ + '.obj')) + '"' }
Invoke-Compiler "lib /nologo /out:`"$out\detours.lib`" $($objects -join ' ')"
Invoke-Compiler "cl $flags /I `"$src`" /I `"$detours`" /I `"$dia\include`" /Fo`"$obj\\`" `"$root\tools\rgbm_resolver\main.cpp`" `"$src\rgbm_hook_bindings.cpp`" `"$src\rgbm_hook_contract.cpp`" `"$src\rgbm_binding_cache.cpp`" /Fe:`"$out\bc7_rgbm_resolver.exe`" /link /NOLOGO /LIBPATH:`"$dia\lib\amd64`" `"$out\detours.lib`""
foreach ($test in @('test_rgbm_bindings','test_native_detour')) {
    $impl = if ($test -eq 'test_rgbm_bindings') { 'rgbm_hook_bindings.cpp' } else { 'native_detour_backend.cpp' }
    Invoke-Compiler "cl $flags /I `"$src`" /I `"$detours`" /Fo`"$obj\\`" `"$root\tests\$test.cpp`" `"$src\$impl`" /Fe:`"$out\$test.exe`" /link /NOLOGO `"$out\detours.lib`""
}
Invoke-Compiler "cl $flags /I `"$src`" /I `"$detours`" /Fo`"$obj\\`" `"$root\tests\p0\test_callback_clone.cpp`" `"$src\rgbm_hook_bindings.cpp`" /Fe:`"$out\test_callback_clone.exe`" /link /NOLOGO `"$out\detours.lib`""
Invoke-Compiler "cl $flags /I `"$src`" /I `"$detours`" /Fo`"$obj\\`" `"$root\tools\rgbm_resolver\profile_probe.cpp`" `"$src\rgbm_hook_bindings.cpp`" `"$src\rgbm_hook_contract.cpp`" /Fe:`"$out\profile_probe.exe`" /link /NOLOGO `"$out\detours.lib`""
Invoke-Compiler "cl $flags /I `"$src`" /I `"$detours`" /Fo`"$obj\\`" `"$root\tools\rgbm_resolver\binding_tool.cpp`" `"$src\rgbm_hook_bindings.cpp`" `"$src\rgbm_hook_contract.cpp`" `"$src\rgbm_binding_cache.cpp`" /Fe:`"$out\bc7_rgbm_bindings.exe`" /link /NOLOGO `"$out\detours.lib`""
Invoke-Compiler "cl $flags /LD /I `"$src`" /I `"$detours`" /Fo`"$obj\\`" `"$root\tests\unity\rgbm_binding_probe.cpp`" `"$src\rgbm_hook_bindings.cpp`" `"$src\rgbm_hook_contract.cpp`" `"$src\rgbm_binding_cache.cpp`" /Fe:`"$out\rgbm_binding_probe.dll`" /link /NOLOGO `"$out\detours.lib`""
Invoke-Compiler "cl $flags /I `"$src`" /I `"$detours`" /Fo`"$obj\\`" `"$root\tests\test_rgbm_disk_decoder.cpp`" `"$src\rgbm_hook_bindings.cpp`" `"$src\rgbm_hook_contract.cpp`" /Fe:`"$out\test_rgbm_disk_decoder.exe`" /link /NOLOGO `"$out\detours.lib`""
Write-Output "RGBM construction tools built in $out"
Write-Output 'DIA is not redistributed. Pass --dia with an explicitly available local msdia140.dll.'
