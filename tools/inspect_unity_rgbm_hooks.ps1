<#
.SYNOPSIS
只读核对 Unity.exe 的版本、SHA256 和三个 RGBM Hook 入口指纹；无需启动 Unity。
.EXAMPLE
.\tools\inspect_unity_rgbm_hooks.ps1 -UnityExe 'C:\Program Files\Unity\Unity 2022.3.62f1\Editor\Unity.exe'
#>
param([Parameter(Mandatory=$true)][string]$UnityExe)
$ErrorActionPreference = 'Stop'
$taskExe = (Resolve-Path -LiteralPath $UnityExe).Path
$taskBytes = [IO.File]::ReadAllBytes($taskExe)
if ($taskBytes.Length -lt 64 -or [BitConverter]::ToUInt16($taskBytes,0) -ne 0x5A4D) { throw 'Not a PE file' }
$taskPe = [BitConverter]::ToInt32($taskBytes,0x3C)
if ($taskPe -lt 0 -or $taskPe + 24 -gt $taskBytes.Length -or [BitConverter]::ToUInt32($taskBytes,$taskPe) -ne 0x4550) { throw 'Invalid PE header' }
$taskSectionCount = [BitConverter]::ToUInt16($taskBytes,$taskPe+6)
$taskOptionalSize = [BitConverter]::ToUInt16($taskBytes,$taskPe+20)
$taskOptional = $taskPe+24
if ($taskOptionalSize -lt 60 -or $taskOptional+$taskOptionalSize+$taskSectionCount*40 -gt $taskBytes.Length) { throw 'Truncated PE headers' }
if ([BitConverter]::ToUInt16($taskBytes,$taskPe+4) -ne 0x8664 -or [BitConverter]::ToUInt16($taskBytes,$taskOptional) -ne 0x20B) { throw 'Expected x64 PE executable' }
$taskSections = @()
for ($taskIndex=0; $taskIndex -lt $taskSectionCount; $taskIndex++) {
    $taskSection = $taskOptional+$taskOptionalSize+$taskIndex*40
    $taskSections += [pscustomobject]@{
        Rva=[BitConverter]::ToUInt32($taskBytes,$taskSection+12)
        RawSize=[BitConverter]::ToUInt32($taskBytes,$taskSection+16)
        RawOffset=[BitConverter]::ToUInt32($taskBytes,$taskSection+20)
    }
}
$taskHooks = @(
    @{Name='CompressSingleImageTexture'; Rva=0x1EDC1A0; Expected='48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18'},
    @{Name='ThreadedProcessImage<BC7 JobData>'; Rva=0x1ED6540; Expected='4C 8B DC 48 81 EC 58 01 00 00 85 C9'},
    @{Name='PerformBC7TextureCompression lambda'; Rva=0x1ED9710; Expected='48 89 5C 24 08 55 56 57 41 54 41 55'}
)
$taskResults = foreach ($taskHook in $taskHooks) {
    $taskLength = ($taskHook.Expected.Split(' ')).Count
    $taskActual = $null
    foreach ($taskSection in $taskSections) {
        $taskDelta = [long]$taskHook.Rva-[long]$taskSection.Rva
        $taskOffset = [long]$taskSection.RawOffset+$taskDelta
        if ($taskDelta -ge 0 -and $taskDelta+$taskLength -le $taskSection.RawSize -and $taskOffset+$taskLength -le $taskBytes.Length) {
            $taskActual = [BitConverter]::ToString($taskBytes,[int]$taskOffset,$taskLength).Replace('-',' ')
            break
        }
    }
    [pscustomobject]@{Name=$taskHook.Name;Rva=('0x{0:X}' -f $taskHook.Rva);Expected=$taskHook.Expected;Actual=$taskActual;Match=($taskActual -eq $taskHook.Expected)}
}
$taskVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($taskExe)
[pscustomobject]@{
    UnityExe=$taskExe
    ProductVersion=$taskVersion.ProductVersion
    FileVersion=$taskVersion.FileVersion
    SHA256=(Get-FileHash -LiteralPath $taskExe -Algorithm SHA256).Hash
    PeTimestamp=('0x{0:X8}' -f [BitConverter]::ToUInt32($taskBytes,$taskPe+8))
    SizeOfImage=('0x{0:X}' -f [BitConverter]::ToUInt32($taskBytes,$taskOptional+56))
    Profile='Unity 2022.3.62f1 (4af31df58517) x64'
    AllMatch=(@($taskResults | Where-Object {-not $_.Match}).Count -eq 0)
    Hooks=@($taskResults)
} | ConvertTo-Json -Depth 4
