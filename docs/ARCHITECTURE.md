# r1-mod architecture

How the parts fit, for someone extending the mod. For the command and hook reference see
[CLIENT.md](CLIENT.md); for build and run see the [README](../README.md).

## The parts

```
r1-mod.exe  (launcher)   starts the backend, launches the game, injects r1-mod.dll
r1-mod.dll  (client mod)  runs inside the QA client: console, UI, gameplay settings, joining
backend/    (local Demonware)  auth, lobby, STUN, rooms, publisher files, server list master
r1-server (separate repo)  the native dedicated server a match runs on
```

The QA client normally talks to Tencent's Demonware services. Those are gone, so `backend/` stands in for them
on loopback: the launcher points the game's Demonware endpoints at `127.0.0.1`, the backend answers auth and the
lobby, and a match is played on an `r1-server`, on this PC or another one. r1-mod ships none of the game's own
files — you supply an unpacked QA 3.3.3.3 build (see the README).

## Start-up and join, at a glance

1. The launcher writes `r1-mod.ini`, points the game's Demonware config at the local backend, starts the backend,
   launches the client, and injects `r1-mod.dll` before the game's code runs.
2. `r1-mod.dll` applies the offline profile (playerdata, unlock, stats overlay), skips onboarding, and draws the
   console and server browser.
3. `cx_match` / `cx_room` / `cx_start` drive the lobby's own StartGame path. The backend first asks the target
   r1-server for its match key (`getmatchkey`, `backend/local_auth/matchkey.py`) and puts it in the StartGame; the
   client then opens its bdDTLS association to that server (`127.0.0.1:28960` by default) and the match is
   ordinary gameplay.

## Research as code

The engine was reverse-engineered; those findings live in the source, not in side notes:

- **`src/client/game/engine_map.h`** — the general engine API (commands, dvars, errors, assets, onboarding) as
  named `EA_*` address constants with a one-line comment each. Reference for anyone extending the mod.
- **`src/client/game/game.h`** — the shared engine access the mod actually calls (`VA_*` constants + helpers),
  plus the rebasing macro `A()`. Every address is an IDA address (base `0x400000`) rebased at runtime, because
  the image relocates.
- **Convention:** an address only one component uses is named in that component (e.g. the dvar-register macros in
  `component/movement.c`); genuinely shared API lives in `game.h`; the broader reference catalog is `engine_map.h`.

## Components

`src/client/component/` is one file per feature — console, overlay (Dear ImGui), server browser, movement dvars,
playerdata, unlock, stats overlay, onboarding, loose Scaleform UI, direct connect, match/room/start, fps, turn
speed, name, rcon, and the start-up plumbing. `src/client/utils/` holds call-site patching (`hook.c`) and
fault-tolerant memory reads (`memory.c`). `src/launcher/` is the exe; `src/common/version.h` the version.

## Tools and data

`tools/data` generates the tables in `data/` (most read a running client with Frida), `tools/net` decodes and
sends packets, `tools/dev` holds the test loops (including `run_native.ps1`, which starts `r1-server`).

## What is game-derived and stays out

Nothing from the game is in the repo: no executables, fastfiles, zones, Scaleform movies or decompiled AS3, and no
QA-derived tables — `data/` is regenerated from your own running client (see `data/README.md`). Unpacking the game
and anything that reads a key from, or patches, the game binary are left to the user and are not covered here.
