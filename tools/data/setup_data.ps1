# Fill data\ (not in git: everything in it is derived from the game) from another r1-mod checkout that has one.
#   tools\data\setup_data.ps1 -From X:\old     copy from another checkout
# Files that already exist in data\ are kept unless -Force is given. To regenerate a file from the game
# instead, run the tool data\README.md lists for it.
param([Parameter(Mandatory)][string]$From, [switch]$Force)
$root = Split-Path (Split-Path $PSScriptRoot)
$data = Join-Path $root 'data'

function Copy-Set([string]$src, [string]$dst, [string[]]$patterns) {
    if (-not (Test-Path $src)) { "skip: $src not found"; return }
    New-Item -ItemType Directory -Force $dst | Out-Null
    $n = 0
    foreach ($p in $patterns) {
        foreach ($f in Get-ChildItem -Path $src -Filter $p -File) {
            $target = Join-Path $dst $f.Name
            if ($Force -or -not (Test-Path $target)) { Copy-Item $f.FullName $target; $n++ }
        }
    }
    "{0,4} files  {1} -> {2}" -f $n, $src, $dst
}

Copy-Set (Join-Path $From 'data') $data @('*.tsv', '*.json', '*.bin')
Copy-Set (Join-Path $From 'data\tables') (Join-Path $data 'tables') @('*.csv')
Copy-Set (Join-Path $From 'server') (Join-Path $data 'server') @('*.json')
