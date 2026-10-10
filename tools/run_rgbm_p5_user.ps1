param([string]$ProjectPath='E:\Unity\StarWings\ClientDevelopment',
      [string]$UnityExe='C:\Program Files\Unity\Hub\Editor\2022.3.62f2\Editor\Unity.exe')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$project=(Resolve-Path -LiteralPath $ProjectPath).Path
foreach($process in @(Get-CimInstance Win32_Process -Filter "Name='Unity.exe'")) {
    if($process.CommandLine -and $process.CommandLine.IndexOf($project,[StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw 'Close the validation project before replacing its DLL'
    }
}
$output=Join-Path $root 'out\rgbm-portability\acceptance'
$snapshot=Join-Path $output 'Bc7Rdo'
$plugin=Join-Path $project 'Assets\Plugins\Bc7Rdo'
$driver=Join-Path $plugin 'RgbmP5Validation.cs'
if(Test-Path -LiteralPath $driver) { throw 'A P5 driver already exists; refusing to overwrite it' }
$hdr=Join-Path $project 'Assets\GameData\GameLevels\MVPSceneTest\LMGroup_MVPSceneTest_64_01_final.hdr'
$protected=@((Join-Path $plugin 'bc7_rdo_config.txt'),$hdr,($hdr+'.meta'),
    (Join-Path $project 'ProjectSettings\EditorSettings.asset'),
    (Join-Path $project 'ProjectSettings\ProjectSettings.asset'))
$before=@($protected | ForEach-Object { [pscustomobject]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash} })
$deployFiles=@(Get-ChildItem -LiteralPath $snapshot -Recurse -File | Where-Object { $_.Extension -in @('.cs','.dll','.exe') })
$backup=Join-Path $output ('user-before-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup | Out-Null
$hook=Join-Path $plugin 'bc7_rdo_hook.dll'
foreach($file in $deployFiles) {
    $relative=$file.FullName.Substring($snapshot.Length+1)
    $saved=Join-Path $backup $relative
    New-Item -ItemType Directory -Force (Split-Path -Parent $saved) | Out-Null
    Copy-Item -LiteralPath (Join-Path $plugin $relative) -Destination $saved
}
$config=Join-Path $plugin 'bc7_rdo_config.txt'
$originalConfig=[IO.File]::ReadAllBytes($config); $configTime=[IO.File]::GetLastWriteTimeUtc($config)
$before | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'user-files-before.json') -Encoding utf8
$passed=$false
try {
    foreach($file in $deployFiles) {
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $plugin $file.FullName.Substring($snapshot.Length+1)) -Force
    }
    Copy-Item -LiteralPath (Join-Path $root 'tests\unity\RgbmP5Validation.cs') -Destination $driver
    foreach($route in @('main','worker')) {
        & (Join-Path $PSScriptRoot 'run_rgbm_p5.ps1') -UnityExe $UnityExe -ProjectPath $project -SourceHdr $hdr -Name ('user-f2-batch-'+$route) -Route $route -Pairs 1
        if($LASTEXITCODE -ne 0) { throw "Actual project $route validation exit=$LASTEXITCODE" }
    }
    $passed=$true
} finally {
    foreach($path in @($driver,($driver+'.meta'))) { if(Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path } }
    [IO.File]::WriteAllBytes($config,$originalConfig); [IO.File]::SetLastWriteTimeUtc($config,$configTime)
    if(-not $passed) {
        foreach($file in $deployFiles) {
            $relative=$file.FullName.Substring($snapshot.Length+1)
            Copy-Item -LiteralPath (Join-Path $backup $relative) -Destination (Join-Path $plugin $relative) -Force
        }
    }
    $after=@($protected | ForEach-Object { [pscustomobject]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash} })
    $unchanged=$true
    for($i=0;$i -lt $before.Count;++$i) { if($before[$i].sha256 -ne $after[$i].sha256) { $unchanged=$false } }
    $fixtures=@(Get-ChildItem -LiteralPath (Join-Path $project 'Assets') -Directory -Filter 'Bc7RdoP5_*')
    [pscustomobject]@{passed=$passed;protected_files_unchanged=$unchanged;files=$after;fixtures_remaining=$fixtures.Count;driver_removed=(-not (Test-Path -LiteralPath $driver));hook_sha256=(Get-FileHash -LiteralPath $hook).Hash;backup=$backup} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'user-cleanup.json') -Encoding utf8
    if(-not $unchanged -or $fixtures.Count) { throw 'Actual project restoration check failed' }
}
$info=[Diagnostics.ProcessStartInfo]::new()
$info.FileName=$UnityExe; $info.WorkingDirectory=$project
$info.Arguments='-batchmode -nographics -quit -projectPath "'+$project+'" -logFile "'+(Join-Path $output 'user-final-compile.log')+'"'
$info.UseShellExecute=$false; $info.CreateNoWindow=$true; $info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
$final=[Diagnostics.Process]::Start($info); $final.WaitForExit()
[pscustomobject]@{pid=$final.Id;exit_code=$final.ExitCode} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'user-final-compile.exit.json') -Encoding utf8
Write-Output "P5 actual-project cleanup complete; final compile exit=$($final.ExitCode)"
exit $final.ExitCode
