param(
    [string]$Version = "v0.1.0"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root "out"
$releaseDir = Join-Path $root "release\$Version"

# 1. 先构建
& (Join-Path $PSScriptRoot "build.ps1")

# 2. 组装 release 目录（先清空，避免残留旧版文件）
if (Test-Path -LiteralPath $releaseDir) {
    Remove-Item -LiteralPath $releaseDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null

Copy-Item -LiteralPath (Join-Path $out "compress_bc7e.dll") -Destination $releaseDir -Force
Copy-Item -LiteralPath (Join-Path $out "exports.txt") -Destination $releaseDir -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "enable_unity_rdo.ps1") -Destination $releaseDir -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "unity_dirs.ps1") -Destination $releaseDir -Force

# 配置示例
$configExample = @"
# bc7_rdo_config.txt 示例：与壳 DLL 放在同一目录（即 Unity Editor 目录）
lambda=1.0
lookback=128
smooth_scale=15
max_rms_increase_ratio=10
two_matches=1
debug=0
"@
Set-Content -LiteralPath (Join-Path $releaseDir "bc7_rdo_config.txt.example") -Value $configExample -Encoding utf8

# Release 说明
$releaseReadme = @"
compress_bc7e_rdo $Version - Unity BC7 RDO shell DLL (win-x64)
================================================================

文件说明
--------
- compress_bc7e.dll          壳 DLL（导出与 Unity 原 DLL 一致）。只读取与自身同目录的
                              compress_bc7e_original.dll 转发压缩，并按同目录的
                              bc7_rdo_config.txt 做 RDO 后处理，无任何环境变量
- enable_unity_rdo.ps1       安装/回滚脚本（与壳 DLL 同目录使用，需管理员权限）
- unity_dirs.ps1             脚本依赖的共享检测模块（请勿删除）
- bc7_rdo_config.txt.example 配置示例（脚本替换时会自动生成正式配置）
- exports.txt                导出表

使用方法
--------
1. 把压缩包解压到任意目录。
2. 右键 PowerShell / Windows Terminal -> 以管理员身份运行，执行：
   powershell -ExecutionPolicy Bypass -File enable_unity_rdo.ps1
   脚本自动检测 Unity 安装目录（C:\Program Files\Unity 下直接安装与 Hub\Editor 两种布局，
   多个版本时按编号选择），也可用 -UnityDir 手动指定。
3. 选择"替换"，按提示输入 lambda（默认 1.0）。脚本自动完成：
   - 把 Unity Editor 目录中的原版 compress_bc7e.dll 改名为 compress_bc7e_original.dll；
   - 复制 compress_bc7e.dll 为壳 DLL；
   - 写入 bc7_rdo_config.txt。
4. 完全退出并重启 Unity，重新导入纹理。

回滚
----
再次运行 enable_unity_rdo.ps1，选择"回滚"即可一键恢复原状：
删除壳 DLL -> compress_bc7e_original.dll 改名为 compress_bc7e.dll -> 删除 bc7_rdo_config.txt。

配置
----
bc7_rdo_config.txt 与壳 DLL 同目录（即 Unity Editor 目录），修改后自动生效：
lambda=1.0                  RDO 强度，0=关闭；建议 0.25~4.0（默认 1.0）
lookback=128                回看窗口字节数
smooth_scale=15             平滑块误差放大系数
max_rms_increase_ratio=10   允许 RMS 最大增幅
two_matches=1               是否尝试两个匹配
debug=0                     调试输出

注意
----
- 替换 Unity 目录中的 DLL 前请评估 Unity EULA 与版本兼容风险。
- Unity 升级后可能覆盖/失效，需重新执行替换。
- 多个 Unity 版本相互独立，需要分别替换/回滚。
- BC7 每块固定 16 字节，RDO 收益体现在块流被 LZ4/LZMA 等再次压缩时。
"@
Set-Content -LiteralPath (Join-Path $releaseDir "README-release.txt") -Value $releaseReadme -Encoding utf8

# 3. SHA256 校验
$files = Get-ChildItem -LiteralPath $releaseDir -File | Sort-Object Name
$lines = foreach ($f in $files) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash
    "{0}  {1}" -f $hash, $f.Name
}
Set-Content -LiteralPath (Join-Path $releaseDir "SHA256SUMS.txt") -Value $lines -Encoding ascii

# 4. 压缩
$zip = Join-Path $root "release\compress_bc7e_rdo-$Version-win-x64.zip"
New-Item -ItemType Directory -Force -Path (Split-Path $zip) | Out-Null
if (Test-Path -LiteralPath $zip) {
    Remove-Item -LiteralPath $zip -Force
}
Compress-Archive -Path (Join-Path $releaseDir "*") -DestinationPath $zip -CompressionLevel Optimal

# ---------------------------------------------------------------------------
# 5. Editor Plugin 发布包（放进工程 Assets 下即生效）
# ---------------------------------------------------------------------------
$pluginDir = Join-Path $root "release\$Version-editor-plugin"
if (Test-Path -LiteralPath $pluginDir) {
    Remove-Item -LiteralPath $pluginDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null

Copy-Item -LiteralPath (Join-Path $out "editor-plugin\Bc7Rdo\hook_loader.cs") -Destination $pluginDir -Force
Copy-Item -LiteralPath (Join-Path $out "editor-plugin\Bc7Rdo\Bc7Rdo.Editor.asmdef") -Destination $pluginDir -Force
Copy-Item -LiteralPath (Join-Path $out "editor-plugin\Bc7Rdo\bc7_rdo_hook.dll") -Destination $pluginDir -Force
Copy-Item -LiteralPath (Join-Path $out "editor-plugin\Bc7Rdo\bc7_rdo_config.txt") -Destination $pluginDir -Force
Copy-Item -LiteralPath (Join-Path $out "editor-plugin\README.md") -Destination (Join-Path $pluginDir "README-editor-plugin.txt") -Force

$files3 = Get-ChildItem -LiteralPath $pluginDir -File | Sort-Object Name
$lines3 = foreach ($f in $files3) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash
    "{0}  {1}" -f $hash, $f.Name
}
Set-Content -LiteralPath (Join-Path $pluginDir "SHA256SUMS.txt") -Value $lines3 -Encoding ascii

$zip3 = Join-Path $root "release\compress_bc7e_rdo-editor-plugin-$Version-win-x64.zip"
if (Test-Path -LiteralPath $zip3) {
    Remove-Item -LiteralPath $zip3 -Force
}
Compress-Archive -Path (Join-Path $pluginDir "*") -DestinationPath $zip3 -CompressionLevel Optimal

Write-Host ""
Write-Host "Release 目录: $releaseDir"
Write-Host "Release 压缩包: $zip"
Write-Host "Editor Plugin 目录: $pluginDir"
Write-Host "Editor Plugin 压缩包: $zip3"
Write-Host ""
Get-Content -LiteralPath (Join-Path $releaseDir "SHA256SUMS.txt")
