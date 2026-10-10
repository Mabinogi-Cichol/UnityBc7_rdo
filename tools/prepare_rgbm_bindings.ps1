param(
    [Parameter(Mandatory=$true)][string]$UnityExe,
    [Parameter(Mandatory=$true)][string]$CacheDirectory,
    [string]$Pdb = '',
    [string]$DiaDll = '',
    [string]$ToolDirectory = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $ToolDirectory) { $ToolDirectory = Join-Path $root 'out\rgbm-portability' }
$editor = (Resolve-Path -LiteralPath $UnityExe).Path
$hash = (Get-FileHash -LiteralPath $editor -Algorithm SHA256).Hash
New-Item -ItemType Directory -Force -Path $CacheDirectory | Out-Null
$cache = Join-Path $CacheDirectory ($hash + '.v1.bindings')
$report = Join-Path $CacheDirectory ($hash + '.validation.json')
$tool = Join-Path $ToolDirectory 'bc7_rgbm_bindings.exe'
if (Test-Path -LiteralPath $cache) {
    & $tool --mode verify --exe $editor --cache $cache --report $report | Out-Null
    if ($LASTEXITCODE -eq 0) { return [pscustomobject]@{Cache=$cache;Report=$report;CacheHit=$true;HookInstalled=$false} }
}
if (-not $Pdb) { $Pdb = Join-Path (Split-Path -Parent $editor) 'unity_x64.pdb' }
if (Test-Path -LiteralPath $Pdb) {
    if (-not $DiaDll) { $DiaDll = Join-Path $ToolDirectory 'msdia140.dll' }
    if (-not (Test-Path -LiteralPath $DiaDll)) { throw 'Matching-PDB route requires an explicit or private staged DIA DLL.' }
    $evidence = Join-Path $CacheDirectory ($hash + '.evidence.json')
    & (Join-Path $ToolDirectory 'bc7_rgbm_resolver.exe') --exe $editor --pdb $Pdb --dia $DiaDll --report $evidence --cache $cache | Out-Null
    # A present but wrong PDB is a hard rejection; never use it as scan evidence.
    if ($LASTEXITCODE -ne 0) { throw "PDB/contract/cache preparation rejected. See $evidence" }
} else {
    & $tool --mode scan --exe $editor --cache $cache --report $report | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Bounded scan rejected. See $report" }
}
& $tool --mode verify --exe $editor --cache $cache --report $report | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Published cache failed revalidation. See $report" }
[pscustomobject]@{Cache=$cache;Report=$report;CacheHit=$false;HookInstalled=$false}
