# Restart the whole local stack: QA client, Demonware backend, native game server.
# Builds r1-mod first; -BuildServer also relinks the server (see run_native.ps1 -Build).
param([string]$Map = 'mp_crash_tropical', [string]$GameType = 'dm', [int]$Bots = 0, [switch]$BuildServer)
$root = Split-Path (Split-Path $PSScriptRoot)
$b = "$root\build\bin\Win32\Release"
$game = (Select-String -Path "$b\r1-mod.ini" -Pattern '^game_dir=(.+)$' -ErrorAction SilentlyContinue | Select -First 1).Matches.Groups[1].Value

# 1. client (only the 3.3.3.3 QA one; a 2.6.9.7 client has the same process name)
Get-Process codoMP* -ErrorAction SilentlyContinue | Where-Object { $game -and $_.Path -like "$game\*" } | Stop-Process -Force
# 2. backend: the local_auth.server python processes started from backend\
Get-CimInstance Win32_Process -Filter "Name like 'python%' or Name like 'uv%'" |
    Where-Object { $_.CommandLine -like '*local_auth.server*' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep 2

cmd /c "`"$root\build.bat`"" 2>&1 | Select-String -Pattern ' error' | ForEach-Object Line

# 3. game server (stops the old one itself)
$serverArgs = @{ Map = $Map; GameType = $GameType; Bots = $Bots }
if ($BuildServer) { $serverArgs.Build = $true }
& "$PSScriptRoot\run_native.ps1" @serverArgs

# 4. launcher: starts the backend (nothing listens on 443 now), then the client
Remove-Item "$b\r1-mod.log", "$b\r1-mod_in.txt" -ErrorAction SilentlyContinue
$p = Start-Process "$b\r1-mod.exe" -ArgumentList '--no-wait','--skip-intro' -WorkingDirectory $b -WindowStyle Hidden `
    -RedirectStandardOutput "$b\launch.out" -RedirectStandardError "$b\launch.err" -PassThru
$p.WaitForExit(120000) | Out-Null
Get-Content "$b\launch.out"
