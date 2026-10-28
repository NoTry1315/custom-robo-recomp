# Turn Citra's background mode on or off for every test run on this machine, from any session
# (tools\citra\autotest.ps1, and so replay.ps1, read the flag at each launch):
#   powershell -ExecutionPolicy Bypass -File tools\citra\background.ps1 on       # working: keep Citra out of the way
#   powershell -ExecutionPolicy Bypass -File tools\citra\background.ps1 off      # watching: Citra opens in front
#   powershell -ExecutionPolicy Bypass -File tools\citra\background.ps1          # show the current mode
# The flag is a file next to Citra's config, not in sdmc (the game never sees it). One run can
# still pick the other mode: replay.ps1 / autotest.ps1 -Background or -Watch.
param([ValidateSet('on', 'off', 'status')][string]$Mode = 'status',
      [string]$Root = 'C:\Users\Alexander\AppData\Roaming\EmuDeck\Emulators\citra')
$flag = Join-Path $Root 'user\custom-robo-background.txt'
if ($Mode -eq 'on') { [IO.File]::WriteAllText($flag, "Citra test runs start minimized and give focus back (tools\citra\background.ps1)`n") }
if ($Mode -eq 'off') { Remove-Item $flag -Force -ErrorAction SilentlyContinue }
if (Test-Path $flag) { 'background: Citra test runs start minimized and give focus back' } else { 'watch: Citra test runs open in front' }
