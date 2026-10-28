# Run the AUTOTEST viewer in Citra and capture its window every 2 s.
#
# Two ways to show Citra (the user picks, per machine or per run):
#   watch      (default) the window opens normally, in front, the two screens side by side across
#              the desktop: for watching a test run.
#   background Citra opens minimized (shown inactive and behind other windows when -Shots takes
#              screenshots) and any focus it grabs goes straight back, so runs from any session never
#              take the keyboard, mouse or screen while the user works. On for every run:
#              tools\citra\background.ps1 on (a flag file next to Citra's config); for one run:
#              -Background. -Watch overrides the flag for one run.
#   powershell -ExecutionPolicy Bypass -File tools\citra\autotest.ps1 -Dir build\shots [-Shots 54]
# Then: python tools\contact_sheet.py build\sheet.png --dir build\shots --from 40 --to 52
#
# Battle tests judge themselves into a log, so they need no screenshots:
#   ... autotest.ps1 -Fast -Shots 0      # unthrottled, wait for the app to exit
# and read build\cr_autotest.log (copied from Citra's SD card). -Fast is
# safe for the results: the game steps once per frame, not by the clock, so
# only the on-screen fps reading changes.
#
# For the run only, Citra shows the two screens side by side (layout_option 3)
# in a window as wide as the desktop, so a shot is mostly 3DS pixels; -Fast
# also lifts the speed limit (frame_limit 0) and turns vsync off. The
# user's qt-config.ini is copied first and put back afterwards; Citra is
# killed rather than closed, so it never writes its own settings over it.
param([string]$Dir = 'build\shots', [int]$Shots = 72, [switch]$Fast, [int]$Timeout = 600, [string]$Pad = '', [string]$Replay = '',
      [string]$Root = 'C:\Users\Alexander\AppData\Roaming\EmuDeck\Emulators\citra', [string]$Exe = 'citra-qt',
      [switch]$Background, [switch]$Watch)
# -Root/-Exe: a private portable Citra copy with a renamed exe (e.g. C:\Users\Alexander\citra-thp,
# citra-thp), so parallel sessions share no config, sdmc or log and do not stop each other's process.
# -Pad '360 366 100; 420 426 100': controller 1 buttons (hex, PAD_BUTTON_*) held for
# retraces [from, to); written to sdmc:/cr_pad_script.txt for runtime/pad.c.
# -Replay <log>: the "# pad r<n> ..." lines of a played log are replayed from
# sdmc:/cr_pad_replay.txt (same retraces, both controllers, sticks).
$sp = Split-Path $MyInvocation.MyCommand.Path
$repo = Resolve-Path "$sp\..\.."
New-Item -ItemType Directory -Force $Dir | Out-Null
Get-Process $Exe -ErrorAction SilentlyContinue | Stop-Process -Force
$root = $Root
$citra = Join-Path $root "$Exe.exe"
$bg = $Background -or ((Test-Path (Join-Path $root 'user\custom-robo-background.txt')) -and -not $Watch)
$config = Join-Path $root 'user\config\qt-config.ini'
$backup = "$config.autotest-bak"
$sdLog = Join-Path $root 'user\sdmc\cr_autotest.log'
$app = Join-Path $repo 'custom-robo-recomp.3dsx'

# A backup left by an interrupted run is the user's real config: restore it first.
# (an empty backup is not a config: an interrupted copy left one on 2026-10-28; restoring it would wipe the settings)
if ((Test-Path $backup) -and (Get-Item $backup).Length -gt 0) { Copy-Item $backup $config -Force }
Copy-Item $config $backup -Force
Remove-Item $sdLog -Force -ErrorAction SilentlyContinue
$padFile = Join-Path $root 'user\sdmc\cr_pad_script.txt'
Remove-Item $padFile -Force -ErrorAction SilentlyContinue
$replayFile = Join-Path $root 'user\sdmc\cr_pad_replay.txt'
Remove-Item $replayFile -Force -ErrorAction SilentlyContinue
if ($Replay) { Select-String -Path $Replay -Pattern '^# pad r' | ForEach-Object { $_.Line } | Set-Content -Encoding ascii $replayFile }
# ndspInit needs sdmc:/3ds/dspfirm.cdc; Citra's HLE DSP never runs it, so a
# placeholder is enough there (a real 3DS needs the user's own DSP1 dump).
$firm = Join-Path $root 'user\sdmc\3ds\dspfirm.cdc'
if (-not (Test-Path $firm)) { New-Item -ItemType Directory -Force (Split-Path $firm) | Out-Null; [IO.File]::WriteAllBytes($firm, (New-Object byte[] 4096)) }
# the mix is dumped to sdmc:/cr_audio.wav only when this flag exists (runtime/ax.c)
[IO.File]::WriteAllText((Join-Path $root 'user\sdmc\cr_audio_dump.txt'), 'on')
# always write a pad script: without one (or a replay) the runtime has no run length (runtime/vi.c)
if ($Pad) { [IO.File]::WriteAllText($padFile, (($Pad -split ';') | ForEach-Object { $_.Trim() }) -join "`n") }
elseif (-not $Replay) { [IO.File]::WriteAllText($padFile, '') }
$start = Get-Date
try {
    $text = [IO.File]::ReadAllText($config)
    $text = $text -replace '(?m)^layout_option\\default=true', 'layout_option\default=false'
    $text = $text -replace '(?m)^layout_option=\d+', 'layout_option=3'
    if ($Fast) {
        $text = $text -replace '(?m)^frame_limit\\default=true', 'frame_limit\default=false'
        $text = $text -replace '(?m)^frame_limit=\d+', 'frame_limit=0'
        $text = $text -replace '(?m)^use_vsync_new\\default=true', 'use_vsync_new\default=false'
        $text = $text -replace '(?m)^use_vsync_new=true', 'use_vsync_new=false'
    }
    [IO.File]::WriteAllText($config, $text, (New-Object Text.UTF8Encoding $false))

    # Background mode: Citra opens without taking focus, so runs never pull keyboard, mouse or the
    # window away from whatever the user is doing: minimized when no screenshots are taken (fb dumps
    # and the log come from the app itself), else shown inactive and sent to the back. Watch mode:
    # a normal window in front. ShowWindow takes the STARTUPINFO show state on the first call, so
    # CreateProcess rather than Start-Process.
    Add-Type @'
using System; using System.Runtime.InteropServices;
public static class BgLaunch {
 [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct SI { public int cb; public string r, d, t; public int x, y, w, h, cx, cy, fill, flags; public short show, r2; public IntPtr r3, i, o, e; }
 [StructLayout(LayoutKind.Sequential)] struct PI { public IntPtr p, t; public int pid, tid; }
 [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool CreateProcess(string app, System.Text.StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, int flags, IntPtr env, string dir, ref SI si, out PI pi);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 public static int Start(string exe, string args, short show) {
  var si = new SI(); si.cb = Marshal.SizeOf(si); si.flags = 1; si.show = show;   // STARTF_USESHOWWINDOW
  PI pi; if (!CreateProcess(exe, new System.Text.StringBuilder("\"" + exe + "\" " + args), IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, System.IO.Path.GetDirectoryName(exe), ref si, out pi)) throw new System.ComponentModel.Win32Exception();
  CloseHandle(pi.p); CloseHandle(pi.t); return pi.pid; }
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
 [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] static extern bool AttachThreadInput(int a, int b, bool on);
 [DllImport("kernel32.dll")] static extern int GetCurrentThreadId();
 // Qt activates its window even when started minimized: if pid holds the foreground, give it back
 // to prev (sharing the foreground thread's input state is what lets a background process do that)
 public static bool GiveBack(int pid, IntPtr prev) {
  IntPtr fg = GetForegroundWindow(); int p; int tid = GetWindowThreadProcessId(fg, out p);
  if (p != pid || prev == IntPtr.Zero) return false;
  int me = GetCurrentThreadId(); AttachThreadInput(me, tid, true); SetForegroundWindow(prev); AttachThreadInput(me, tid, false);
  return true; } }
'@
    $show = if (-not $bg) { 1 } elseif ($Shots -gt 0) { 4 } else { 7 }   # SW_SHOWNORMAL / SW_SHOWNOACTIVATE / SW_SHOWMINNOACTIVE
    $prev = [BgLaunch]::GetForegroundWindow()
    $cpid = [BgLaunch]::Start($citra, "`"$app`"", $show)
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 500
        $p = Get-Process $Exe -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0
        if ($p) { break }
    }
    if ($bg) { for ($i = 0; $i -lt 40; $i++) { [BgLaunch]::GiveBack($cpid, $prev) | Out-Null; Start-Sleep -Milliseconds 50 } }
    # Side by side is 720 x 240, 3:1; the extra height is the menu and status bars.
    Add-Type -AssemblyName System.Windows.Forms
    $area = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    $w = $area.Width
    $h = [Math]::Min($area.Height, [int]($w / 3) + 90)
    if (-not $bg) { & "$sp\capture.ps1" -Name $Exe -Place -Width $w -Height $h | Out-Null }
    elseif ($Shots -gt 0) { & "$sp\capture.ps1" -Name $Exe -SendToBack -Width $w -Height $h | Out-Null }
    if ($Shots -le 0) {
        # No screenshots: wait for the app to write its closing "# DONE" line
        # (Citra itself stays open when the app returns, so its process is no
        # signal), then close Citra here.
        $finished = $false
        while ((Get-Process $Exe -ErrorAction SilentlyContinue) -and ((Get-Date) - $start).TotalSeconds -lt $Timeout) {
            Start-Sleep -Milliseconds 500
            if ($bg) { [BgLaunch]::GiveBack($cpid, $prev) | Out-Null }
            if ((Test-Path $sdLog) -and (Select-String -Path $sdLog -Pattern '^# DONE' -Quiet)) { $finished = $true; break }
        }
        if (-not $finished) { "no DONE line after {0:N0} s" -f ((Get-Date) - $start).TotalSeconds }
    }
    for ($i = 0; $i -lt $Shots; $i++) {
        if (-not (Get-Process $Exe -ErrorAction SilentlyContinue)) { "citra exited at shot $i"; break }
        & "$sp\capture.ps1" -Name $Exe -Out (Join-Path $Dir ('shot_{0:D2}.png' -f $i)) | Out-Null
        Start-Sleep -Seconds 2
    }
} finally {
    Get-Process $Exe -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    Copy-Item $backup $config -Force
    Remove-Item $backup -Force
    # leftover script/replay would cap and drive the user's own Citra play (runtime/vi.c, pad.c)
    Remove-Item $padFile, $replayFile -Force -ErrorAction SilentlyContinue
}
if (Test-Path $sdLog) { Copy-Item $sdLog (Join-Path $repo 'build\cr_autotest.log') -Force }
"done in {0:N0} s" -f ((Get-Date) - $start).TotalSeconds
