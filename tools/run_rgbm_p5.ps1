param([Parameter(Mandatory=$true)][string]$UnityExe,
      [Parameter(Mandatory=$true)][string]$ProjectPath,
      [Parameter(Mandatory=$true)][string]$SourceHdr,
      [Parameter(Mandatory=$true)][string]$Name,
      [switch]$EditorMode, [switch]$Fallback,
      [ValidateSet('main','worker')][string]$Route='main',
      [ValidateRange(1,6)][int]$Pairs=6)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root 'out\rgbm-portability\acceptance'
if($Name -notmatch '^[a-zA-Z0-9_-]+$') { throw 'Invalid report name' }
foreach($value in @($UnityExe,$ProjectPath,$SourceHdr)) { if($value.Contains('"')) { throw 'Invalid path argument' } }
$resolvedProject=(Resolve-Path -LiteralPath $ProjectPath).Path
foreach($running in @(Get-CimInstance Win32_Process -Filter "Name='Unity.exe'")) {
    if($running.CommandLine -and $running.CommandLine.IndexOf($resolvedProject,[StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "P5 project already open in Unity PID $($running.ProcessId)"
    }
}
$info=[Diagnostics.ProcessStartInfo]::new()
$info.FileName=(Resolve-Path -LiteralPath $UnityExe).Path
$importMode=if($Route -eq 'worker') { 'OutOfProcess' } else { 'InProcess' }
$info.Arguments='-projectPath "'+$resolvedProject+'" -refreshImportMode '+$importMode+' -executeMethod Bc7Rdo.RgbmP5Validation.Start -logFile "'+(Join-Path $output ($Name+'.log'))+'"'
if(-not $EditorMode) { $info.Arguments+=' -batchmode -nographics' }
$info.UseShellExecute=$false; $info.CreateNoWindow=$true; $info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden; $info.WorkingDirectory=$ProjectPath
$info.EnvironmentVariables['BC7_RGBM_P5_REPORT']=Join-Path $output ($Name+'.json')
$info.EnvironmentVariables['BC7_RGBM_P5_SOURCE_HDR']=(Resolve-Path -LiteralPath $SourceHdr).Path
$info.EnvironmentVariables['BC7_RGBM_P5_MODE']=if($Fallback) { 'fallback' } else { 'matrix' }
$info.EnvironmentVariables['BC7_RGBM_P5_ROUTE']=$Route
$info.EnvironmentVariables['BC7_RGBM_P5_PAIRS']=[string]$Pairs
$ownedProjects=[IO.Path]::GetFullPath((Join-Path $output 'projects'))+'\'
$info.EnvironmentVariables['BC7_RGBM_P5_ISOLATED']=if([IO.Path]::GetFullPath($ProjectPath).StartsWith($ownedProjects,[StringComparison]::OrdinalIgnoreCase)) { '1' } else { '0' }
if($info.EnvironmentVariables['BC7_RGBM_P5_ISOLATED'] -eq '1') {
    # Workers read project settings from disk; prepare the owned project's quality
    # before launch instead of relying on an unsaved parent-only setter.
    $settings=Join-Path $ProjectPath 'ProjectSettings\ProjectSettings.asset'
    if(-not (Test-Path -LiteralPath $settings)) {
        $template=Join-Path $output 'projects\f1\ProjectSettings\ProjectSettings.asset'
        Copy-Item -LiteralPath $template -Destination $settings
    }
    $yaml=[IO.File]::ReadAllText($settings)
    $pattern='(?ms)^  m_BuildTargetGroupLightmapEncodingQuality:.*?(?=^  m_BuildTargetGroupHDRCubemapEncodingQuality:)'
    if(-not [regex]::IsMatch($yaml,$pattern)) { throw 'Unexpected owned ProjectSettings schema' }
    $normal="  m_BuildTargetGroupLightmapEncodingQuality:`n  - m_BuildTarget: Standalone`n    m_EncodingQuality: 1`n"
    [IO.File]::WriteAllText($settings,[regex]::Replace($yaml,$pattern,$normal),[Text.UTF8Encoding]::new($false))
}
$process=[Diagnostics.Process]::Start($info); $process.WaitForExit(); $code=$process.ExitCode
[pscustomobject]@{pid=$process.Id;exit_code=$code;editor_mode=[bool]$EditorMode;fallback=[bool]$Fallback;route=$Route;exe=$info.FileName;project=$ProjectPath} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output ($Name+'.exit.json')) -Encoding utf8
Write-Output "P5 $Name exit=$code"
exit $code
