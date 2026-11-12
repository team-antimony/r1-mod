# backend

The local Demonware backend for the CoD Online 3.3.3.3 QA client: HTTPS auth on `127.0.0.1:443`, the lobby on
`127.0.0.1:3074`, STUN on `127.0.0.1:8000`, rooms and the StartGame push, publisher files, and a server list master
on `127.0.0.1:20810`. It is loopback-only and stands in for Tencent's services; it is not an account service.

The r1-mod launcher starts it (`backend_cmd` in `r1-mod.ini`) when nothing listens on 443 yet.

## Set up once

From this folder, with your unpacked QA game folder and CoD Online 1.1.1.13 install:

```
uv run --with cryptography python make_ca.py <game_dir>
uv run python make_playlists.py <1.1.1.13 install>
```

1. `make_ca.py` makes a local CA and the HTTPS certificate (`pki\`) and copies the CA over the game's `cacert.pem`
   (the original is kept as `cacert.pem.orig`), so the client trusts the local auth.
2. `make_playlists.py` builds the QA playlist publisher file from the 1.1.1.13 reference playlist
   (`publisher\mp_playlists_shipRetail_release.info`); `--solo` lets one player start a room, `--maps` keeps only
   maps both builds ship.
3. The menu-lock publisher file (`publisher\configs_s.info`, which greys out the PvE/co-op/store entry points) is
   served as-is when you place one there; r1-mod does not generate it.

`pki\`, `publisher\`, `data\` (profiles), `state\` (the native server's home) and `logs\` are made locally and are
never committed.

## Run

```
uv run --with cryptography python -m local_auth.server --player <name> --lobby-port 3074 --stun-port 8000
```

A name selects a local profile (`data\profiles.sqlite3`); it is not a password. Do not expose or port-forward this
service. No Tencent account credentials are read.

The master: game servers send `heartbeat <game>` once a minute (the native server does with
`+set sv_master1 127.0.0.1:20810`, as `run_native.ps1` sets) and `heartbeat flatline` when they stop; a server is
dropped after 200 s without one. `getservers` returns the live addresses; r1-mod's server browser asks each server
for its map and players. `--master-port 0` turns it off; `python -m local_auth.master` runs it alone.

## Tests

```
uv run --with cryptography python -m unittest discover -s tests -v
```

The HTTPS test needs `pki\` (run `make_ca.py` first); it is skipped otherwise.
