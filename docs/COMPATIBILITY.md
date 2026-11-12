# Compatibility with r1-server

r1-mod (the client) and r1-server (the dedicated server) are developed and released separately. They meet only
through the protocol below. Each side declares a **compatibility number**, `r1_compat`, raised by one whenever an item
here changes in a way the other side has to know about. A client and a server with the same number agree on every
item; with different numbers, the items added after the lower number may not work.

**r1_compat: 1**

r1-mod's number is `R1_COMPAT` in `src/common/compat.h`. `tools/check_compat.py` checks it against the line above, and
CI runs that check.

## How each side declares it

| Side | Where | Since |
|---|---|---|
| Client | the connect userinfo: `r1_compat` (the number) and `r1_version` (r1-mod's release, e.g. `2026.11.11.3`) | 1 |
| Server | its `getinfo` reply: `r1_compat` | to be added by r1-server |

Before a join (`cx_match`, the server browser) the client reads the server's `getinfo` reply. A server that declares
no number, or a different one, gets a console line; the join goes ahead either way. The server browser marks servers
on another number, and its tooltip shows each server's. `cx_compat` prints the client's number and the last
server's.

## What r1-mod expects from a server

| Item | Direction | Details | Since |
|---|---|---|---|
| Server list | client → master | `getservers 6 full empty`, answered with `getserversResponse` | 1 |
| Server info | client → server | `getinfo` reply keys read by the browser: `hostname`, `mapname`, `gametype`, `clients`, `g_humanplayers`, `sv_maxclients`, `pswrd` | 1 |
| Message of the day | client → server | `getinfo` keys `hostname` and `sv_motd`, shown on the loading screen | 1 |
| Server text colour | client → server | `getinfo` key `sv_customTextColor`, `"r g b"` from 0 to 1, for `^;` text | 1 |
| Match key | backend → server | `getmatchkey [password]`, answered with `matchkey <32 hex key> <16 hex security id>`, `matchkey refused` or `matchkey unavailable` (`backend/local_auth/matchkey.py`) | 1 |
| Match variables | server → client | one server command each, `s <name> "<value>"`: `bg_bounces`, `bg_bouncesAllAngles`, `bg_disableLandingSlowdown`, `bg_bunnyHopAuto`, `bg_playerCollision`, `bg_climbAnything`, `player_duckedSpeedScale`, `player_proneSpeedScale`, `cg_noclip_scaler`, `cg_ufo_scaler`, `bg_weaponSwapStyle`, `bg_weaponAnimFix`, `r1_clientMod` (a folder name, or empty) | 1 |
| Chat | client → server | `say "<text>"` and `say_team "<text>"` reliable commands while in a match | 1 |
| Player names | client → server | the game's base64 `name` in the userinfo. Colour codes `^0`..`^9`, `^:` (rainbow) and `^;` (server colour) may be in it. A name cannot hold `;`, so the server turns `^;` into `^<`, which the client draws as `^;` | 1 |
| Host commands | client → server, rcon | `god`, `noclip`, `ufo` `[slot]`; `endgame`; `devmap <map>` (after `g_gametype` when one is given) | 1 |
| GSC mods (`cx_mod`) | client → server, rcon | reads `codol_scriptNativeOverlay` and `codol_scriptOverlay`, sets `codol_scriptNativeOverlay "<folder>"`, then `map_restart` | 1 |
| Compatibility number | both | client: userinfo `r1_compat`, `r1_version`; server: `getinfo` `r1_compat` | 1 |

Everything else the client does (inspect animations, the stats overlay, `cx_loc`, playerdata) stays on the client and
does not depend on the server.

## Releases

| r1_compat | r1-mod | r1-server |
|---|---|---|
| 1 | 2026.11.11.3 | not declared yet (builds of 2026.11.11 implement the items above without declaring the number) |
