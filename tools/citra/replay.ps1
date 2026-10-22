# One deterministic Citra replay of this checkout's custom-robo-recomp.3dsx, for before/after checks:
#   powershell -ExecutionPolicy Bypass -File tools\citra\replay.ps1 -Name battle -Tag base -Out logs\perf
#   ... -Name campaign -Tag new -Out logs\perf -Flags cr_gxthread_off,cr_mtx_off
#   ... -Name boot -Tag look -Shots 4      (no replay: 4 window screenshots, 2 s apart, to <Out>\<Tag>-shots)
# battle   = cr_autotest_2026-10-18_rec-to-battle.log: Vs. battle from r1920 (scene 4.2 r2400-3600, idle robots)
# campaign = cr_autotest_2026-10-18_pause-ok.log: campaign scene 8.3 from r1470 to r2400
# Writes <Out>\<Tag>-<Name>.log and <Out>\<Tag>-<Name>-cr_fb_<frame>.bmp. Always without a memory card
# (sdmc:/cr_card_off.txt, as the recordings were made): the Citra save folder is never touched. Every flag
# file it sets is removed afterwards. autotest.ps1 kills a running Citra, so several checkouts share one
# Citra through a lock file: use this script, not autotest.ps1 directly, while other sessions may run.
param([Parameter(Mandatory)][ValidateSet('battle', 'campaign', 'boot')][string]$Name, [string]$Tag = 'run',
      [string]$Out = 'logs\perf', [string[]]$Flags = @(), [int]$Shots = 0)
$sp = Split-Path $MyInvocation.MyCommand.Path
$repo = Resolve-Path "$sp\..\.."
$citra = 'C:\Users\Alexander\AppData\Roaming\EmuDeck\Emulators\citra\user'
$sd = Join-Path $citra 'sdmc'
$rec = @{ battle = 'cr_autotest_2026-10-18_rec-to-battle.log'; campaign = 'cr_autotest_2026-10-18_pause-ok.log'; boot = '' }
$len = @{ battle = '3600 0 0'; campaign = '2400 0 0'; boot = '2400 0 0' }
$fbs = @{ battle = "2100`n2700`n3300`n"; campaign = "1500`n1980`n2300`n"; boot = '' }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $repo $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$set = @('cr_card_off', 'cr_fbdump') + ($Flags | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
# exclusive handle on a lock file for the whole run; the OS drops it if this process dies
$lock = $null
for ($i = 0; -not $lock; $i++) {
    try { $lock = [IO.File]::Open((Join-Path $citra 'custom-robo-citra.lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
    catch { if ($i % 30 -eq 0) { 'waiting for Citra: another replay holds the lock' }; Start-Sleep -Seconds 2 }
}
try {
    Get-ChildItem $sd -Filter 'cr_fb_*.bmp' | Remove-Item -Force
    foreach ($f in $set) { [IO.File]::WriteAllText((Join-Path $sd "$f.txt"), '') }
    [IO.File]::WriteAllText((Join-Path $sd 'cr_fbdump.txt'), $fbs[$Name])
    $a = @('-Fast', '-Timeout', '1200', '-Pad', $len[$Name])
    if ($rec[$Name]) { $a += @('-Replay', (Join-Path $sd $rec[$Name])) }
    if ($Shots -gt 0) { $a += @('-Shots', "$Shots", '-Dir', (Join-Path $Out "$Tag-shots")) } else { $a += @('-Shots', '0') }
    & powershell -ExecutionPolicy Bypass -File (Join-Path $sp 'autotest.ps1') @a
    Copy-Item (Join-Path $repo 'build\cr_autotest.log') (Join-Path $Out "$Tag-$Name.log") -Force -ErrorAction SilentlyContinue
    Get-ChildItem $sd -Filter 'cr_fb_*.bmp' | ForEach-Object { Move-Item $_.FullName (Join-Path $Out "$Tag-$Name-$($_.Name)") -Force }
} finally {
    foreach ($f in $set) { Remove-Item (Join-Path $sd "$f.txt") -Force -ErrorAction SilentlyContinue }
    $lock.Dispose()
}
"$Tag-$Name -> $Out"
