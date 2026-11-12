# r1-mod client reference

What the launcher and the DLL do, every console command, and how each part attaches to the QA 3.3.3.3 client. Building is in the top-level README.

## Launcher

`r1-mod.exe` does everything needed to reach the lobby, configured by `r1-mod.ini` (written with defaults on
first run: `name`, `game_dir`, `game_exe`, `auto_start`, `skip_intro`, `extra_args`, `backend_dir`, `backend_cmd`,
`start_backend`):

1. Points the game folder's `dwauth.ini` / `stun.ini` at 127.0.0.1 if they are not already (original kept as `.orig`).
2. Starts the local Demonware backend (`backend\`, `local_auth.server --player <name>`, output in
   `backend.log`) unless 127.0.0.1:443 already answers, and waits for ports 443 and 3074.
3. Starts the client with `-q 0 -src=tgp -game_id 29 -zone_id 83886082` (without `-q`, WinMain exits with
   `WIN_NO_TCLS`) and `+set dw_username <name>`. With `skip_intro=1` (default 0) or `r1-mod.exe --skip-intro` the
   game also gets `R1MOD_SKIP_INTRO=1` in its environment; `--intro` plays the videos even with `skip_intro=1`.
   `--no-wait` returns once the DLL is injected instead of waiting for the game to exit.
4. Injects `r1-mod.dll` before the game's code runs: the entry point is held on `jmp $` while the loader
   finishes, `LoadLibraryA` runs in a remote thread, then the entry bytes are restored. DllMain installs the
   loopback redirects that the old frida launcher used to apply with frida (the hard-wired Tencent STUN probe
   116.130.x.x through the WS2_32 `connect` import, and the STUN host table at the `sub_B166D0` call 0xB16B8B).
5. **Start-up logo videos:** with `R1MOD_SKIP_INTRO=1`, DllMain turns the `jnz` that picks `autocinematic
   PUB_logo` / `CODO_logo` into a `jmp` to `autocinematic none` (Com_Init 0x59CE3F, renderer start-up 0x4655D2).
   The engine's own switches, dvars 2452 and 2628, read 0 after registration even when set on the command line.
6. **Onboarding and playerdata, in order.** The onboarding screen's Continue button starts a matchmaking search the
   local backend cannot answer, which is the "Server is busy" error, and the screen writes a fresh profile's
   default classes. The engine decides on it in `sub_6B9ED0`, which calls `IsOnboardComplete` (0x5C36F0) once
   Demonware is live and the profile is fetched; that call (0x6BA054) is hooked from DllMain. On the first call
   r1-mod applies `r1-mod_playerdata.bin`, sets the profile's `onboard_complete` notification flag through the
   engine's setter (`sub_5952E0`) if it is missing, and lets the check run, so the lobby opens with the saved
   profile and the operator screen never appears. The playerdata thread loads and saves nothing before that
   point. If the call is never reached, the older path takes over (dvar 3233 + `switchtomainmenu`, then the file
   is applied again). Before this, the file was applied on a timer (2 s after Demonware came up), and when the
   engine checked first, the operator screen opened and its default classes could be saved over the file.

**Player name:** without a Demonware account name, `dw_username` stays `unknown_user` and `sub_6CA5B0` replaces it
with the first 9 characters of the Windows user name + "Client" (GetUserNameA, `sub_643770`), hence "aClient".
The `name` dvar defaults to `dw_username`. The launcher sets `dw_username`; the DLL also applies `name` from
`r1-mod.ini` through `cx_name` if it differs. `dw_username` keeps 15 characters, `name` 31.

**Key bindings:** the lobby's "检测到有重要操作命令未绑定按键" (an important command has no key) came from
`+reload`: the engine's defaults bind `r` to IW4's combined `+usereload`, but CoDOL splits use (`+activate`, `e`)
from reload, and its settings list (41 commands at `dword_CE1E50`, mirrored to playerdata `keyBindings`) only has
`+reload`. Before the lobby opens, the DLL binds `r` to `+reload` when `+reload` has no key and `r` is free or still
`+usereload` (`apply_default_binds`, bind table `dword_1A2762C`). It runs every launch, so it does not depend on the
engine writing the binding back to playerdata or its encrypted `players\config_mp.cfg`.

`tools\dev\launch.py` (frida) and `tools\dev\inject.py [dll]` still work for late injection; the STUN redirect is then too late
and only the frida hooks cover it.

## Console

A console window opens in the game process. Commands can also be appended to `r1-mod_in.txt`
(next to the DLL; only lines appended while the game runs are executed); output goes to the window and `r1-mod.log`.

| Command | What it does |
|---|---|
| `dvar <name>` | print a dvar's value (via `Dvar_FindVar`) |
| `cmdlist [filter]` | list registered engine commands |
| `help` | usage |
| anything else | sent to the game command buffer (`Cbuf_AddText`) |

Registered inside the game (usable from game scripts and the engine console path):

| Command | What it does |
|---|---|
| `r1mod` | module base and rebase delta |
| `cx_dvar <name>` | print a dvar |
| `connect <ip>[:port]` | direct connect over UDP; registers the game dvars 8 s later (see `cx_gdvars`) |
| `cx_net` | direct-connect packet counters |
| `cx_trace <text>` / `off` / `*` | log `Com_sprintf` results containing the text |
| `cx_gdvars` | run G_RegisterDvars (0x4E2AC0) so `g_gametype` and friends exist in a remote match |
| `cx_name [new name]` | show or set the player name (dvar `name`, userinfo `name`, 31 bytes max) |
| `cx_version [0\|1]` | show or hide the on-screen banner |
| `cx_menu` | skip the first-run operator screen by hand with dvar 3233 (normally not needed: see Launcher, step 6) |
| `cx_playerdata [reset\|list\|restore [n]]` | re-apply the saved playerdata (classes, tutorial flags); `reset` drops the saved file and reloads the seed. Every launch backs the file up to `playerdata_backups\` first (newest 20 kept); `list` shows them (1 = newest), `restore n` puts one back after backing up the current file. Class and character ints in the buffer are stored as `key ^ rotr(value, 8)` with a key the game picks every launch (stats object +37156, flag +37160); the file keeps that key and flag after the buffer (12-byte `R1PK` trailer) and r1-mod puts them back on load. Files saved without it decode to garbage item ids, which crashed the class item lookup or reset every class |
| `cx_unlock [0\|1]` | mark every item owned (default on; create `r1-mod_nounlock.txt` to start with it off) |
| `cx_operator [name [1-12]]` | without arguments, list your 12 characters and all 27 operators. With a name (`cx_operator ghost`, `cx_operator cha_soap 3`), put that operator on a character, by default the current one. QA has only 12 character slots and the menu adds an owned operator only to a free one, so with all 12 taken the other operators never appear. The server dresses each player from `data\qa_operators.tsv` (`tools\data\gen_qa_operators.py`) |
| `cx_stats [0\|1]` / `cx_stats level N prestige N xp N flags N weaponxp N` | secure stats overlay, non-destructive: player level (default 70), prestige, XP within the rank, unlock-flag level for every gate flag (maps, modes, challenges, per-weapon attachment/camo tiers), weapon XP for items owned through `cx_unlock` (default max). `0` puts the profile's own values back. Saved to `r1-mod_stats.txt`; `r1-mod_nostats.txt` starts it off. See "Stats overlay" |
| `cx_stats flag <name\|index> <v>` / `weapon <name\|id> <xp>` / `raw <offset> <1\|2\|4> <v>` | one override: any unlock flag (state flags too), one weapon's XP, any field of the secure buffer by byte offset; `clear` as the value removes it, `clearall` removes all |
| `cx_stats show [flag X \| raw O B]` / `cx_stats find <text>` | settings and overrides, or a live value with the profile's own value; `find` lists the unlock flags whose name contains the text, with values |
| `cx_clientmod [<name>\|off]` | client mod: list the mods, or load or unload `<mods_dir>\<name>` on top of the game's files (saved as `client_mod` in `r1-mod.ini`). See "Client mods" |
| `cx_fspath` / `cx_fsfind <file>` | the file system's search paths, first wins; open a file the way the game does and say which path serves it |
| `cx_statspanel` (F7) | stats panel: overlay on/off, level, prestige, XP, gate-flag level, weapon XP, and a searchable list of every unlock flag with its live value, the profile's value and an override to set or clear (each change is a `cx_stats` line). `stats_key` in `r1-mod.ini` |
| `cx_swapstyle reloads` | for the current map's view models: each staged-reload stage whose clip family differs from the start clip's, and the clip `bg_weaponAnimFix` puts in (reads only) |
| `cx_swapstyle` | weapon swap style (`bg_weaponSwapStyle`) and reload clip fix (`bg_weaponAnimFix`), both set by the server, and the clips they replaced. See "Weapon swap style" and "Reload clip families" |
| `cx_match [ip[:port] [port] [password]]` | join a server through the real bdDTLS path without the lobby menu: runs `room.create`, then writes `startgame.trigger` in the backend folder (`backend_dir`, default the repository's `backend\`) so the local backend pushes StartGame (default 127.0.0.1 28960) |
| `cx_room` | only the `room.create` half of `cx_match`: no start timer runs (r1-mod keeps `dw_autoStartGame` off), so classes can be edited under Inventory for as long as needed. The console room has no screen of its own |
| `cx_start [ip [port]]` | only the start half: StartGame is pushed at once, with no countdown. See "Rooms and starting a match" |
| `cx_browser` | open or close the in-game server browser (see below) |
| `cx_fps [n\|off]` | show or set the frame rate cap: dvar `1848` (IW4 `com_maxfps`, default 85, 0 = no cap). The game limits it to 100; r1-mod raises the range to 1000, keeps the value if something resets it and saves it as `max_fps` in `r1-mod.ini`. `off` hands it back to the game. While connecting (connection state neither 0 nor 10) the game's limiter (0x59D840) still holds 30 |
| `cx_chat`, `cx_chat test <text>` | in a match the chat box and the message mode send `say` / `say_team` to the game server instead of the lobby's chat channel (which is each player's own local backend). `cx_chat` says whether that is on and how many lines went; `test` sends a line through the same function. `chat_to_server=0` in `r1-mod.ini` leaves the game's own behaviour |
| `cx_devmap <map> [gametype]` | host: load a map with cheats on through the server's `devmap` (sets the game type first when given). Needs the server's rcon password in `r1-mod.ini`, like `cx_rcon` |
| `cx_god`, `cx_noclip`, `cx_ufo` `[slot]` | host: toggle god mode, noclip or ufo for a player, yourself by default (r1-server's `god`/`noclip`/`ufo <slot>`, no cheats needed) |
| `cx_endgame` | host: end the match now (r1-server's `endgame`) |
| `cx_frametime [reset]` | drawn-frame times over the last 4096 frames (or since `reset`): average and frame rate, median, 99th percentile, worst, and frames over twice the median (hitches), with the connection state (10 = in a match). Timed once per frame where the overlay draws, so it covers the whole frame |
| `cx_yawspeed [n\|off]` / `cx_pitchspeed [n\|off]` | show or set how fast `+left`/`+right` and `+lookup`/`+lookdown` turn the view, in degrees per second: dvars `965` and `966` (IW4 `cl_yawspeed`, `cl_pitchspeed`, default 140; `+speed` multiplies them by dvar `967`, 1.5). r1-mod keeps the value and saves it as `yaw_speed` / `pitch_speed` in `r1-mod.ini`; `off` hands it back to the game |
| `cx_rcon <command>` | send a command to the game server over rcon and print its reply, e.g. `cx_rcon map mp_seatown`, `cx_rcon status`. The QA client has no rcon of its own. Server and password come from `rcon_address` (default `127.0.0.1:28960`) and `rcon_password` (default `r1modlocal`, as `tools\dev\run_native.ps1` sets) in `r1-mod.ini` |
| `cx_mod [<name>\|off]` | load a GSC mod on the local server (`cx_mod hns`: hide and seek), go back to its own scripts (`off`), or with no argument list the mods and show which one runs. Over rcon, then `map_restart`. See "GSC mods" |
| `cx_movement` | show the movement dvars the server sent (`bg_bounces` and the rest; see "Movement dvars") |
| `cx_inspect` / `state` / `stop` | play the held gun's inspect animation (QA's inspect key does the same), with Modern Warfare Remastered's rules: gun ready, not aimed, one at a time. `state` prints the weapon state, ADS fraction and whether an inspect plays; `stop` ends it. The clips come from `r1_xanims_319.bin` next to the DLL or in the client mod's folder, which r1-mod does not include |
| `cx_loc` / `reload` / `apply` / `<KEY>` | your own translation of the game's text from `r1_english.txt` (`KEY<TAB>text`, `\n` escapes, `#` comments) in the client mod's folder or next to the DLL: how many strings it holds and how many are in use, read the file again, apply it again, or show one string. Applied at start and whenever the connection state changes. r1-mod ships no game text |
| `cx_compat` | this client's compatibility number (`r1_compat`, sent in the connect userinfo with `r1_version`) and the last server's, from its info reply before a join. See [COMPATIBILITY.md](COMPATIBILITY.md) |
| `cx_cs [0\|1]` | configstring checksum check ignored (1, default) or enforced (0); prints the client's computed value. The value depends on the mode the client loaded, so a private server cannot know it in advance |

Automatic on load: onboarding skip (disable with `r1-mod_noskip.txt`), playerdata seed/persistence (`r1-mod_playerdata.bin`, seeded from `data\qa_playerdata.bin`),
unlock-all, secure stats overlay.

The banner (`r1-mod <version> | QA 3.3.3.3 | <name>`) is drawn bottom-left every frame in every connection
state by a call-site hook at 0x47010F (the per-frame 2D pass) that queues text with R_AddCmdDrawText (0xAA61A0).

**Dvar protection:** the console `set` path passes source 1/2 to `Dvar_SetValueCore` (0x632150), which refuses
dvars whose flags include 0x2800 (`name` is 0x2000). Engine-side sets use source 0, so r1-mod calls the
function directly for protected dvars.

## Rooms and starting a match

The room host runs `sub_6E7260` every frame. With every member Ready (and at least the mode's minimum player
count) it starts at once (`sub_6E4A20`: a FIND_SERVERS query the local backend leaves empty, then
`dwRoomMessageStartGame`); with some members Ready it broadcasts `StartCountingDown(dw_startGameCountingDown)`
(60 s, `sub_6E4CD0`) and starts when that runs out; when a Ready is withdrawn during the countdown it broadcasts
`StartCountingDown(-1)` (`sub_6E4D30`). `dw_lockLobbyCountingDown` (5 s) locks the room before the start. The local
backend turns the countdown into a StartGame push when it ends and drops it on -1 (`schedule_start` in `backend\local_auth`;
before 2026-11-03 a -1 read as "start now"). Alone in a Team Deathmatch room (4–12 players) Ready never starts the
match, so `cx_start` is the way to start a solo room.

Class edits are written straight into playerdata (everything is owned, so `WL_EquipWeapon` is not a preview) and
reach the server with the stats the client sends when it connects.

**Class caches.** The engine also keeps the classes in two caches (character classes at 0x2E16E54, filled from
playerdata by `sub_593700`; weapon classes at 0x2E88F1C, by `sub_593B20`) and writes them back over playerdata with
`sub_6D90C0`: on connect (`CL_ConnectToHost`; `CL_CheckForResend` and the profile upload `sub_6D8560`, both through
the profile sanitizer `sub_6D8A70`), on gamestate and on some server commands. The caches are filled before r1-mod
applies `r1-mod_playerdata.bin`, so until 2026-11-03 they held the engine's default classes (M4 / Vector / L86 with
M9), and the first StartGame wrote those over every class; r1-mod then saved that over the file. Found with a hardware
write breakpoint on class 0's primary. r1-mod now reloads both caches right after applying the file, on the game
thread (the onboarding check hook); loads from other threads (`cx_playerdata`, "engine blanked it") ask `sub_5E2630`
for its state 9, which runs the same two functions and is what the engine uses after a disconnect.

**Rooms made with Create Room in the menu** preload the room's mode and map. The client's configstring type table
(`g_csTypes`) then differs from the one the native server encodes with (`codol_qaTypesPath`), and
`CL_ParseConfigStrings` stops with `XBOXLIVE_CANTJOINSESSION` ("Unable to join game session") when the server sends
a full string at an index the client has typed. Seen 2026-11-02 with a Crash room against a Seatown server. Rooms
from `cx_room` / `cx_match` load nothing beforehand and join fine. Not solved yet: the server would need the type
table of the room's mode and map.

**loadoutVersion:** the engine's profile validation (`sub_6985E0`, run by `sub_5E2630` when the lobby is ready)
treats a profile whose `loadoutVersion` is below dvar `3294` (3) as an old class layout and, once onboarding is
complete, puts the default classes in every slot (`sub_698030`), then sets the version to 3. The seed and every saved
file had 0 and the profile now has `onboard_complete`, so a validation pass after the file is applied would reset all
classes. Not the cause of the resets above (the trace never saw it run), but r1-mod raises `loadoutVersion` (int at
+13318) whenever it applies the file.

## GSC mods

GSC runs only on the native server (`r1-server`), which compiles each script from the first source root that
has it (`codol_script_source.c`): `codol_scriptNativeOverlay`, then `codol_scriptOverlay`, then `codol_scriptPath`
(`tools\dev\run_native.ps1` sets all three to the collated 1.1.1.13 set, `...\home\codol_rawfiles`). The dvars are
read on every script load, and a full `map_restart` reloads the level (`SV_LoadLevel`), so pointing the native overlay
at another folder and restarting changes the scripts without touching the server binary.

A mod is a folder under `[r1-mod] mods_dir` (default `<dll dir>\mods`; the build copies the repository's `mods\`
there) with the same layout as the script set, holding only the files it replaces. `cx_mod <name>`:

1. reads the server's `codol_scriptNativeOverlay`; when it is not a mod folder, saves it as `mod_base_overlay` in
   `r1-mod.ini` (what `cx_mod off` goes back to; without it, `off` falls back to the server's `codol_scriptOverlay`),
2. sends the lines of `mods\<name>\mod.cfg` if there is one (server commands, `//` comments),
3. sets `codol_scriptNativeOverlay "<mods_dir>\<name>"`, checks the server kept it, and sends `map_restart`.

The server prints compile and runtime errors to its `qconsole.log`. Limits: the client has no `fs_game` and no mod
fastfile, so a mod can only use assets the QA zones already hold (models, materials, sounds, weapons, localized
strings), and IW4 script menus exist only in-game (class select); hud elems, `iPrintLn` and buttons are what a mod
has for UI. Every distinct string given to a hud elem's `setText` takes a configstring until the next map, so
countdowns belong in `setTimer`/`setValue`. A path with a space may be split by rcon (`cx_mod` warns).

| Mod | |
|---|---|
| `hns` | Hide and seek (prop hunt), replaces Team Deathmatch: `mods\hns\README.md`. Port of the round of iCore's CoD4 Hide 'n Seek (github.com/KristofMorva/hns, MIT); hiders become props of the current map, seekers wait blinded, then hunt |

## Client mods

A client mod is a folder under `mods_dir` (default `<dll dir>\mods`, the same folders `cx_mod` uses for GSC) that
holds files laid out like the game's `main\` folder: `images\*.iwi`, `models\...`, `sound\...`, and `ui\...` for
Scaleform movies. `client_mod=<name>` in `r1-mod.ini` loads it at start; `cx_clientmod <name>` / `off` switches in the
lobby. One `mods\<name>` can carry a GSC mod for the server and the client files together.

How it attaches: the QA build kept IW4's file system. Its search-path builder (`sub_627D30`) is called from
`FS_InitFilesystem` (0x62815F) and `FS_Restart` (0x62821A); both calls are hooked, and after the engine's paths
r1-mod adds the mod folder through the engine's own `sub_627430` (ecx = folder, one stack argument = base path), so it
is searched first. Every file read walks those paths (`sub_626230`) and the loose-file exe opens them with `fopen`, so
a file in the mod replaces the game's. `cx_clientmod` in the lobby runs `FS_Restart` (`sub_628210`) on the game
thread and then `vid_restart`; `vid_restart` alone does not rebuild the paths (it restarts the file system only while
0x1A2A578 > 0). IW4's own `fs_game` (dvar `1006`) is not used, because the engine also keeps the player's config in
that folder. UI files are looked for in `mods\<name>\ui\` before `<dll dir>\ui\`.

Limits: assets inside the fastfiles (rawfiles, string tables, weapon definitions, `.cfg` files run by `exec`, which
reads rawfiles only) do not go through the file system, and QA has no `mod.ff`.

**A server's mod.** r1-server sends `r1_clientMod` with the movement dvars. When it names a mod other than the loaded
one while the client is in a match and `mods_dir` has that folder, r1-mod disconnects, loads it in the lobby (not saved
to `r1-mod.ini`) and rejoins the same server; without the folder it says so once and plays on. Back in the lobby the
dvar is cleared. On the server, `reset r1_clientMod` clears it (over rcon an empty `set` loses its quotes). Verified 2026-11-10: with `set r1_clientMod r1test` on the server, joining loaded `mods\r1test` and
rejoined by itself, and the mod's file was served from it.

Verified 2026-11-10 in a running client: a mod-only `images\r1test_only.iwi` and an overriding `images\$black.iwi`
were opened by the game's own `FS_FOpenFileRead` from the mod (search path 0) while `$white.iwi` still came from
`main`; `cx_clientmod off` and back rebuilt the paths both ways in the lobby without a restart.

## Stats overlay

The secure playerdata (`mp/playersecuredata.def`) holds rank, prestige and the unlock flags; the local backend serves
none, so r1-mod writes the values it is asked for into the buffer (offsets in `component/stats.c`). Every value it
replaces keeps the value that was there (when the engine or backend changes a value behind the overlay, that becomes
the profile's value), and each pass every 2 s writes what the settings want and puts back every value no setting wants
any more: lowering a setting, removing an override or `cx_stats 0` restores the profile's own values. The overlay's
values reach the server with the stats the client sends on connect; neither r1-mod nor the backend stores them.
Weapon XP is not written to the buffer: the item-info stub answers it per weapon (`weapon <name> <xp>`, else
`weaponxp`). Verified 2026-11-10: off restored rank 0, `uf_ak47_mp_rank` 0 and a raw field; `weapon ak47_mp 5000`
gave 5000 through the game's own item info (`sub_695BC0`) while other weapons kept the blanket value.

## Weapon swap style

`bg_weaponSwapStyle` is a match variable: the server sends it like the movement dvars, and it returns to 0 in the
lobby. 0 is QA. 1 asks for IW4's quick drop and raise: a switch from a primary to a pistol is a quick drop, and IW4
played separate `_quick` clips there (the gun goes away below the hip), where every QA weapon definition puts its
regular putaway (the over-the-shoulder flip).

How it attaches: `sub_556760` (plays view-model clip slot N, scaled to the weapon's time for that slot) is detoured
at its entry; for slots 22 (quick raise) and 23 (quick drop) it points the view model's anim set at
`<clip>_quick` when the asset table holds a loaded clip of that name with frames and bones. The timing stays the
weapon's, so prediction and server agree.

**Where the clips come from.** QA's zones do not contain the IW4 quick clips: the names exist in the client only as
placeholders (0 frames, no bones), and an early build that played them exploded the arms. The 1.1.1.13 zones have
them, and they animate the same bones as QA's own clips (QA's `viewmodel_cheytac_putaway` is 1.1.1.13's 35-frame clip
with the sleeve bones stripped). r1-server's `codol_xanim_export` exports them from your 1.1.1.13 install
into `iw4_quick_xanims.bin` (R1XA: each clip's serialized zone bytes plus its bone and
notify names as text). At start the client rebuilds each clip as an `XAnimParts`: arrays copied to aligned memory,
names turned into its own script strings with `SL_GetStringOfSize` (0x5F73A0, as the zone loader `sub_485710` does);
a clip with a back-referenced array or a delta part is refused. The bundle is looked for in the client mod folder,
then next to the DLL; without it style 1 changes nothing.

Verified 2026-11-10 on Seatown: 42 clips built; with style 1 the Intervention's quick drop to the M9 played
`viewmodel_cheytac_putaway_quick` (the gun goes below the hip) and looked right in game; no errors. Pistols have no quick
pullout in either build, so the pistol's own raise follows.

## Reload clip families

`bg_weaponAnimFix` (default 1; a server can send 0 for stock QA): a part can swap a gun's clips for its own set (the
vertical grip: `viewmodel_m1014_grip_*`), and QA ships some sets incomplete. The gripped M1014 has `grip_reload_loop`
but no grip intro or outro, so a reload went base intro (hand on the pump), grip loop (hand on the grip), base outro.
With the fix a staged reload keeps the family of its start clip: when the reload, reload-empty or the two other stage
slots (9-12) name another family than the start clip (slot 13, else the end clip 14), the same stage of the start's
family is used if loaded (`viewmodel_m1014_reload_loop` for the grip loop). Timing is unchanged. `cx_swapstyle` lists
what it replaced. Slot numbers come from the clip timing table (`dword_CDD6E0`: 9 `iReloadTime`, 11
`iReloadEmptyTime`, 13 `iReloadStartTime`, 14 `iReloadEndTime`).

## Movement dvars

iw4x's movement dvars, with iw4x's names, values and defaults. The server decides movement (the native server runs
its own C1 pmove) and the client predicts it, so each rule exists twice: in the server's pmove
(`r1-server\src\codol\player\codol_movement_dvars.c`) and in the QA client's prediction
(`src\client\component\movement.c`). The server sends each client its values (`s <name> "<value>"`) when the client
joins and whenever one changes; set them on the server (`cx_rcon set bg_bounces 1`). Back in the lobby the client
returns to the defaults. `cx_movement` prints what the client has. The server sends them to every active client from
its per-frame client loop (`SV_SendClientMessages`), spawned or not; until 2026-11-10 it sent them only from
`ClientEndFrame`'s playing path, so a player on the class screen or spectating got nothing until it spawned, which is
why the swap style once looked broken after a rejoin.

| Dvar | Default | Effect | Client site (IDA) |
|---|---|---|---|
| `bg_bounces` | 0 | 1: a jump step may land on a non-walkable slope (the IW3 bounce IW4 removed); 2: also keep the jump state where `Jump_ApplySlowdown` would clear it, for double bounces | `PM_StepSlideMove` 0x4219D8; `Jump_ApplySlowdown` 0x40E5E3 |
| `bg_bouncesAllAngles` | 0 | 1: `PM_ProjectVelocity` always projects; 2: with `bg_bounces`, steep surfaces (normal z < 0.3) are taken too | 0x415C94; 0x4219D8 |
| `bg_disableLandingSlowdown` | 0 | landing keeps the velocity | `PM_CrashLand` call 0x418731 |
| `bg_bunnyHopAuto` | 0 | holding jump jumps again on landing | `Jump_Check` 0x40EB0D |
| `bg_playerCollision` | 1 | 0: players move through each other (shots still hit) | `CG_ClipMoveToEntity` call 0x458EDE |
| `bg_playerEjection` | 1 | overlapping players are pushed apart at `g_playerCollisionEjectSpeed` (25); server only | — |
| `bg_climbAnything` | 0 | every surface is a ladder | `PM_CheckLadderMove` traces 0x41C39B, 0x41C44B |
| `player_duckedSpeedScale` | 0.65 | crouch speed scale | `PM_CmdScaleForStance` 0x416960 constants |
| `player_proneSpeedScale` | 0.15 | prone speed scale | same |
| `cg_noclip_scaler` | 3 | noclip speed scale | move scales 0x416670, 0x416820, 0x416AA0 |
| `cg_ufo_scaler` | 6 | ufo speed scale | same |
| `bg_rocketJump`, `bg_rocketJumpScale` | 0, 64 | rockets push the shooter back along the shot (not exclusive launchers such as killstreaks); server only | — |

How the client attaches: the constants (3.0, 6.0, 0.65, 0.15) are operands that now read the dvar's value at +0x0C,
the `fmul m64` ones turned into `fmul m32` (same length); the tests are short jumps to stubs that check the dvar and
continue at the original branch targets; `PM_CrashLand`, the two ladder traces and the entity clip are call-site hooks.
Each site is checked against its expected bytes; the log line `r1-mod: movement dvars registered, 20 pmove sites
patched` says all matched. The dvars are registered and the sites patched on the game thread (`cx_movement install`,
queued once at start), because `Dvar_Register*` keeps state in the caller's thread-local storage.

Verified 2026-11-03 against the native server: all 20 sites patch, rcon `set`s reach the client within a frame or two
(`cx_movement`), and spawning, dying and respawning on the patched pmove log no errors or prediction corrections. The
movement itself (bounces, auto hop, collision, ejection) was not exercised yet; it needs a player at the right spots.

Not ported: `bg_disableBarrierClips` (the QA client's contents bits for player clip are not mapped yet),
`bg_ladderFixedInput`, `bg_sprintIgnoreRepress`, omnimovement and dive.

Before this, the native server's `StuckInClient` was empty (no ejection at all) and every player-fired rocket pushed
the shooter back by 64 (the CoD4 kick). With the defaults, ejection is now on and the rocket push off, as in iw4x and
stock QA.

## Loose UI files

The frontend is one Scaleform movie, `scaleform/default/codo.gfx`, a rawfile in `ui_mp.ff`. Every file the UI opens
goes through the game's `FileOpener` (vtable 0xC7C4C8, slot 1 = 0x6B94A0). DllMain replaces that slot: when
`ui\<path>` exists next to `r1-mod.dll` (for the frontend `ui\scaleform\default\codo.gfx`), it is served in a
Scaleform `MemoryFile` the same way the original serves the rawfile; otherwise the original runs. Each file is read
once per session and logged (`r1-mod: UI file ... loaded from ...`).

## In-game console and server browser

The QA build compiled IW4's drop-down console out (CL_InitRenderer 0x467E20 still loads the `console`
material and `fonts/consoleFont`, but nothing draws them) and has no server browser. r1-mod draws both
with Dear ImGui 1.92.7 (`deps\imgui`, MIT) in `src\client\component\overlay.cpp`.

| Key | Panel |
|---|---|
| `` ` `` / `~` | console; Shift+`` ` `` makes it taller, Esc closes. Tab completes commands and dvars (matches are listed under the console with dvar values), Up/Down walk the history (kept in `r1-mod_history.txt`), PageUp/PageDown scroll, `clear` or Ctrl+L empties it. The frame rate shows next to the version |
| F6 (or `cx_browser`) | server browser: Refresh (F5), sort by column, hide empty/full, search; double-click, Enter or Join connects through `cx_match`. Ping is coloured, password servers are marked |

Text and spacing scale up on screens taller than 1080 pixels.

The console shows r1-mod output and the game message windows (chat, obituaries, server prints: the 7
callers of 0x45B070 are hooked; the QA build has no Com_Printf). Lines run through the same line
runner as the external console. A game message repeated back to back is shown once and then counted
("(repeated N more times)" when another line arrives); every 500 repeats `r1-mod.log` gets the call site, message
window and a stack. That found the announcer flood of 2026-11-03: on mp_rust_long "我们上吧" (subtitle of
`UK_1mc_boost`) ~950 times a second. Window 3 is sound subtitles: QA prints an alias' subtitle each time the sound
starts (sub_444430, from the sound update `sub_70C660`), and it restarts an entity's loop sound every frame (cgame
frame 0x529114: `if (ent->loopSound /* +0x17C */) sub_5019B0`). The native server sent loop sounds as C1 sound
slots, which named the announcer line in QA's table; it now translates them (`r1-server\src\sv_snapshot.c`).

The browser asks the master in `[r1-mod] master=` (default `127.0.0.1:20810`, the backend's
`local_auth.master`) for `getservers`, then each listed server for `getinfo`, and shows host name, map,
mode, humans/slots (+bots) and ping. The native server heartbeats to that master when started with
`+set sv_master1 127.0.0.1:20810` (`tools\dev\run_native.ps1` does; r1-server's `server.ini` has `master`). Joining
uses the lobby's StartGame path: the backend first asks the server for its match key (`getmatchkey`, with
`join_password` from `r1-mod.ini`) and puts it in the StartGame, so any reachable r1-server can be joined. A server on
this PC that does not answer gets the older local key file instead.

`r1-mod.ini` keys: `console_key` and `browser_key` (Windows virtual-key codes, defaults 0xC0 and 0x75),
`master`.

How it attaches:
- **Drawing:** on the render thread, inside the last scene of each frame: the call to 0xAD3DA0 right before the
  EndScene of the end-of-frame function 0xA986B0 (site 0xA98795) is hooked, and the panels are drawn on the back
  buffer of `dx.device` (0x103208C). The device object is not touched: swapping its vtable crashed the renderer
  (0xA945F8), and the game presents through neither Present nor PresentEx of the device.
- **Input:** the game window (0x3749AAC) is subclassed. While a panel is open, keyboard and mouse messages
  go to ImGui (key and button releases also reach the game, so nothing stays held) and keyCatchers
  (0x1A2E7E4) bit 0, IW4's KEYCATCH_CONSOLE, is set: IN_MouseMove (0x45EE80) then frees the cursor
  instead of re-centring it.

## Direct connect

`connect <ip>[:port]` (default port 28969) swaps the Demonware transport for one UDP socket for that peer.
Verified 2026-10-29 against `tools\net\udp_listen.py`: the client sends `getchallenge 0 "<id>"` every
3 s, and replies from the listener reach the engine (`cx_net` shows sent/received counters).

Hooks (call sites patched, each checks the original target first):

| Site | Replaces | Behavior for the direct peer |
|---|---|---|
| 0x46BA49 | `DW_XNADDR_ToNetadr` | fills netadr {4, ip, port, handle 0x7FFFFFFF} |
| 0x46BA6A, 0x640CC0, 0x6D6337, 0x6D63F7 | `DW_NetadrToString` (0x6D6A40) | name "direct" |
| 0x465C55, 0x685D80, 0x685E11, 0x6E75A8 | `DW_PeerState` (0x6D61D0) | reports connected (2) |
| 0x6429C0 | `DW_SendPacket` | `sendto` |
| 0x598915, 0x598AE5 | `DW_RecvPacket` | non-blocking `recvfrom` first |

The handle must be out of range for both signed (`< 157`) and unsigned (`<= 0x9C`) table checks;
`0xFFFFFFFF` crashed in `DW_NetadrToString`.

Test loop: `tools\net\udp_listen.py 28969 udp_listen.log [--reply]` answers or logs what the client sends.

## 3.19.26.22: console only

`r1-mod.dll` also recognises the 3.19.26.22 retail client (file version 1649370). There it starts the console
and nothing else, as a first step of a port:

- the console window and `r1-mod_in.txt` take lines; `dvar <name>`, `cmdlist [filter]`, `cx_dvar`, `r1mod`;
- any other line goes to the game's command buffer, so engine commands run and `<dvar> <value>` sets a dvar
  (the engine's own `Dvar_Command`);
- nothing in the client is hooked or patched.

Everything else in this file (overlay, server browser, `connect`, `cx_match`, playerdata, stats, onboarding,
intro skip, UI files, movement dvars, the kept settings) is QA 3.3.3.3 only: those components hold QA addresses
and are not started.

How it attaches (`src\client\game\game.c`, addresses in `engine_map_319.h`):

- `engine_detect` tells the builds apart by code: QA by the first bytes of `Cbuf_AddText`, 3.19 by `Cmd_Argv`
  (0x443F60), whose bytes hold the addresses of the command level and argc table. The exe has no relocations.
- 3.19 has no `Cbuf_AddText` function (it is inlined at about 49 sites), so r1-mod appends to `cmd_text[0]`
  (0x76595A4: `{data, maxsize, cmdsize}`) itself, under the engine's lock (0x81A55A0).
- One command list (0x33303B0) instead of QA's two; the executor moves the command it runs to the front, so
  r1-mod checks that its nodes stayed in after linking them.
- Dvars are laid out as in QA where the console reads them: name +0, type +8, value +0xC, hash chain +0x48.

r1-mod does not start this client and does nothing about its start-up requirements: `r1-mod.exe` is for QA.
The client has to be running already, in a state that lets a DLL load, with its code unpacked (r1-mod waits up
to a minute for that). Then: `uv run --with frida python tools\dev\inject.py <dll> <process name or pid>`.

State 2026-11-10: builds, and the detection and every address in `engine_map_319.h` were checked against the
unpacked 3.19 exe and the QA exe on disk. Not run in a live 3.19 client yet.

## Notes

- Commands need a node in both lists: `0x2E0111C` (walked by the executor `0x58D910`) and `0x2E01224`
  (`Cmd_FindCommand`). A node in only one list is found but never run, or run but not found.
- Commands sent while the client is busy (e.g. `vid_restart`) can be delayed or lost; wait a few seconds.
- Not done yet: answering the challenge (needs a QA-compatible game server), `disconnect`, Com_Error output
  mirroring, mod fastfiles (client mods replace loose files only: see "Client mods").
