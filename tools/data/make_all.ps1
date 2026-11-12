<#
Make every data\ table r1-server and r1-mod read, from your own running QA 3.3.3.3 client.

    powershell -ExecutionPolicy Bypass -File tools\data\make_all.ps1

Start the client with r1-mod and join a match first (cx_match against any r1-server, even with no tables yet):
a few tables only exist once the client has loaded a match (player animations, the configstring types, the
replicated dvars) and one needs the loadout screen to have been opened (weapon parts). Running this again later
redoes everything; a table that fails is reported and the rest still run. Reads the client's memory only.
qa_weapon_defaults.tsv has no generator; the server runs without it.
#>
$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
$steps = @(
    @('dump_stringtable.py', 'mp/coMarketplaceItems.csv'),
    @('dump_stringtable.py', 'mp/splashTable.csv')
)
# the character item tables gen_qa_operators.py reads
foreach ($cat in 'bar', 'boo', 'elp', 'eye', 'fja', 'fpa', 'han', 'hea', 'heg', 'knp', 'ves') {
    $steps += , @('dump_stringtable.py', "mp/coCharItems/$cat.csv")
}
$steps += @(
    @('dump_playerdata_def.py'),
    @('dump_qa_class_layout.py'),
    @('gen_qa_operators.py'),
    @('build_playerdata.py'),
    @('dump_client_weapons.py'),
    @('dump_client_weapon_keys.py'),
    @('dump_ncs.py'),
    @('qaguns.py'),
    @('dumptimes.py'),
    @('dumpparts.py'),
    @('dump_qa_weapon_camos.py'),
    @('dump_qa_parts.py'),
    @('dump_qa_player_anims.py'),
    @('repldvars.py', '--tsv'),
    @('dump_qa_cs_types.py')
)
$failed = @()
foreach ($s in $steps) {
    $script = Join-Path $here $s[0]
    $rest = @($s | Select-Object -Skip 1)
    Write-Host "== $($s -join ' ')"
    & uv run -q --with frida python -I $script @rest
    if ($LASTEXITCODE -ne 0) { $failed += ($s -join ' ') }
}
Write-Host ''
if ($failed) {
    Write-Host "$($failed.Count) step(s) failed; join a match (and open the loadout screen) in the client, then run again:"
    $failed | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host "data\ is complete: $(Join-Path (Split-Path (Split-Path $here)) 'data')"
