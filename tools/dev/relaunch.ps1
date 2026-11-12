# Rebuild, restart the client through the r1-mod launcher (no connect). Start the server separately with run_native.ps1.
$root = Split-Path (Split-Path $PSScriptRoot)
$b = "$root\build\bin\Win32\Release"
$game = (Select-String -Path "$b\r1-mod.ini" -Pattern '^game_dir=(.+)$' -ErrorAction SilentlyContinue | Select -First 1).Matches.Groups[1].Value
# Only the 3.3.3.3 QA client: a 2.6.9.7 client may be running under the same name.
Get-Process codoMP* -ErrorAction SilentlyContinue | Where-Object { $game -and $_.Path -like "$game\*" } | Stop-Process -Force
Start-Sleep 2
cmd /c "`"$root\build.bat`"" 2>&1 | Select-String -Pattern 'error' | ForEach-Object { $_.Line }
Remove-Item "$b\r1-mod.log","$b\r1-mod_in.txt" -ErrorAction SilentlyContinue
# not -Wait: that also waits for the game, which the launcher leaves running
$p = Start-Process "$b\r1-mod.exe" -ArgumentList '--no-wait','--skip-intro' -WorkingDirectory $b -WindowStyle Hidden -RedirectStandardOutput "$b\launch.out" -RedirectStandardError "$b\launch.err" -PassThru
$p.WaitForExit(90000) | Out-Null
Get-Content "$b\launch.out"
$t = 0
while (-not (Test-Path "$b\r1-mod.log") -and $t -lt 150) { Start-Sleep 2; $t += 2 }
Start-Sleep 3
'relaunched'
