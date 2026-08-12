# unity_dirs.ps1 - Unity 安装目录自动检测模块（共享）
# 只提供 Find-UnityEditorDirs 一个函数，PS 5.1 兼容（不使用 -Depth）。

function Find-UnityEditorDirs {
    $result = @()
    $root = "C:\Program Files\Unity"
    if (-not (Test-Path -LiteralPath $root)) { return $result }
    Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $dll = Join-Path $_.FullName "Editor\compress_bc7e.dll"
        if (Test-Path -LiteralPath $dll) {
            $dir = Split-Path -Parent $dll
            $result += [PSCustomObject]@{
                Dir      = $dir
                Dll      = $dll
                Replaced = (Test-Path -LiteralPath (Join-Path $dir "compress_bc7e_original.dll"))
            }
        }
    }
    $hub = Join-Path $root "Hub\Editor"
    if (Test-Path -LiteralPath $hub) {
        Get-ChildItem -LiteralPath $hub -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            $dll = Join-Path $_.FullName "Editor\compress_bc7e.dll"
            if (Test-Path -LiteralPath $dll) {
                $dir = Split-Path -Parent $dll
                $result += [PSCustomObject]@{
                    Dir      = $dir
                    Dll      = $dll
                    Replaced = (Test-Path -LiteralPath (Join-Path $dir "compress_bc7e_original.dll"))
                }
            }
        }
    }
    return @($result | Sort-Object Dir)
}
