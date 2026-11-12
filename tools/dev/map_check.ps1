param([string[]]$Maps = @(), [string]$GameType = 'war', [int]$Bots = 4, [int]$Port = 28970, [int]$SvFps = 20,
      [int]$Settle = 25, [int]$Timeout = 180, [string]$Native = '', [string]$C1Install = '', [string]$QaGame = '')
# Per-map check: load every QA multiplayer map on its own r1-server, beside any server already running (another port,
# its own home folder, off the server list), and record what the server reports while the map loads and bots play for
# -Settle seconds: load time, missing assets (missingasset.csv and "not in any mounted zone"), assets that could not be
# built, script and other errors, path-node errors, hitches, whether bots moved, CPU and memory. Writes
# build\map_check.tsv and build\map_check\<map>.log (each map's qconsole.log).
#   tools\dev\map_check.ps1 -Native <r1-server checkout> -C1Install <1.1.1.13 install> [-Maps mp_seatown,mp_rust_long]
# Without -Maps it takes every mp_*.ff of the QA game folder (game_dir in r1-mod.ini) except *_const, *_load (loading
# screens) and mp_playlists.
$root = Split-Path (Split-Path $PSScriptRoot)
$ini = "$root\build\bin\Win32\Release\r1-mod.ini"
function IniValue([string]$key) {
    if (-not (Test-Path $ini)) { return '' }
    $m = Select-String -Path $ini -Pattern "^$key=(.+)$" | Select-Object -First 1
    if ($m) { $m.Matches[0].Groups[1].Value.Trim() } else { '' }
}
if (-not $QaGame) { $QaGame = IniValue 'game_dir' }
$backend = IniValue 'backend_dir'; if (-not $backend) { $backend = "$root\backend" }
$Maps = @($Maps | ForEach-Object { $_ -split ',' } | Where-Object { $_ })  # -File passes a,b as one string
if (-not $Maps -or $Maps.Count -eq 0) {
    $Maps = Get-ChildItem "$QaGame\client_shipRetail\chinese\mp_*.ff" | ForEach-Object BaseName |
        Where-Object { $_ -notlike '*_const' -and $_ -notlike '*_load' -and $_ -ne 'mp_playlists' } | Sort-Object
}
# a home folder of its own, seeded from the running server's (scripts the server unpacks, animtrees)
$homeDir = "$env:TEMP\r1-mod-mapcheck\home"
$seed = "$backend\state\sh1x_home"
if (-not (Test-Path "$homeDir\main")) {
    New-Item -ItemType Directory -Force "$homeDir\main" | Out-Null
    foreach ($d in 'codol_animtrees', 'codol_rawfiles') { if (Test-Path "$seed\$d") { Copy-Item -Recurse "$seed\$d" $homeDir } }
    foreach ($f in 'sh1x_svr_scripts.iwd', 'q3config_server.cfg') { if (Test-Path "$seed\main\$f") { Copy-Item "$seed\main\$f" "$homeDir\main" } }
}
$outDir = "$root\build\map_check"
New-Item -ItemType Directory -Force $outDir | Out-Null
$tsv = "$root\build\map_check.tsv"
"map`tresult`tload_s`tmissing_models`tmissing_anims`tmissing_other`tprecache_skipped`tnot_built`terrors`tpathnode_errors`thitches_after_load`tbots_moved`tcpu_pct`tmem_mb`tmissing_names`tfirst_error" |
    Set-Content $tsv -Encoding utf8
$args0 = @('-ExecutionPolicy', 'Bypass', '-File', "$PSScriptRoot\run_native.ps1", '-GameType', $GameType, '-Bots', "$Bots",
           '-Port', "$Port", '-HomeDir', $homeDir, '-KeepOthers', '-NoMaster', '-LogName', 'mapcheck', '-SvFps', "$SvFps")
if ($Native) { $args0 += @('-Native', $Native) }
if ($C1Install) { $args0 += @('-C1Install', $C1Install) }
if ($QaGame) { $args0 += @('-QaGame', $QaGame) }
foreach ($map in $Maps) {
    $q = "$homeDir\main\qconsole.log"
    Remove-Item $q, "$homeDir\main\missingasset.csv" -ErrorAction SilentlyContinue
    $start = Get-Date
    # through a file, not a pipe: the server inherits the launcher's handles and would hold a pipe open
    $launchLog = "$outDir\_launch.txt"
    $l = Start-Process powershell -ArgumentList ($args0 + @('-Map', $map)) -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $launchLog -RedirectStandardError "$outDir\_launch.err"
    $l.WaitForExit()
    $out = Get-Content $launchLog -ErrorAction SilentlyContinue
    $pidLine = $out | Select-String '^pid=(\d+)' | Select-Object -First 1
    if (-not $pidLine) { "$map`tnot started`t`t`t`t`t`t`t`t`t`t`t`t`t`t$(($out | Select-Object -Last 1))" | Add-Content $tsv; continue }
    $procId = [int]$pidLine.Matches[0].Groups[1].Value
    $result = 'timeout'; $loaded = $null
    while (((Get-Date) - $start).TotalSeconds -lt $Timeout) {
        Start-Sleep 2
        $proc = Get-Process -Id $procId -ErrorAction SilentlyContinue
        $text = if (Test-Path $q) { Get-Content $q -Raw -ErrorAction SilentlyContinue } else { '' }
        if (-not $loaded -and $text -match '------- Game Initialization -------') { $loaded = Get-Date }
        if (-not $proc) { $result = 'exited'; break }
        if ($text -match 'Server crashed') { $result = 'failed'; break }
        if ($loaded -and ((Get-Date) - $loaded).TotalSeconds -ge $Settle) { $result = 'ok'; break }
    }
    $cpu = ''; $mem = ''
    $proc = Get-Process -Id $procId -ErrorAction SilentlyContinue
    if ($proc) {
        $c0 = $proc.TotalProcessorTime.TotalMilliseconds; Start-Sleep 5; $proc.Refresh()
        $cpu = [math]::Round(($proc.TotalProcessorTime.TotalMilliseconds - $c0) / 50, 1)
        $mem = [math]::Round($proc.WorkingSet64 / 1MB)
        Stop-Process -Id $procId -Force
    }
    Start-Sleep 1
    $lines = if (Test-Path $q) { Get-Content $q -ErrorAction SilentlyContinue } else { @() }
    Copy-Item $q "$outDir\$map.log" -ErrorAction SilentlyContinue
    $initAt = ($lines | Select-String -SimpleMatch '------- Game Initialization -------' | Select-Object -First 1).LineNumber
    # "CoDOL <type> '<name>' is not in any mounted zone" (once per asset and map); precacheItem of IW4 attachment-combo
    # weapon names (QA composes guns from parts instead) is counted apart; missingasset.csv also lists .gsx script caches
    $unmounted = @($lines | Select-String "CoDOL (\w+) '([^']+)' is not in any mounted zone" | ForEach-Object {
        [pscustomobject]@{ Type = $_.Matches[0].Groups[1].Value; Name = $_.Matches[0].Groups[2].Value } } | Sort-Object Type, Name -Unique)
    $models = @($unmounted | Where-Object Type -eq 'xmodel').Count
    $anims = @($unmounted | Where-Object Type -eq 'xanim').Count
    $other = @($unmounted | Where-Object { $_.Type -ne 'xmodel' -and $_.Type -ne 'xanim' }).Count
    if (Test-Path "$homeDir\main\missingasset.csv") {
        $other += @(Get-Content "$homeDir\main\missingasset.csv" | Where-Object { $_ -notmatch '\.gsx$' }).Count
    }
    $precache = @($lines | Select-String 'precacheItem: weapon .* is not in any mounted zone').Count
    $names = ($unmounted | ForEach-Object { "$($_.Type):$($_.Name)" }) -join ' ' 
    $notBuilt = @($lines | Select-String 'could not be built').Count
    $errs = @($lines | Select-String '\^1Error' | Where-Object { $_.Line -notmatch 'steam_api|Pathnode|^\^1Error:\s*$' })
    $pathnode = @($lines | Select-String 'Pathnode').Count
    $hitches = @($lines | Select-String 'Hitch warning' | Where-Object { $initAt -and $_.LineNumber -gt $initAt }).Count
    $botsMoved = @($lines | Select-String 'CoDOL stance: Training').Count -gt 0
    $loadS = if ($loaded) { [math]::Round(($loaded - $start).TotalSeconds) } else { '' }
    if ($result -eq 'exited' -and $loaded) { $result = 'exited after load' }
    $first = if ($errs.Count) { ($errs[0].Line -replace '\t', ' ').Substring(0, [math]::Min(160, $errs[0].Line.Length)) } else { '' }
    "$map`t$result`t$loadS`t$models`t$anims`t$other`t$precache`t$notBuilt`t$($errs.Count)`t$pathnode`t$hitches`t$botsMoved`t$cpu`t$mem`t$names`t$first" |
        Add-Content $tsv
    "{0,-24} {1,-18} load {2,4}s  models {3,3}  anims {4,3}  other {5,3}  errors {6,3}  bots {7}" -f $map, $result, $loadS,
        $models, $anims, $other, $errs.Count, $botsMoved
}
"written $tsv"
