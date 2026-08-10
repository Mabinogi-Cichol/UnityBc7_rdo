param(
    [string]$Version = "v0.1.0"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $root "out"
$releaseDir = Join-Path $root "release\$Version"

# 1. 先构建
& (Join-Path $PSScriptRoot "build.ps1")

# 2. 组装 release 目录
New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null

Copy-Item -LiteralPath (Join-Path $out "compress_bc7e.dll") -Destination $releaseDir -Force
Copy-Item -LiteralPath (Join-Path $out "compress_bc7e_original.dll") -Destination $releaseDir -Force
Copy-Item -LiteralPath (Join-Path $out "exports.txt") -Destination $releaseDir -Force

# 配置示例
$configExample = @"
# bc7_rdo_config.txt 示例：与本 DLL 放在同一目录
# 环境变量优先级高于本文件（BC7_RDO_LAMBDA / BC7_RDO_LOOKBACK / ...）
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
- compress_bc7e.dll          壳 DLL（导出与 Unity 原 DLL 一致）
- compress_bc7e_original.dll Unity 原 DLL 的项目内副本（转发目标；Unity 专有二进制，
                              仅限本地使用，禁止公开分发）
- bc7_rdo_config.txt.example RDO 配置文件示例（复制为 bc7_rdo_config.txt 使用）
- exports.txt                导出表

使用方法
--------
1. 保持 compress_bc7e.dll 与 compress_bc7e_original.dll 在同一目录；
   或设置环境变量 BC7E_ORIGINAL_DLL 指向原 DLL 副本。
2. 默认行为：转发到原 DLL，输出逐字节一致（RDO 关闭）。
3. 开启 RDO：设置 BC7_RDO_LAMBDA > 0（建议 0.25 ~ 4.0），或在本目录放置
   bc7_rdo_config.txt（内容见示例）。

侧信道参数（环境变量优先）
--------------------------
BC7_RDO_LAMBDA                  RDO 强度，0=关闭（默认）
BC7_RDO_LOOKBACK                回看窗口字节数，默认 128
BC7_RDO_SMOOTH_SCALE            平滑块误差放大系数，默认 15
BC7_RDO_MAX_RMS_INCREASE_RATIO  允许 RMS 最大增幅，默认 10
BC7_RDO_TWO_MATCHES             是否尝试两个匹配，默认 1
BC7_RDO_DEBUG                   调试输出，默认 0

注意
----
- 本项目未修改 Unity 安装目录；在编辑器内实际替换 DLL 前请评估 Unity EULA 与版本兼容风险。
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

Write-Host ""
Write-Host "Release 目录: $releaseDir"
Write-Host "Release 压缩包: $zip"
Write-Host ""
Get-Content -LiteralPath (Join-Path $releaseDir "SHA256SUMS.txt")
