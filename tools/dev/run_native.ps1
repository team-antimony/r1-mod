param([string]$Map = 'mp_killhouse_sh', [string]$GameType = 'dm', [int]$Bots = 0, [switch]$Build,
      [string]$Native = '', [switch]$Iw4SpawnRaise, [string]$C1Install = '', [string]$QaGame = '', [string]$Backend = '',
      [int]$SvFps = 60, [int]$Port = 28960, [string]$HomeDir = '', [switch]$KeepOthers, [switch]$NoMaster,
      [string]$LogName = 'native', [string]$ServerCfg = '',
      [string]$RconPassword = $(if ($env:R1MOD_RCON_PASSWORD) { $env:R1MOD_RCON_PASSWORD } else { 'r1modlocal' }))
# Dev loop: start the native QA server (r1-server) on 127.0.0.1:28960 next to a client on this PC, with debug
# probes; log to build\native.log. To host for other players use r1-server's start-server.cmd instead.
# Server data comes from this repository's data\ folder (tools\data\make_all.ps1 or setup_data.ps1 fills it).
# -Build: relink bin\r1-server.exe (older checkouts: codol_dedrun.exe) after the old server stops (the running exe is locked); the previous exe
# is kept in bin\rollback and restored if the link fails, so a server always comes back up.
# Only the native QA build: a 2.6.9.7 server (bin2697) has the same process name.
# -Native: the server checkout (r1-server, released separately); default server_dir in r1-mod.ini, else the folder
# r1-server next to this repository
# -SvFps: server frames per second (default 60; the engine's and IW4's own rate is 20)
# -Port / -HomeDir / -LogName: a second server beside a running one (tools\dev\map_check.ps1): another UDP port, its
# own home folder (qconsole.log, missingasset.csv) and build\<LogName>.log; -KeepOthers leaves running servers alone,
# -NoMaster keeps it off the server list. The started server's pid is printed as pid=<n>.
# -Iw4SpawnRaise: codol_iw4SpawnRaise 1, the IW4 first raise on spawn instead of C1's ready gun (players only)
# -C1Install: your CoD Online 1.1.1.13 install (CODOL_Client, CODOL_Server\dedicated); default c1_install in r1-mod.ini
# (build\bin\Win32\Release), else $env:R1MOD_C1_INSTALL
# -QaGame: your unpacked QA 3.3.3.3 game folder (its zones are mounted for QA-only maps); default game_dir in r1-mod.ini
# -Backend: the Demonware backend, whose state\ holds the server home; default backend_dir in r1-mod.ini, else backend\
# -RconPassword: default $env:R1MOD_RCON_PASSWORD, else r1modlocal (r1-mod.ini rcon_password must match). The server
# listens on 127.0.0.1 only; set another password before binding it to a reachable address.
# -ServerCfg: a server configuration (r1-server tools\server.cfg.example) run after these settings, as a dedicated
# host's server.cfg is: copied to <home>\main\server.cfg and run with +exec, so its name, message of the day, limits,
# developer and logfile win. rcon_password stays -RconPassword so r1-mod and the admin tools keep their access.
$root = Split-Path (Split-Path $PSScriptRoot)
$ini = "$root\build\bin\Win32\Release\r1-mod.ini"
function IniValue([string]$key) {
    if (-not (Test-Path $ini)) { return '' }
    $m = Select-String -Path $ini -Pattern "^$key=(.+)$" | Select-Object -First 1
    if ($m) { $m.Matches[0].Groups[1].Value.Trim() } else { '' }
}
if (-not $Native) { $Native = IniValue 'server_dir' }
if (-not $Native) { $Native = Join-Path (Split-Path $root) 'r1-server' }
if (-not (Test-Path "$Native\bin\r1-server.exe") -and -not (Test-Path "$Native\bin\codol_dedrun.exe") -and
    -not ($Build -and (Test-Path "$Native\tools\BUILD-CODOL.ps1"))) {
    "no r1-server at ${Native}: give -Native (or server_dir in r1-mod.ini) an r1-server checkout. r1-server is released separately."
    exit 1
}
if (-not $C1Install) { $C1Install = IniValue 'c1_install' }
if (-not $C1Install) { $C1Install = $env:R1MOD_C1_INSTALL }
if (-not $QaGame) { $QaGame = IniValue 'game_dir' }
if (-not $Backend) { $Backend = IniValue 'backend_dir' }
if (-not $Backend) { $Backend = "$root\backend" }
if (-not $C1Install -or -not $QaGame) {
    'set -C1Install (or c1_install in r1-mod.ini) and -QaGame (or game_dir in r1-mod.ini)'
    exit 1
}
$data = "$root\data"
$nat=$Native.TrimEnd('\')
$exe="$nat\bin\r1-server.exe"
if (-not (Test-Path $exe) -and (Test-Path "$nat\bin\codol_dedrun.exe")) { $exe="$nat\bin\codol_dedrun.exe" }
if ($Build) {
    New-Item -ItemType Directory -Force "$nat\bin\rollback" | Out-Null
    $backup = "$nat\bin\rollback\$([IO.Path]::GetFileNameWithoutExtension($exe))_$(Get-Date -Format 'yyyyMMdd_HHmmss').exe"
    Copy-Item $exe $backup
}
if (-not $KeepOthers) {
    Get-Process r1-server, codol_dedrun -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$nat\*" } | Stop-Process -Force
}
if ($Build) {
    Start-Sleep 2
    powershell -ExecutionPolicy Bypass -File "$nat\tools\BUILD-CODOL.ps1" -Jobs 8 2>&1 | Select-String -Pattern ' error|undefined reference' | ForEach-Object Line
    if (-not (Test-Path $exe)) { Copy-Item $backup $exe; 'link failed: previous server restored' } else { 'linked' }
}
$sh1x="$C1Install\CODOL_Server\dedicated\bin"; $install="$C1Install\CODOL_Client"
$state="$Backend\state"; $scripts="$sh1x\home\codol_rawfiles"
# r1-server's own GSC (scripts\codol) over the base scripts, as its tools\start-server.ps1 runs them
$native = if (Test-Path "$nat\scripts\codol") { "$nat\scripts\codol" } else { $scripts }
if (-not $HomeDir) { $HomeDir = "$state\sh1x_home" }
Remove-Item "$HomeDir\codol_sessions.txt" -ErrorAction SilentlyContinue
$master = if ($NoMaster) { '""' } else { '127.0.0.1:20810' }
Set-Location $sh1x
$a=@('+set','codol_qaGamestateProbe','1','+set','codol_qaSnapshotProbe','1','+set','codol_qaUsermoveProbe','1',
'+set','codol_qaTypesPath',"$state\qa_cs_types.bin",'+set','codol_qaConstChecksum','3852334052',
'+set','codol_qaDumpSnapshots',"$root\build\qa_snapdump.txt",'+set','codol_qaWeaponKeysPath',"$data\qa_client_weapon_keys.tsv",'+set','codol_pushSecureProfile','0','+set','codol_qaNcsPath',"$data\qa_ncs.tsv",'+set','codol_qaAnimsPath',"$data\qa_player_anims.tsv",'+set','codol_qaGunBasesPath',"$data\qa_gun_bases.tsv",'+set','codol_qaPartsPath',"$data\qa_parts.tsv",'+set','codol_qaReplicatedPath',"$data\qa_replicated_dvars.tsv",'+set','codol_qaEntityTypes','1','+set','codol_qaWeaponTimesPath',"$data\qa_weapon_times.tsv",'+set','codol_qaWeaponPartsPath',"$data\qa_weapon_parts.tsv",'+set','codol_qaWeaponCamosPath',"$data\qa_weapon_camos.tsv",'+set','codol_qaWeaponDefaultsPath',"$data\qa_weapon_defaults.tsv",'+set','codol_qaHud','1','+set','codol_scriptErrorsFatal','0',
'+set','codol_dtlsQa','1','+set','rcon_password',$RconPassword,'+set','dedicated','1','+set','fs_basepath',$install,'+set','fs_homepath',$HomeDir,
'+set','codol_zonePath',"$install\client_shipRetail\chinese",
'+set','codol_qaZonePath',"$QaGame\client_shipRetail\chinese",
'+set','net_enabled','1','+set','net_ip','127.0.0.1','+set','net_port',"$Port",'+set','sv_master1',$master,'+set','logfile','2','+set','developer','1','+set','sv_fps',"$SvFps",
'+set','codol_scriptPath',$scripts,'+set','codol_scriptOverlay',$scripts,'+set','codol_scriptNativeOverlay',$native,'+set','codol_scriptR1Path',"$nat\scripts\r1",
'+set','codol_securityKeyPath',"$state\securitykey.bin",'+set','codol_admissionPath',"$state\game_admissions.txt",'+set','codol_admissionRequired','0','+set','codol_profilePath',"$state\profiles",
'+set','codol_zoneType','mp','+set','codol_sessionMap',$Map,'+set','g_gametype',$GameType,'+set','scr_codol_bots',"$Bots",'+set','codol_qaComposeWeapons','all','+set','codol_qaWeaponParts','5:3048','+set','codol_qaDefaultArms','fja_redsnake_ss_f_vmchar','+set','codol_qaAppearance','"1 0 0 1 1 71 54 1 0 0 0 0 0 0"','+set','scr_dm_timelimit','0','+set','scr_dm_scorelimit','0','+set','scr_war_timelimit','0','+set','scr_war_scorelimit','0',
'+set','scr_game_playerwaittime','0','+set','scr_game_matchstarttime','0','+set','codol_iw4SpawnRaise',$(if ($Iw4SpawnRaise) { '1' } else { '0' }),
'+set','codol_qaClassLayoutPath',"$data\qa_weapon_classes.tsv",'+set','codol_qaMarketplacePath',"$data\tables\mp_coMarketplaceItems.csv",'+set','codol_qaSplashTablePath',"$data\tables\mp_splashTable.csv",'+set','codol_qaOperatorsPath',"$data\qa_operators.tsv")
if ($ServerCfg) {
    if (-not (Test-Path $ServerCfg)) { "no server config at $ServerCfg"; exit 1 }
    New-Item -ItemType Directory -Force "$HomeDir\main" | Out-Null
    # every +set runs before any other startup command, so the password is kept by the config's own last line
    (Get-Content $ServerCfg) + "set rcon_password `"$RconPassword`"" | Set-Content "$HomeDir\main\server.cfg" -Encoding ascii
    $a = @('+exec','server.cfg') + $a
}
$a += @('+map',$Map)
$p = Start-Process $exe -ArgumentList $a -WorkingDirectory $sh1x -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput "$root\build\$LogName.log" -RedirectStandardError "$root\build\$LogName.err"
"pid=$($p.Id)"
