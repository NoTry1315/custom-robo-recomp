# One deterministic Citra replay of this checkout's custom-robo-recomp.3dsx, for before/after checks:
#   powershell -ExecutionPolicy Bypass -File tools\citra\replay.ps1 -Name battle -Tag base -Out logs\perf
#   ... -Name campaign -Tag new -Out logs\perf -Flags cr_gxthread_off,cr_vramtex_off
#   ... -Name boot -Tag look -Shots 4      (no replay: 4 window screenshots, 2 s apart, to <Out>\<Tag>-shots)
# battle   = cr_autotest_2026-10-18_rec-to-battle.log: Vs. battle from r1920 (scene 4.2 r2400-3600, idle robots)
# campaign = cr_autotest_2026-10-18_pause-ok.log: campaign scene 8.3 from r1470 to r2400
# 2v2      = data\saves\2026-10-22_pre-2v2 (memory card on) + tools\citra\pad_2v2.txt: story save -> dialogue ->
#            2v2 battle: scene 4.2 r4200-8610 (hardware run 28 r8730-13140, game 25-39 ms: the heaviest scene so far)
# Writes <Out>\<Tag>-<Name>.log and <Out>\<Tag>-<Name>-cr_fb_<frame>.bmp. battle/campaign/boot run without a
# memory card (sdmc:/cr_card_off.txt, as the recordings were made). 2v2 (or -Save <dir>) copies a save into
# sdmc:/custom-robo/save/ for the run and puts the previous files back afterwards, so the game's own save
# writes never carry over between runs. -Pad '<from> <to> <hex>; ...' adds pad script lines and -RunLen the run
# length (exploring a route), -Fb '<frame> ...' the framebuffer dump frames, -RecLog <log> replays that log's
# pad lines (e.g. a hardware log, with -Save and -Name boot). Every flag file it sets is removed afterwards. autotest.ps1 kills a running
# Citra, so several checkouts share one Citra through a lock file: use this script, not autotest.ps1
# directly, while other sessions may run.
param([Parameter(Mandatory)][ValidateSet('battle', 'campaign', 'boot', '2v2')][string]$Name, [string]$Tag = 'run',
      [string]$Out = 'logs\perf', [string[]]$Flags = @(), [int]$Shots = 0, [string]$Save = '', [string]$Pad = '',
      [int]$RunLen = 0, [string]$Fb = '', [string]$RecLog = '', [switch]$Background, [switch]$Watch)
$sp = Split-Path $MyInvocation.MyCommand.Path
$repo = Resolve-Path "$sp\..\.."
$citra = 'C:\Users\Alexander\AppData\Roaming\EmuDeck\Emulators\citra\user'
$sd = Join-Path $citra 'sdmc'
$rec = @{ battle = 'cr_autotest_2026-10-18_rec-to-battle.log'; campaign = 'cr_autotest_2026-10-18_pause-ok.log'; boot = ''; '2v2' = (Join-Path $sp 'pad_2v2.log') }
$len = @{ battle = '3600 0 0'; campaign = '2400 0 0'; boot = '2400 0 0'; '2v2' = '8600 0 0' }
$fbs = @{ battle = "2100`n2700`n3300`n"; campaign = "1500`n1980`n2300`n"; boot = ''; '2v2' = "5000`n6500`n8000`n" }
if ($Name -eq '2v2') {
    if (-not $Save) { $Save = Join-Path $repo 'data\saves\2026-10-22_pre-2v2' }
    $script = (Get-Content (Join-Path $sp 'pad_2v2.txt') | Where-Object { $_ -match '^\s*\d' }) -join ';'
    $Pad = (@($script, $Pad) | Where-Object { $_ }) -join ';'
}
if ($RunLen -gt 0) { $len[$Name] = "$RunLen 0 0" }
if ($RecLog) { $rec[$Name] = (Resolve-Path $RecLog).Path }   # -RecLog <log>: replay that log's "# pad r" lines instead
if ($Fb) { $fbs[$Name] = (($Fb -split '[ ,;]+' | Where-Object { $_ }) -join "`n") + "`n" }   # -Fb '1500 2000': framebuffer dumps
$padArg = (@($len[$Name], $Pad) | Where-Object { $_ }) -join ';'
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $repo $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$set = @('cr_fbdump') + ($Flags | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $Save) { $set += 'cr_card_off' }
$saveDir = Join-Path $sd 'custom-robo\save'
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
    if ($Save) {
        # file by file: directory creation from a sandboxed PowerShell silently fails here
        if (-not (Test-Path $saveDir)) { New-Item -ItemType Directory -Force $saveDir | Out-Null }
        if (-not (Test-Path $saveDir)) { throw "cannot create $saveDir (create it once by hand)" }
        if (Get-ChildItem $saveDir -Filter '*.replay-bak') { throw "$saveDir has *.replay-bak files: an interrupted run left the real save there; restore them first" }
        Get-ChildItem $saveDir -File | ForEach-Object { Move-Item $_.FullName "$($_.FullName).replay-bak" }
        Get-ChildItem $Save -File | ForEach-Object { [IO.File]::Copy($_.FullName, (Join-Path $saveDir $_.Name), $true) }
    }
    $a = @('-Fast', '-Timeout', '1200', '-Pad', $padArg)
    if ($rec[$Name]) { $a += @('-Replay', $(if ([IO.Path]::IsPathRooted($rec[$Name])) { $rec[$Name] } else { Join-Path $sd $rec[$Name] })) }
    if ($Shots -gt 0) { $a += @('-Shots', "$Shots", '-Dir', (Join-Path $Out "$Tag-shots")) } else { $a += @('-Shots', '0') }
    if ($Background) { $a += '-Background' }   # Citra kept out of the way for this run (autotest.ps1)
    if ($Watch) { $a += '-Watch' }
    & powershell -ExecutionPolicy Bypass -File (Join-Path $sp 'autotest.ps1') @a
    Copy-Item (Join-Path $repo 'build\cr_autotest.log') (Join-Path $Out "$Tag-$Name.log") -Force -ErrorAction SilentlyContinue
    Get-ChildItem $sd -Filter 'cr_fb_*.bmp' | ForEach-Object { Move-Item $_.FullName (Join-Path $Out "$Tag-$Name-$($_.Name)") -Force }
} finally {
    foreach ($f in $set) { Remove-Item (Join-Path $sd "$f.txt") -Force -ErrorAction SilentlyContinue }
    if ($Save -and (Test-Path $saveDir)) {
        Get-ChildItem $saveDir -File | Where-Object { $_.Name -notlike '*.replay-bak' } | Remove-Item -Force
        Get-ChildItem $saveDir -Filter '*.replay-bak' | ForEach-Object { Move-Item $_.FullName ($_.FullName -replace '\.replay-bak$', '') }
    }
    $lock.Dispose()
}
"$Tag-$Name -> $Out"
