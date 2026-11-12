# One hands-free native-server test: restart server and client, join through the lobby backend with cx_match,
# then print the client's connection state every few seconds until it is in game, drops, or time runs out.
param([int]$Seconds = 240, [string]$Map = 'mp_killhouse_sh')
$root = Split-Path (Split-Path $PSScriptRoot)
$b = "$root\build\bin\Win32\Release"
& "$PSScriptRoot\run_native.ps1" -Map $Map
$started = Get-Date
& "$PSScriptRoot\relaunch.ps1" | Out-Null
$log = "$b\r1-mod.log"
$t = 0
while (-not (Select-String -Path $log -Pattern 'onboarding skipped' -Quiet) -and $t -lt 120) { Start-Sleep 2; $t += 2 }
Start-Sleep 5
if ($env:R1MOD_NAME) { Add-Content "$b\r1-mod_in.txt" "cx_name $($env:R1MOD_NAME)"; Start-Sleep 2 }
Add-Content "$b\r1-mod_in.txt" 'cx_match'
$end = (Get-Date).AddSeconds($Seconds); $last = ''; $active = $null
while ((Get-Date) -lt $end) {
  Start-Sleep 5
  $r = uv run --with frida python "$root\tools\data\read_cs.py" 2>&1 | Select -Last 1
  if ($r -ne $last) { "$(Get-Date -Format HH:mm:ss) $r"; $last = $r }
  if ($r -match "connState': 10") { if (-not $active) { $active = Get-Date } elseif (((Get-Date) - $active).TotalSeconds -ge 45) { break } }
  if ($active -and $r -match "connState': 0") { break }
}
'--- server'
Select-String -Path "$root\backend\state\sh1x_home\main\qconsole.log" -Pattern 'Going from|dropped|lost \d+ clientCommands|clientCommand:|QA gamestate |ClientBegin|spawn' |
  Select -Last 15 | % { $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) }
'--- client errors'
Get-Content "$b\launch.out" | Select-String com_error | Select -Last 3 | % { $_.Line.Substring(0, [Math]::Min(220, $_.Line.Length)) }
