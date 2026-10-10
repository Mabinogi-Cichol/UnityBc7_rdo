param(
    [Parameter(Mandatory=$true)][string]$DiaDll,
    [string]$Destination = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Destination) { $Destination = Join-Path $root 'out\rgbm-portability\portable' }
$diaSource = (Resolve-Path -LiteralPath $DiaDll).Path
$bytes = [IO.File]::ReadAllBytes($diaSource)
if ($bytes.Length -lt 64 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5A4D) { throw 'DIA must be a PE file' }
$pe = [BitConverter]::ToInt32($bytes, 0x3c)
if ($pe -lt 0 -or $pe+24 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes,$pe) -ne 0x4550 -or
    [BitConverter]::ToUInt16($bytes,$pe+4) -ne 0x8664) { throw 'DIA must be x64' }
if ([IO.Path]::GetFileName($diaSource) -ne 'msdia140.dll') { throw 'Expected msdia140.dll' }
$resolver = Join-Path $root 'out\rgbm-portability\bc7_rgbm_resolver.exe'
if (-not (Test-Path -LiteralPath $resolver)) { throw 'Build resolver first' }
New-Item -ItemType Directory -Force $Destination | Out-Null
Copy-Item -LiteralPath $diaSource -Destination (Join-Path $Destination 'msdia140.dll') -Force
Copy-Item -LiteralPath $resolver -Destination $Destination -Force
Copy-Item -LiteralPath (Join-Path $root 'out\rgbm-portability\bc7_rgbm_bindings.exe') -Destination $Destination -Force
Copy-Item -LiteralPath (Join-Path $root 'vendor\detours\LICENSE.md') -Destination (Join-Path $Destination 'DETOURS-LICENSE.md') -Force
$notice = @'
RGBM runtime binding tools for the BC7 RDO Editor plugin.
Microsoft DIA msdia140.dll is staged unmodified from the explicitly supplied local SDK.
Redistribution is subject to the applicable Visual Studio license terms; licensed distributors
must use release, non-preview files listed by Microsoft. Do not redistribute Unity.exe/PDB.
https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution#dia-sdk
Detours MIT license is included. No symsrv.dll or symbol-server download is used.
Use --dia <this-directory>/msdia140.dll; no COM registration or installed VS is used to locate DIA.
'@
Set-Content -LiteralPath (Join-Path $Destination 'DEPENDENCY-NOTICE.txt') -Value $notice -Encoding utf8
$records = foreach ($name in @('bc7_rgbm_resolver.exe','bc7_rgbm_bindings.exe','msdia140.dll','DETOURS-LICENSE.md','DEPENDENCY-NOTICE.txt')) {
    $path = Join-Path $Destination $name
    [pscustomobject]@{Name=$name;SHA256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
}
$manifest = [pscustomobject]@{
    package_kind='rgbm_bindings_runtime_dependencies'
    dia_version=([Diagnostics.FileVersionInfo]::GetVersionInfo($diaSource)).FileVersion
    redistribution_basis='Visual Studio release DIA SDK redistributable list, subject to its license terms'
    files=@($records)
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Destination 'dependency-manifest.json') -Encoding utf8
if ((Get-FileHash -LiteralPath (Join-Path $Destination 'msdia140.dll') -Algorithm SHA256).Hash -ne
    (Get-FileHash -LiteralPath $diaSource -Algorithm SHA256).Hash) { throw 'DIA copy identity mismatch' }
Write-Output "Staged RGBM runtime dependencies: $Destination"
