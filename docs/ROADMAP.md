# r1-mod roadmap: from "reaches a match" to "finished"

Evaluation of 2026-11-10, written after the branches were converged onto `main` (`db13fbd`). It covers the six
features still needed for r1-mod to count as finished, where each one stands, what is left, and how each step is
checked. Work in r1-server (the native dedicated server, separate repository) is listed where a feature needs it.

**Guiding rule.** The base experience is the QA 3.3.3.3 build as it shipped. Every change that departs from it,
whether a 3.19 UI or animation style or an IW4 (Xbox) behaviour that trickshot servers want, is a **match variable**:
a dvar the server sets and replicates to each client, default off (stock QA). A server host chooses the experience;
a player joining gets it without configuring anything.

## How match variables reach the client

The channel already exists for the movement dvars: r1-server sends `s <name> "<value>"` to a client when it joins
and whenever the value changes (`r1-server src/codol/player/codol_movement_dvars.c`), the client registers the same
dvar names (`src/client/component/movement.c`) so the engine applies the values, and `movement_keep` puts the
defaults back once the client is in the lobby again. New match variables use the same path:

| Dvar | Default | Side | Effect |
|---|---|---|---|
| `bg_*`, `player_*`, `cg_*_scaler` | stock | both | iw4x movement (done, see CLIENT.md "Movement dvars") |
| `codol_iw4SpawnRaise` | 0 | server | IW4 first raise on spawn (done in r1-server) |
| `bg_weaponAnimFix` | 1 | client | a staged reload keeps one clip family (gripped M1014: no jump between pump and grip); 0 is stock QA |
| `r1_clientMod` | "" | server | the client mod players load (r1-mod `mods\<name>`); clients load it and rejoin |
| `bg_weaponSwapStyle` | 0 | both | 0 QA (each gun's own putaway), 1 IW4/Xbox: the gun drops below the hip instead of flipping over the shoulder (this roadmap, step 3) |
| `r1_spawnFallback` | 1 | server script | a map without a mode's spawn class uses its other spawns or points made from path nodes (mp_bridge); 0 is stock |
| `r1_trophy`, `r1_trophyRange`, `r1_trophyCharges` | 1, 384, 2 | server script | the trophy system deploys and intercepts (`_r1_trophy.gsc`); 0 leaves it a landed projectile |
| future `ui_*` 3.19 styles | 0 | client | 3.19 HUD/UI changes, each behind its own variable (step 3, later) |

## 1. Client-side mod loading

**State.** `cx_mod` loads GSC mods on the server only (`codol_scriptNativeOverlay` + `map_restart`). Scaleform
movies can be replaced from `<dll dir>\ui\`. The client has no way to load a mod's own files (images, models,
sounds, configs, UI) and the docs listed "client-side mod assets" as not done.

**Finding (IDA, qa3333.exe).** The QA build kept IW4's file system. `sub_627D30` builds the search paths
(`FS_Startup`), `sub_627430` adds one game directory (ecx = directory name, one stack argument = base path; it also
adds that directory's localized subfolders), `sub_628130` is `FS_InitFilesystem` and `sub_628210` `FS_Restart`. Dvar
`1006` is IW4's `fs_game` (flags 0xC00, lower-cased by `sub_627A80`) and `vid_restart` (`sub_465260`) restarts the
file system when it changed. Every file read walks those paths (`sub_626230`) and ends in `sub_58EE90`, which the
loose-file client points at plain `fopen`. So a directory added last to the search paths overrides every loose file
the client reads: `images\*.iwi`, `models\*` stream models, `sound\*`, `.cfg` files. Assets compiled into the
fastfiles (rawfiles, string tables, weapon definitions) do not go through it, and the client has no `mod.ff`.

**Steps.**
1. Hook both calls of `sub_627D30` (0x62815F, 0x62821A): after the engine's paths, add `<mods_dir>\<mod>` through the
   engine's own `sub_627430`, so it has the highest priority. Do not use `fs_game` itself: IW4 also writes the
   player's config into the `fs_game` folder, and QA keeps its encrypted `players\config_mp.cfg` there.
2. The mod is chosen by `client_mod=` in `r1-mod.ini` (next start) or `cx_clientmod <name>|off` (in the lobby:
   `FS_Restart`, then `vid_restart`). The same folder can carry a GSC mod for `cx_mod`, so one `mods\<name>`
   holds a whole mod.
3. Scaleform UI files: look in `mods\<mod>\ui\` before `<dll dir>\ui\`.
4. **Done 2026-11-10:** the server names the mod it needs (`r1_clientMod`, sent with the movement dvars); the client
   disconnects, loads it and rejoins; a missing folder is reported. Verified with a test mod.
5. Later, needs zone tooling: a mod fastfile (QA-format zone writer) for rawfiles, tables and weapon definitions.

**Done 2026-11-10 (steps 1-3).** `component/fsmod.c`; `client_mod=` and `cx_clientmod`. `exec` cannot be the test
(QA's `exec` reads rawfiles only), so the check opens files through the game's own `FS_FOpenFileRead`: `cx_fsfind`
served a mod-only image and an overriding `$black.iwi` from the mod, `$white.iwi` still from `main`, and
`cx_clientmod off` / `<name>` rebuilt the paths both ways in the lobby (`FS_Restart`, then `vid_restart`).

## 2. Stats unlocking and editing, non-destructive and at full depth

**State.** `cx_unlock` patches the ownership check (nothing written to the profile). `cx_stats` writes the player's
level, prestige, the gate unlock flags and a single weapon XP into the secure playerdata buffer every 2 s. It is not
non-destructive: the values it replaces are never kept, so `cx_stats 0` leaves the edited values in place
("values already written stay until the profile reloads"), and it can only set every gate flag to one level and
every weapon to one XP.

**Steps.**
1. Keep the original of every value the overlay changes (first write wins) and put it back when the overlay is
   turned off, a single override is removed, or the client disconnects from the backend; `cx_stats restore` does it
   on demand. Nothing the overlay writes is saved into `r1-mod_playerdata.bin` (that file holds only the regular
   playerdata; the secure buffer is served by the backend and never written by r1-mod).
2. Full depth: per-flag overrides by name or index from `mp/unlockFlags.csv` (`cx_stats flag uf_ak47_mp_rank 3`),
   every flag including state flags only when named explicitly, per-weapon XP (`cx_stats weapon ak47_mp 5000`), XP
   within the rank, and a raw field override by byte offset for anything else in `mp/playersecuredata.def`
   (`cx_stats raw <offset> <bytes> <value>`), all saved in `r1-mod_stats.txt`.
3. `cx_stats show <flag|weapon|raw>` reads back the live and the original value so every edit can be checked.
4. **Done 2026-11-10:** a stats panel in the overlay (F7, `cx_statspanel`) over the same commands; opens in a match.

**Done 2026-11-10 (steps 1-3).** Checked in a running client: overriding `uf_ak47_mp_rank` and a raw field, then
`cx_stats 0`, gave back the profile's values (0); clearing an override fell back to the blanket level; `weapon ak47_mp
5000` came back as 5000 from the game's own item info while other weapons kept the blanket XP. Not checked yet: the
menu's lock icons following a single flag. Step 4 (an overlay panel) is open.

## 3. Weapon swap style: QA or IW4/Xbox (match variable)

**State (2026-11-04 weapon session, live).** Intervention to M9 is a quick drop then the pistol's first raise. The
server and the client's prediction agree frame by frame. The over-the-shoulder flip is the Intervention's only QA
putaway animation (`viewmodel_cheytac_putaway`, used for drop, quick drop and alt drop), so the real QA game looks the
same; no server setting can change it, because the server only sends animation numbers. `codol_iw4SpawnRaise`
(server) covers the IW4 first raise on spawn.

**Done 2026-11-10.** Client: `component/weapon_anims.c` detours `sub_556760` (plays clip slot N scaled to the
weapon's time for it; slot 22 quick raise, 23 quick drop, read from `dword_CDD6E0`) and points those slots at
`<clip>_quick`. Server: `bg_weaponSwapStyle` is registered and sent with the movement dvars
(`r1-server src/codol/player/codol_movement_dvars.c`, built, not committed: that checkout also holds other
uncommitted work).

**Finding that changes the plan.** QA's zones do not carry IW4's quick clips: `viewmodel_cheytac_putaway_quick` and
the rest exist in the client only as placeholders (0 frames, no bones), and `DB_FindAsset` answers 1 for exactly those.
The first build played them and the arms exploded (seen in a match). Now only a clip that is really loaded with
frames and bones is used, so style 1 does nothing on stock data (checked in a match). Playing the gun's raise clip
backwards instead was considered and dropped: it needs per-frame XAnim time control the engine does not offer.

**Done 2026-11-10, second pass: the real clips.** The 1.1.1.13 zones have the quick clips (checked with an offline
probe: `viewmodel_cheytac_putaway_quick` 25 frames, `pullout_quick` 20) and they animate the same bones as QA's clips.
r1-server's `tools\codol_xanim_export.c` writes them to `iw4_quick_xanims.bin` (42 clips) and the client rebuilds them at start. In a match with style 1 the Intervention's quick drop played the IW4
clip and the user confirmed it looks right.

Fixed the same day: the server sent its match variables only to spawned players (from `ClientEndFrame`), so after a
rejoin the client sat at style 0 on the class screen; it now sends them to every active client each frame
(`SV_SendClientMessages`), checked with the client at style 1 before spawning.

Left: the clips only exist for the guns IW4 had (42 in the two common zones; more in co-op/zombie zones, not mounted
by the export); other QA guns keep their own putaway under style 1. Commit the exporter in r1-server.

## 4. Animations that match the attachments

**State.** QA builds a composed gun ("** base camo x parts") and each part can swap the gun's animation set
(`for_gripvt*` swaps to grip versions). The M1014 with a vertical grip (bots get it) looks like an LMG raise and
moves the hand between the grip and the pump between reload stages, because QA has a grip version of the reload loop
only. A gripped M1014 looks the same in the real game. The server drops some of the player's attachments ("no C1
row": FMJ, bipod, stalker) and gave bots a default rail on the grip's mount.

Seen 2026-11-10 (Seatown, server log): a bot's UMP45 was given `muz_flashhidersmgump45`, a part the client does not have
for that gun ("the client has no part ... 5 of 6 parts"); the server drops it from the composed name, so the bot's
gun is still drawn, but bot loadouts are built from parts the QA client never offers. The M1014 vertical grip
(`for_gripvtshortm1014`) is offered by the QA client itself, so the mixed reload clips are a QA data issue and the fix
has to be on the client (step 3).

**Steps (best effort, r1-server unless noted).**
1. Bots: pick attachments from the QA part table per weapon (`qa_base_parts.tsv`) and never give a gun a part whose
   animation set is incomplete for that gun (the grip on shotguns), so bots stop carrying combinations the menu
   never offers.
2. Players: carry every QA part through composition (QA ids, not 1.1.1.13 rows) so the gun the server builds is the
   gun the client built from the class.
3. Client: for a part whose animation set lacks a clip, use the base gun's clip for that slot instead of mixing
   sets mid-reload. **Done 2026-11-10** as `bg_weaponAnimFix` (default 1, `component/weapon_anims.c`): a staged reload
   keeps the family of its start clip; for the gripped M1014 the loop becomes `viewmodel_m1014_reload_loop`. Built;
   checked that the target clip is loaded in a live client (17 frames, as the grip loop); not yet seen in game.
   Checked against every loaded clip the same day (read from the live client's asset table): of the 22 staged reload
   families QA loads (`viewmodel_*reload_start/loop/end`, `_intro/_out` for the M1014), only `viewmodel_m1014_grip_`
   is incomplete (a loop, no intro or outro); the other grip sets (`spas12_grip_`, `model1887_grip_`, `csg12_grip_`)
   are complete, so the fix touches the gripped M1014 only. Its replacement loop has the same 17 frames and 189 bones
   as the grip loop. `cx_swapstyle reloads` lists, for the current map's view models, each mixed reload stage and the
   clip the fix puts in.

## 5. All assets, weapon models, reticles and attachments in the server

**State (r1-server `docs/CODOL_QA_PARITY_PLAN_20261031.md`).** The server reads QA fastfiles (all 238 archives open;
weapons, models, fx, materials, sounds and anims replay) and mounts QA `common_mp` / `common_gametype_mp` beneath the
1.1.1.13 zones, so QA-only weapons (Freedom, crossbow, skins) build. Open items there: QA attachment composition on
QA-only guns, the damage curve in `Bullet_Process`, QA clip-size scaling, QA effects (muzzle flashes), and missing
skins (RPD Hellwalker, Zombie Axe) that QA's zones do not have.

**Steps (r1-server).** Finish part 8 of that plan in order: composed QA guns with the player's QA parts (reticles
are parts: the sight's reticle material comes with the part), the alt-weapon slot numbering, the damage curve,
clip scaling, QA effects. Missing skins need 3.19 assets converted to QA zones, which needs a zone writer.

Seen 2026-11-10 (Seatown, Intervention skin + M9): in the death camera the player's own third-person model held no
gun (arms posed around nothing). QA guns ship `tag_origin` as their world model and draw the visible gun from the
composed variant, so the world model of the composed gun is what to check first. The server did build that player's
guns with parts (Intervention skin variant 457, 6 default parts; M9 variant 458, 3 parts), so the next check is in a
match: do other players' guns show in third person outside the death camera. A body with no gun is also what IW4 does:
`dropWeaponForDeath` drops the gun as a pickup when a player dies, so the corpse in the death camera is empty-handed.

**Trophy system (reported 2026-11-10, Boomtown: "models incomplete when placed down").** Neither QA's nor C1's trophy
script is on hand, and r1-server had none, so a trophy (`trophy_mp`, `trophydisco_mp`) landed as its thrown
projectile, the folded model, and did nothing. **Done the same day** in r1-server's scripts
(`maps/mp/gametypes/_r1_trophy.gsc`, started from `_weapons.gsc` `init`): when the projectile comes to rest it
becomes the deployed model (`mp_trophy_system`, `mp_trophydisco_system`) with its bombsquad model and the disco start
effect, and it shoots down enemy grenades and rockets it can see within `r1_trophyRange` (384) up to
`r1_trophyCharges` (2) times, as IW5's trophy does; enemy damage destroys it, one per player, removed on a team change
or when its owner leaves; `r1_trophy 0` turns it off. Verified on a test server (bots throwing through the equipment
hooks): on Boomtown the disco trophy deployed (`deployed trophydisco_mp ... (2 charges)`) and stopped an enemy's
concussion grenade 205 units away; on Seatown `trophy_mp` deployed and let its owner's frag through; no script errors.
Not yet seen by a player: whether the client draws the deployed model complete.

**Stowed guns.** `stow_on_back` hid the IW4 attachment tags of the gun on a player's back, which QA's gun models
(`wea_*`, composed from parts) do not have (`cannot find part 'tag_up' in entity model 'wea_pp2000_pp2000_blk'`); it
now skips that step for `wea_*` models (2026-11-10).

The map check (section 6) also shows the 1.1.1.13 `_weapons.gsc` asking for IW4 stow models (`weapon_at4_stow`,
`weapon_rpg7_stow`, `weapon_stinger_stow`, `weapon_javelin_stow`: launchers drawn on a player's back) that QA does not
have, and precaching about 1,080 IW4 attachment-combo weapon names (`m4_xmags_silencer_mp`) per map that QA does not
have either (QA composes guns from parts). Both are skipped safely; the stow table and the precache list should come
from QA's own weapon and part tables.

## 6. Maps: all supported maps load every asset, efficiently for players and bots

**State.** The client has 36 MP maps in `client_shipRetail\chinese` (`mp_*.ff` plus `_const`). r1-server runs
1.1.1.13 map zones and mounts a QA map zone when 1.1.1.13 lacks the map; each map still needs a level script, and maps
without a waypoint file route bots over path nodes. Not yet read from QA zones: map geometry types 15/17/21/42 for
maps 1.1.1.13 also has (the server then runs the 1.1.1.13 layout of a map the client loaded from QA).

**Steps (r1-server).**
1. A per-map check over the 36 maps: start the map, join a client, log missing assets, script errors and spawn
   counts; keep the result as a table in r1-server's docs.
2. Prefer the QA zone for every map the client loads, so collision, entities and spawns are the ones the client sees.
3. Efficiency: the server keeps only the zones a map needs mounted (common zones once), bots on precomputed
   waypoints, and the client's `cx_fps` cap; measure server frame time with 18 bots per map.

**Map check 2026-11-10** (`tools\dev\map_check.ps1`, server build that reads `.ff` archives, `war`, 4 bots, `sv_fps 20`,
20 s of play per map; results in `build\map_check.tsv`, each map's log in `build\map_check\`):

- All 30 QA `mp_*` zones load (7-11 s each; 800-900 MB; the server idles at 1-2 % of a core with 4 bots).
- 29 are PvP maps and play: spawns, bots moving (on mp_dr_cistern the bots did not change stance within the 20 s,
  but spawned). mp_bridge is a co-op map (QA's only reference is `cf_ch_coop_suspension_mp_bridge`): it has no TDM
  spawns, so `_spawnlogic.gsc` aborts the level and it restarts as `dm` in a loop; it needs the co-op mode.
- Missing on every map: 4 IW4 stow models (launchers on a player's back) and 12 melee "clash" animations
  (`pt_clash_*`, `pb_clash_*`). Per map: `body_city_civ_male_a` (10 maps), `mp_flag_neutral` (firingrange, mountain),
  `com_laptop_rugged_open` (suburbs), `vehicle_80s_hatch1_greendest` (fav_tropical), the generator clips on
  estate_tropical and the air-conditioner fan on crash_tropical. QA has none of these names; the map entities ask for
  them, so each is either mapped to a QA model or left out.
- About 1,080 skipped precaches per map (IW4 attachment-combo weapon names, see section 5) and path-node warnings on
  some maps; bots on maps without a waypoint file route over the map's own path nodes.

Follow-up the same day:
- The per-map props are absent from QA itself, not just from the server: no QA zone or loose folder has
  `mp_flag_neutral`, `com_laptop_rugged_open`, `vehicle_80s_hatch1_greendest` or the generator/fan clips, and the map
  entities that name them are QA's own, so the stock QA client cannot draw them either. `body_city_civ_male_a` is
  precached by `aitype/civilian_urban_male_drone.gsc` for civilian actors that never spawn in PvP. The clash clips are
  player animations of melee clashes that QA dropped. Nothing loadable is missing on the 29 PvP maps.
- mp_bridge needs QA's `coop` mode, and r1-server has no `coop.gsc` (only its `.txt`): co-op is not implemented on the
  server, so it is not a multiplayer map to support yet.
- **mp_bridge plays in PvP modes (later the same day).** Its only player spawn is one `mp_global_intermission`;
  `_spawnlogic.gsc` aborted the level on the missing `mp_tdm_spawn` and the restart looped. With `r1_spawnFallback` 1
  (default; 0 = stock) a class a map lacks gets the map's other `mp_*spawn*` points, else, when the map has none,
  up to 48 points made from its path nodes (floor found with a player hull trace, room checked with `canSpawn`, at
  least 384 units apart, facing the middle), else `info_player_start` or the intermission point. On mp_bridge (`war`,
  4 bots): 48 points from 978 nodes, players spawned along the bridge, bots moved, no script errors and no restart.
  Maps with their own spawns are unchanged (Seatown rechecked: no fallback).
- The IW4 attachment-combo precaches are gone (1,079 -> 1 per map; Seatown and Killhouse rechecked, no errors).

Next: implement QA's co-op mode in r1-server for mp_bridge and the `coop_spec` zones, and repeat the check with a
client joined to measure frame time per map.

**Server load with a near-full lobby (2026-11-10, `map_check.ps1 -Bots 17 -Settle 40`, `war`, `sv_fps 20`,
`build\map_check_17bots.tsv`).** Share of one core while 17 bots play, and what that is per 50 ms server frame:

| Map | Load | CPU | Per frame | Memory | Script errors |
|---|---|---|---|---|---|
| mp_shipment_long | 14 s | 8.4 % | 4 ms | 845 MB | 0 |
| mp_terminal_cls | 9 s | 9.4 % | 5 ms | 939 MB | 0 |
| mp_afghan | 11 s | 10.9 % | 5 ms | 873 MB | 0 |
| mp_seatown | 11 s | 11.2 % | 6 ms | 937 MB | 0 |
| mp_boomtown | 9 s | 15.0 % | 8 ms | 927 MB | 0 |
| mp_bridge | 9 s | 23.1 % | 12 ms | 929 MB | 0 |

Every map stays well inside the frame, with one hitch each (the first frame after load) and bots moving. mp_bridge
costs the most: it has no waypoint file and its bots route over 978 raw path nodes. The client side is measured with
`cx_frametime` (frame times where the overlay draws); no per-map client numbers are recorded yet.

## 7. Choppy player movement (reported 2026-11-10)

On Seatown with 2 bots on an r1-server build that reads `.ff` archives, started by `run_native.ps1` (`sv_fps 60`): every player
"bounces around while moving in one direction". Ping 9, server CPU ~18 % of a core, no hitches after map load. The
engine default and IW4's server rate is `sv_fps 20`; r1-server's sway study (`CODOL_SWAY_CAUSE_SWEEP_20260916.md`)
found the client's own `commandTime` reaching the server in ~35 ms lumps. Next: compare `sv_fps 20` and 60 on the same
map, then measure snapshot `serverTime` steps and the entity positions in them (`codol_qaDumpSnapshots`).

## 8. Killcams: IW4 / CoD Online style, and the slow-motion final killcam

**State before 2026-11-10.** r1-server already replayed the killer's archived view to a QA client (IW4's
`_killcam.gsc`, `doFinalKillcam` and `doFinalKillCamFX` are all in the script tree), but the client never knew it was
watching a killcam, the killer's card was missing, and the final killcam stayed grey.

**Done 2026-11-10 (r1-server).**
- *Killcam state.* The QA client enters its killcam state when the playerstate's `deltaTime` (field 142, ps+2664)
  becomes nonzero and leaves at zero (`sub_51B3F0`; it sets `cg+0xC6FE8`, which 70-odd places test: HUD, head icon
  "you", sounds, the playercard view). The QA playerstate writer never sent it and always sent `killCamEntity` (136,
  ps+2668) as none. Both are sent now while the client views an archived frame (`codol_qaKillcamState`, default 1; 0 is
  the old archived view only). Read from the live client: `inKillCam` 0 -> 1 with `deltaTime` 6550 for the 7 s of each
  killcam, where it stayed 0 before.
- *Playercard.* `setCardDisplaySlot( attacker, 7 )` is the server command `L <client> <slot>`; the QA client's case
  76 copies that client's card into slot 7, which its `HUD_PlayerCardView` state 3 (in a killcam, `sub_5D3E70`) shows.
  r1-server's QA layer dropped `L` (`QA reliable drop -> player: L 1 7`); it passes now.
- *Colour.* The round end sets the grey `mpOutro` vision for everyone and the final killcam resets it per player with
  `N "<map>" 0`. QA's cases 78..82 are its five per-player vision slots `N O P Q R`, and only `P` passed, so the final
  killcam was black and white. All five pass now.
- *Slow motion.* `setSlowMotion( 1.0, 0.25, ... )` already reached the client (`d 6132 -> d 6415`); with the killcam
  state in place the final killcam plays it: the client's script time scale ramped 1.0 -> 0.25 and back (read at 50 Hz
  during a Search and Destroy round end on mp_afghan).
- *Countdown.* The tenths timer shows on the round-winning killcam only; `scr_killcam_countdown 1` shows it on every
  killcam, as IW4 does (`_killcam.gsc`).

**Open.**
- An overhead name in the friendly colour on the killer's victim (seen in a final killcam where an enemy killed a
  teammate; not in the next one, the player's own kill). The client drew that name by the watching player's team
  rather than the killer's; the head-icon code (`sub_4DA750`) compares against the viewed client (`cg+324`), so
  the name path differs. Find the overhead-name function and its team reference.
- Smoothness: confirmed smooth by the player at their frame rate with `sv_fps 60` (2026-11-10). QA's `CL_AdjustTimeDelta` (0x4FA5B0) keeps the client clock 16 ms behind the
  newest snapshot, so at `sv_fps 20` (50 ms spacing) it extrapolates most of each interval and a replayed view steps;
  the archive is recorded at `sv_fps` too. `run_native.ps1` defaults to `sv_fps 60`; measure the killcam view with
  `cx_frametime` at each cap and, if it still steps, widen the client's interpolation delay as a match variable.
- Grenade and explosive killcams (`killCamEntity` is sent now; not yet seen in a match), the look-at entity (137) and
  `killCamClientNum` (138).
- The card's icon, title and background come from the client info of the killer; check they are the killer's own.

## 9. Next for the public release (asked 2026-11-10)

1. **Fire sound lost after a quick double switch.** With the gold CheyTac M200 (`海之翼`): firing straight after a
   double weapon switch plays no fire sound. Compare the client's shot with the server's weapon state at that moment
   (`CoDOL QA wstate` lines) and the fire event the server sends back.
2. **Chat under the player's name.** *Done 2026-11-10.* QA reuses IW4's chat letters and both of its chat inputs
   (the engine's message mode and the Scaleform chat box) send through the lobby's room object to the chat channels
   `chat.room` / `chat.roomteam`, which with r1-mod are each player's own local backend, so a line reached nobody.
   - r1-server sends every chat line as a game message with the sender's name (`codol_qaChatMode` 1).
   - r1-mod replaces the room's two send functions (`component/chat.c`; `dwRoomImp` vftable slots 16 and 17): in a
     match the text goes to the game server as the engine's own `say "<text>"` / `say_team "<text>"`; outside a
     match the lobby channel gets it as before. `chat_to_server=0` in `r1-mod.ini` turns it off; `cx_chat` reports,
     `cx_chat test <text>` sends a line through the same function.
   Verified live: `cx_chat test` arrived as `clientCommand: player : say "hello from the chat box path"` and was
   broadcast as `player: hello from the chat box path`; a line typed in the in-game team chat box arrived as
   `say_team "hi"`. Left: draw received chat in the QA chat box itself (it shows in the game message feed and in
   r1-mod's console).
3. **XUID generation for playerdata.** *State 2026-11-10:* a player already has a persistent id. The local backend
   makes a random 63-bit `user_id` the first time a name is used and keeps it in its profile store
   (`backend/local_auth/profiles.py`); the client connects with it, and r1-server uses it as the player's guid in
   the game log, in `status` and for the saved profile (`5e571aeed6e7739a` for one player across every session
   here). It is keyed by the player *name*: a new name is a new id. Open question for the owner: keep that, or make
   the id per installation as iw4x does, so a renamed player keeps their stats. If iw4x code is used, credit and
   licence text go in the README.
4. **Console and host commands.** *Done 2026-11-10.* r1-server console (so rcon): `god <slot>`, `noclip <slot>`,
   `ufo <slot>` toggle it for one player without cheats for everyone, and `endgame` ends the match through the
   scripts' `forceEnd` (`_r1_admin.gsc`); `devmap <map>` was already there. r1-mod: `cx_devmap <map> [gametype]`,
   `cx_god`, `cx_noclip`, `cx_ufo` (yourself by default) and `cx_endgame` send them with the rcon password of
   `r1-mod.ini`. Checked over rcon: each toggle answers `GAME_..._ON/OFF` for the slot, a missing or bad slot prints
   the usage, and `endgame` on a test server logged `r1 admin: endgame requested` and put players in the
   intermission. The r1-mod commands build; not yet used in a match.
5. **IW4M-Admin compatibility** (https://github.com/RaidMax/IW4M-Admin). *Done and run live 2026-11-10* with
   IW4M-Admin 2026.2.4.1 against the local server (files and notes: r1-server `tools/iw4madmin/`).
   - Parser plugin (`ParserR1Server.js`): `status` with the game log's guid, the dvar format, composed gun names in
     kill lines. IW4M-Admin monitored the server, found the player as `NetworkId=5E571AEED6E7739A` and the bots,
     read `!owner` / `!help` from the game log and answered with `tell`.
   - Game Interface (`_integration_r1.gsc` with IW4M-Admin's base and shared scripts, started by `_r1_admin.gsc`):
     `gsc integration is enabled`; `!alert`, `!kill`, `!takeweapons`, `!switchteam`, `!playertome`, `!lockcontrols`,
     `!setspectator` and `!giveweapon` ran over the dvar bus without script errors, and the game log shows their
     effect. `!hide` / `!noclip` were accepted but the player was a spectator, so their effect is unconfirmed.
   - Server changes it needed: `tempbanclient`, `sv_sayname`, `_website`, the guid in `status`, chat lines and bold
     game messages (`c`, `g`) passed to QA clients, synchronous game log (`+set g_logsync 1` at start).
   - Left: a `fast_restart` no longer doubling the bots is fixed in the bot script but not yet tested live.
6. **mp_boomtown in Search and Destroy.** *Done 2026-11-10.* Its S&D spawns are placeholders in a holding box far
   outside the map (-9472 8576 -192) with a kill trigger, so players died on spawning and the round restarted in a
   loop. A spawn point more than 2048 units outside the box around the map's team and free-for-all spawns is now
   ignored (`r1_spawnFallback`, section 6), and the class takes the map's other spawns. Test server, `sd`, 4 bots:
   `1 of 1 mp_sd_spawn_attacker points ... outside the map; ignored`, players spawned inside the map from 93 other
   points, no restart, no script errors. The map has no bomb sites, so a round there is decided by elimination.

## Order of work

1, 2 and 3 are in this repository and are done first (this pass). 4, 5 and 6 are r1-server work, tracked in its
docs; their r1-mod side (match variables, the per-map check tooling) follows them.
