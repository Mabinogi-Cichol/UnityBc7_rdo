$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'out\rgbm-portability\coordinator\test'
$test = Join-Path $out 'test_detour_coordinator.exe'
$results = @()
foreach ($case in @('matrix','restore-fault','resume-fault')) {
    $log = Join-Path $out ($case + '.log')
    if ($case -eq 'matrix') { & $test *> $log } else { & $test $case *> $log }
    $code = $LASTEXITCODE
    if ($code -ne 0) { throw "$case failed with exit $code; log: $log" }
    $report = Get-Content -LiteralPath $log -Tail 1 | ConvertFrom-Json
    if (-not $report.passed) { throw "$case did not produce a passing report" }
    $results += [pscustomobject]@{case=$case;exit_code=$code;report=$report}
}
$missing = Join-Path $out 'missing-gate'
New-Item -ItemType Directory -Force $missing | Out-Null
if (Test-Path -LiteralPath (Join-Path $missing 'bc7_rgbm_loader_gate.dll')) { throw 'Missing-gate fixture already contains the gate DLL' }
Copy-Item -LiteralPath $test -Destination $missing
Copy-Item -LiteralPath (Join-Path $out 'bc7_rgbm_coordinator.dll') -Destination $missing
$log = Join-Path $out 'missing-gate.log'
& (Join-Path $missing 'test_detour_coordinator.exe') missing-gate *> $log
$code = $LASTEXITCODE
if ($code -ne 0) { throw "missing-gate failed with exit $code; log: $log" }
$report = Get-Content -LiteralPath $log -Tail 1 | ConvertFrom-Json
if (-not $report.passed) { throw 'missing-gate did not produce a passing report' }
$results += [pscustomobject]@{case='missing-gate';exit_code=$code;report=$report}
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
Write-Output 'Coordinator native matrix passed (4 processes)'
