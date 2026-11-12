# Join the native server (native_test.ps1), pick class custom1 the way the QA loadout screen does
# (cmd mr <sv_serverId> 1 custom1), then report the spawn and take an in-engine screenshot.
param([int]$Seconds = 40, [string]$Class = 'custom1', [string]$Name = '')
$root = Split-Path (Split-Path $PSScriptRoot)
$b = "$root\build\bin\Win32\Release"
$game = (Select-String -Path "$b\r1-mod.ini" -Pattern '^game_dir=(.+)$' -ErrorAction SilentlyContinue | Select -First 1).Matches.Groups[1].Value
if ($Name) { $env:R1MOD_NAME = $Name }
& "$PSScriptRoot\native_test.ps1" -Seconds $Seconds | Select -First 3
$r = py -3 "$root\tools\net\rcon.py" "sv_serverid" | Out-String
$sid = [regex]::Match($r, '"sv_serverid" is: "(\d+)').Groups[1].Value
"serverId=$sid"
Add-Content "$b\r1-mod_in.txt" "cmd mr $sid 1 $Class"
Start-Sleep 10
'--- server'
Select-String -Path "$root\backend\state\sh1x_home\main\qconsole.log" -Pattern 'menuresponse|spawn:|dropped|runtime error|could not|Server Shutdown' |
  Select -Last 8 | % { $_.Line.Substring(0, [Math]::Min(170, $_.Line.Length)) }
'--- client errors'
Get-Content "$b\launch.out" | Select-String com_error | Select -Last 2 | % { $_.Line.Substring(0, [Math]::Min(220, $_.Line.Length)) }
Add-Content "$b\r1-mod_in.txt" 'screenshotJpeg'
Start-Sleep 4
Get-ChildItem "$game\main\screenshots" | Sort LastWriteTime -Desc | Select -First 1 -Expand FullName
