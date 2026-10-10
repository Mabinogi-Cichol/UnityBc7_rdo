param([Parameter(Mandatory=$true)][string]$UnityExe,
      [Parameter(Mandatory=$true)][string]$ProjectPath,
      [string]$Name='batch-f2-workers', [switch]$CompileOnly)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root 'out\rgbm-portability\deployment'
if ($Name -notmatch '^[a-zA-Z0-9_-]+$') { throw 'Invalid report name' }
if (Get-Process Unity -ErrorAction SilentlyContinue) { throw 'Close Unity before batch validation' }
$info=[Diagnostics.ProcessStartInfo]::new()
$info.FileName=$UnityExe
$info.Arguments='-batchmode -nographics -projectPath "'+$ProjectPath+'" -logFile "'+(Join-Path $output ($Name+'.log'))+'" '
if ($CompileOnly) { $info.Arguments+='-quit' }
else { $info.Arguments+='-refreshImportMode OutOfProcess -executeMethod Bc7Rdo.RgbmP4Validation.Run' }
$info.UseShellExecute=$false; $info.CreateNoWindow=$true; $info.WorkingDirectory=$ProjectPath
$info.EnvironmentVariables['BC7_RGBM_P4_REPORT']=Join-Path $output ($Name+'.json')
$info.EnvironmentVariables['PATH']='C:\Windows\System32;C:\Windows;C:\Windows\System32\WindowsPowerShell\v1.0'
$process=[Diagnostics.Process]::Start($info)
$process.WaitForExit(); $code=$process.ExitCode
[IO.File]::WriteAllText((Join-Path $output ($Name+'.exit.txt')),[string]$code)
Write-Output "Unity batch exit=$code"
exit $code
