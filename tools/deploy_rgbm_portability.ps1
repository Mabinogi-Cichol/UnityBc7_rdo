param([Parameter(Mandatory=$true)][string]$ProjectPath, [string]$ExistingBackup = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$project = (Resolve-Path -LiteralPath $ProjectPath).Path.TrimEnd('\')
$destination = [IO.Path]::GetFullPath((Join-Path $project 'Assets\Plugins\Bc7Rdo'))
if (-not $destination.StartsWith($project + '\Assets\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Plugin destination outside project Assets' }
foreach ($running in @(Get-CimInstance Win32_Process -Filter "Name='Unity.exe'")) {
    if ($running.CommandLine -and $running.CommandLine.IndexOf($project, [StringComparison]::OrdinalIgnoreCase) -ge 0) { throw 'Close the target Unity project before deployment' }
}
$source = Join-Path $root 'out\editor-plugin\Bc7Rdo'
foreach ($name in @('hook_loader.cs','bc7_rdo_hook.dll','bc7_rgbm_coordinator.dll','bc7_rgbm_loader_gate.dll','RgbmBindings~\msdia140.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $name))) { throw "Missing staged file: $name" }
}
$backup = Join-Path $root ('out\rgbm-portability\deployment\installed-before-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
if ($ExistingBackup) {
    $backup = (Resolve-Path -LiteralPath $ExistingBackup).Path
    if (-not $backup.StartsWith((Join-Path $root 'out\rgbm-portability\deployment\installed-before-'), [StringComparison]::OrdinalIgnoreCase)) { throw 'Recovery backup outside owned backup directory' }
} elseif (Test-Path -LiteralPath $destination) { Copy-Item -LiteralPath $destination -Destination $backup -Recurse }
New-Item -ItemType Directory -Force $destination | Out-Null
$config = Join-Path $destination 'bc7_rdo_config.txt'
$configHash = if (Test-Path -LiteralPath $config) { (Get-FileHash -LiteralPath $config).Hash } else { '' }
foreach ($name in @('hook_loader.cs','bc7_rdo_hook.dll','bc7_rgbm_coordinator.dll','bc7_rgbm_loader_gate.dll')) {
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $destination $name) -Force
}
Copy-Item -LiteralPath (Join-Path $source 'RgbmBindings~') -Destination $destination -Recurse -Force
foreach ($name in @('rgbm_bindings_loader.cs','rgbm_bindings_loader.cs.meta')) {
    $legacy = Join-Path $destination $name
    if (Test-Path -LiteralPath $legacy) { Remove-Item -LiteralPath $legacy }
}
if (-not $configHash) { Copy-Item -LiteralPath (Join-Path $source 'bc7_rdo_config.txt') -Destination $config }
elseif ((Get-FileHash -LiteralPath $config).Hash -ne $configHash) { throw 'Existing configuration was changed' }
foreach ($name in @('bc7_rgbm_coordinator.dll','bc7_rgbm_loader_gate.dll')) {
    $meta = Join-Path $destination ($name + '.meta')
    if (-not (Test-Path -LiteralPath $meta)) {
        $guid = [Guid]::NewGuid().ToString('N')
        $text = @"
fileFormatVersion: 2
guid: $guid
PluginImporter:
  externalObjects: {}
  serializedVersion: 2
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 0
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
  - first:
      Any:
    second:
      enabled: 0
      settings: {}
  - first:
      Editor: Editor
    second:
      enabled: 1
      settings:
        CPU: x86_64
        OS: Windows
        DefaultValueInitialized: true
  userData:
  assetBundleName:
  assetBundleVariant:
"@
        # Unity's YAML reader needs a line terminator after the final empty value.
        # PowerShell here-strings omit the newline before the closing marker.
        [IO.File]::WriteAllText($meta, $text + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    }
    else {
        $text = [IO.File]::ReadAllText($meta)
        if ($text.Length -gt 0 -and -not $text.EndsWith("`n")) {
            # Repair metadata from earlier deployments without changing GUID/settings.
            [IO.File]::WriteAllText($meta, $text + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
        }
    }
}
$records = @(Get-ChildItem -LiteralPath $source -File -Recurse | Where-Object { $_.Name -ne 'bc7_rdo_config.txt' } | ForEach-Object {
    $relative = $_.FullName.Substring($source.Length + 1)
    if ($relative -notlike 'RgbmBindings~\*' -and $relative -notin @('hook_loader.cs','bc7_rdo_hook.dll','bc7_rgbm_coordinator.dll','bc7_rgbm_loader_gate.dll')) { return }
    $target = Join-Path $destination $relative
    if (Test-Path -LiteralPath $target) {
        $sha = (Get-FileHash -LiteralPath $_.FullName).Hash
        if ((Get-FileHash -LiteralPath $target).Hash -ne $sha) { throw "Deployed artifact mismatch: $relative" }
        [pscustomobject]@{name=$relative;sha256=$sha}
    }
})
[pscustomobject]@{project=$project;destination=$destination;backup=$backup;config_preserved=$true;files=$records} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'out\rgbm-portability\deployment\deployment.json') -Encoding utf8
Write-Output "Deployed RGBM portability plugin; backup=$backup"
