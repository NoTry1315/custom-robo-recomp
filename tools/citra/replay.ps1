# One deterministic Citra replay of this checkout's custom-robo-recomp.3dsx, for before/after checks:
#   powershell -ExecutionPolicy Bypass -File tools\citra\replay.ps1 -Name battle -Tag base -Out logs\perf
#   ... -Name campaign -Tag new -Out logs\perf -Flags cr_gxthread_off,cr_mtx_off
# battle   = cr_autotest_2026-10-18_rec-to-battle.log: Vs. battle from r1920 (scene 4.2 r2400-3600, idle robots)
# campaign = cr_autotest_2026-10-18_pause-ok.log: campaign scene 8.3 from r1470 to r2400
# Writes <Out>\<Tag>-<Name>.log and <Out>\<Tag>-<Name>-cr_fb_<frame>.bmp. Always without a memory card
# (sdmc:/cr_card_off.txt, as the recordings were made): the Citra save folder is never touched. Every flag
# file it sets is removed afterwards. Citra runs one at a time (autotest.ps1 kills a running Citra).
param([Parameter(Mandatory)][ValidateSet('battle', 'campaign')][string]$Name, [string]$Tag = 'run',
      [string]$Out = 'logs\perf', [string[]]$Flags = @())
$sp = Split-Path $MyInvocation.MyCommand.Path
$repo = Resolve-Path "$sp\..\.."
$sd = 'C:\Users\Alexander\AppData\Roaming\EmuDeck\Emulators\citra\user\sdmc'
$rec = @{ battle = 'cr_autotest_2026-10-18_rec-to-battle.log'; campaign = 'cr_autotest_2026-10-18_pause-ok.log' }
$len = @{ battle = '3600 0 0'; campaign = '2400 0 0' }
$fbs = @{ battle = "2100`n2700`n3300`n"; campaign = "1500`n1980`n2300`n" }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $repo $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$set = @('cr_card_off', 'cr_fbdump') + ($Flags | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
try {
    Get-ChildItem $sd -Filter 'cr_fb_*.bmp' | Remove-Item -Force
    foreach ($f in $set) { [IO.File]::WriteAllText((Join-Path $sd "$f.txt"), '') }
    [IO.File]::WriteAllText((Join-Path $sd 'cr_fbdump.txt'), $fbs[$Name])
    & powershell -ExecutionPolicy Bypass -File (Join-Path $sp 'autotest.ps1') -Fast -Shots 0 -Timeout 1200 -Replay (Join-Path $sd $rec[$Name]) -Pad $len[$Name]
    Copy-Item (Join-Path $repo 'build\cr_autotest.log') (Join-Path $Out "$Tag-$Name.log") -Force
    Get-ChildItem $sd -Filter 'cr_fb_*.bmp' | ForEach-Object { Move-Item $_.FullName (Join-Path $Out "$Tag-$Name-$($_.Name)") -Force }
} finally {
    foreach ($f in $set) { Remove-Item (Join-Path $sd "$f.txt") -Force -ErrorAction SilentlyContinue }
}
"$Tag-$Name -> $Out"
