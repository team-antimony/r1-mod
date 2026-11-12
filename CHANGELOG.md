# Changelog

Versions are release dates: `YYYY.MM.DD`, tagged `vYYYY.MM.DD` (a second release that day adds `.2`, `.3`, ...).
Between releases `src/common/version.h` holds the last release's date with `-dev`.

## 2026.11.11.3

- Message of the day: when a join starts (`cx_match`, the server browser) r1-mod asks the server for its name and
  `sv_motd`: the message replaces the loading screen's "did you know" line above the loading bar, and the name and
  message fill the first tip of its intel panel (F1), Plutonium / TeknoMW3 style. Colour codes show as colours;
  every other `^` sequence (which could embed an icon), control characters and a leading `@` are removed before the
  text reaches the UI. The message also goes to the console. Nothing the server sends is drawn by r1-mod itself.
- IW4x-style text colours: `^:` is a rainbow (one turn every 3 seconds) and `^;` is the server's colour
  (`sv_customTextColor`, read when a join starts). They work in player names, chat, the message of the day and the
  rest of the game's text; `^0`..`^9` are unchanged. A name cannot hold `;`, so r1-server turns a name's `^;` into
  `^<`, which r1-mod draws the same way.
- Fixed: the scoreboard and the rest of the Scaleform UI showed colour codes as characters (`^<pla^:yer`); they are
  removed from the text it draws.
- Fixed: with the stats overlay on, the lobby header showed level 1 while everything else showed 70. The overlay
  now applies as soon as the profile arrives, before the header reads the level.
- `cx_loc`: your own translation of the game's text. Put `r1_english.txt` (`KEY<TAB>text` lines, `\n` escapes, `#`
  comments) in the client mod's folder or next to `r1-mod.dll`, and each string the game loads takes its text,
  again whenever a map brings new strings. `cx_loc reload`, `cx_loc apply`, `cx_loc <KEY>` shows one string.
  r1-mod ships no game text and no translation.
- `tools\dev\run_native.ps1 -ServerCfg <file>` starts the server with a dedicated host's configuration (r1-server's
  `tools\server.cfg.example`) as `server.cfg`, run with `+exec`; the rcon password stays the script's. The server
  checkout can be set once as `server_dir` in `r1-mod.ini`, and the script says so when it finds none.
- Compatibility number: `docs\COMPATIBILITY.md` lists everything r1-mod expects from a server, and `r1_compat` (1 in
  this release) names that set. The client sends `r1_compat` and `r1_version` in its connect userinfo and reads the
  server's `r1_compat` from its info reply before a join: a server on another number, or one that declares none,
  gets a console line, and the browser marks it. `cx_compat` prints both.
- Removed `tools\data\make_quick_xanims.ps1`, which drove r1-server's exporter over your 1.1.1.13 zones;
  `iw4_quick_xanims.bin` comes from r1-server's tools. `NOTICE.md` lists the third-party code.
- Known issues: renaming a class in the loadout menu does not keep the new name; while spectating before choosing a
  class, the followed player's calling card shows only the rank emblem and title, without the name.

## 2026.11.11.2

- Fixed: 2026.11.11 dropped five console commands at start (`cx_swapstyle`, `cx_movement`, `cx_operator`,
  `cx_yawspeed`, `cx_pitchspeed`), because r1-mod's command table held 32 and it registers more. Without
  `cx_swapstyle`, `bg_weaponSwapStyle`, `bg_weaponAnimFix` and the IW4 quick clips were never set up. The table now
  holds 96.
- Weapon inspect: QA's inspect key (or `cx_inspect`) plays the held gun's inspect animation, with Modern Warfare
  Remastered's rules: only with the gun ready and not aimed, one at a time, ended by aiming or by any other gun
  action. QA ships no inspect animations for its guns, so the clips come from `r1_xanims_319.bin` (next to the DLL or
  in the client mod's folder): inspect animations from your own 3.19.26.22 install. r1-mod includes neither the
  clips nor a tool to make them; without the file the key does nothing. Client side only: other players do not see
  an inspect, and it has no sound yet.
- `tools\dev\vm_slots.py` lists the clips in the view-model animation slots of the running client;
  `tools\dev\find_weapon_anim.py` finds which animation of a weapon names a clip.

## 2026.11.11

Goes with r1-server 2026.11.11, which adds killcams, chat, host commands, IW4M-Admin support and the net code fixes
on the server side.

- Killcams: the client enters its killcam state (playercard, HUD, vision) from the state r1-server now sends, and the
  round-winning killcam plays in slow motion. Mid-match killcams start without a countdown.
- Net code audit (`docs\NETCODE_AUDIT.md`): what limited how smoothly and cheaply r1-server talks to QA clients, what
  was fixed and how to measure it on another host. `tools\dev\snap_cadence.py` measures snapshot arrival at the
  client, `tools\dev\sample_server.py` profiles the server, and `tools\dev\trace_delta.py` with
  `tools\dev\compare_delta.py` check delta snapshots against what the client really decoded.
- Client mods: `client_mod=<name>` in `r1-mod.ini` or `cx_clientmod <name>|off` puts `mods\<name>` first in the
  game's file search paths, so its loose files (images, models, sounds, UI movies) replace the game's. `cx_fspath`
  lists the search paths and `cx_fsfind <file>` says which one serves a file. A server can name its mod
  (`r1_clientMod`): the client loads it and rejoins.
- In-game chat reaches the server: in a match the chat box and message mode send `say` / `say_team` to the game
  server, which shows the line to everyone under the sender's name (`cx_chat`; `chat_to_server` in `r1-mod.ini`).
- Host commands over rcon: `cx_devmap <map> [gametype]`, `cx_god`, `cx_noclip`, `cx_ufo` and `cx_endgame`.
- `cx_frametime [reset]` reports frame times (average, median, 99th percentile, worst, hitches) for measuring a
  map's cost on the client.
- `cx_swapstyle reloads` lists the mixed staged-reload clips of the current map and what `bg_weaponAnimFix`
  replaces them with.
- Stats panel in the overlay (F7 or `cx_statspanel`): settings and a searchable list of every unlock flag with
  live and profile values and per-flag overrides.
- The stats overlay is non-destructive: it keeps every value it replaces and puts it back when a setting no longer
  asks for it or with `cx_stats 0`. New per-item overrides: any unlock flag by name (`cx_stats flag`), one weapon's
  XP (`cx_stats weapon`), any secure field by offset (`cx_stats raw`), plus `cx_stats show` and `cx_stats find`.
- `bg_weaponSwapStyle` (match variable, set by the server): 1 plays IW4's quick drop and raise clips, so a sniper
  goes away below the hip instead of over the shoulder when switching to a pistol. QA's zones lack those clips;
  `tools\data\make_quick_xanims.ps1` exports them from your 1.1.1.13 install into `iw4_quick_xanims.bin`, which the
  client builds at start.
- `bg_weaponAnimFix` (default 1): a staged reload keeps the clip family of its start, so a gripped M1014 no longer
  moves the hand between pump and grip mid-reload (QA ships a grip loop without a grip intro or outro).
- `tools\dev\map_check.ps1` loads every QA map on its own server beside a running one and writes
  `build\map_check.tsv`: load time, missing models and animations, unbuilt assets, errors, whether bots moved, CPU and
  memory. `run_native.ps1` gained `-Port`, `-HomeDir`, `-KeepOthers`, `-NoMaster`, `-LogName` and `-SvFps` for it.
- `docs\ROADMAP.md`: what is left for client mods, stats, swap style, attachment animations, server assets and maps.
- Join servers on other machines: before a join the backend asks the r1-server for its match key
  (`getmatchkey`) and puts it in the StartGame, so no key file has to be shared. `cx_match <ip>[:port] [port]
  [password]` takes the server's join password; `join_password` in `r1-mod.ini` covers the server browser.
- The backend's `--master-bind` lets a host run a server list for other machines; `--join-password` sets the
  default join password for room starts.
- `tools\data\make_all.ps1` makes every data table from your own running client, including the new
  `qa_cs_types.bin` (`dump_qa_cs_types.py`) and `qa_replicated_dvars.tsv` (`repldvars.py --tsv`).
- `run_native.ps1` runs `r1-server.exe` (the renamed server binary) with r1-server's `scripts\codol` over the base
  scripts, and no longer passes a key-file path.
- `build.bat` finds any Visual Studio 2022 with the C++ tools, not only the Build Tools. CI packages the Release
  build with the backend; a version tag makes a draft release.
- First step of a 3.19.26.22 port: `r1-mod.dll` recognises that client and starts its console there (`dvar`,
  `cmdlist`, engine commands, `<dvar> <value>`); every other component stays QA 3.3.3.3 only. The command and
  dvar addresses of both builds are behind one profile (`g_engine`), and `engine_map_319.h` holds the 3.19 map.
  Not run in a live 3.19 client yet. See "3.19.26.22: console only" in `docs\CLIENT.md`.
- `tools\dev\inject.py` takes the process to inject into as a second argument (name or pid).

## 2026.11.03

First release.

### Client
- `r1-mod.exe` starts the local backend and the QA 3.3.3.3 client and injects `r1-mod.dll`. `--skip-intro` and
  `--intro` choose whether the logo videos play.
- Offline profile: no first-run operator screen, classes and operators kept in `r1-mod_playerdata.bin` with
  automatic backups, every item unlocked, levels raised.
- In-game console with completion and history, and a server browser (F6).
- Joining: `cx_match`, `cx_room`, `cx_start`, `connect <ip>`. `cx_rcon` sends commands to the server.
- Settings the build lacks: `cx_fps`, `cx_yawspeed`, `cx_pitchspeed`, `cx_name`, `cx_operator`.
- iw4x's movement dvars in the client's prediction, such as `bg_bounces` and `bg_playerCollision`; `cx_movement`
  shows them. The server sends each client the values.
- GSC mods on the local server with `cx_mod`. The first mod is `mods\hns`, hide and seek ported from Kristóf
  Morva's Hide 'n Seek.
- Scaleform UI files can be replaced from `ui\` next to the DLL.

### Backend
- `backend\` is the local Demonware backend: auth, lobby, STUN, rooms, publisher files and a server list master.

### Tools
- `tools\data` generates the tables in `data\`, `tools\net` decodes and sends packets, and `tools\dev` holds the
  test loops, including `run_native.ps1`, which starts r1-server.
