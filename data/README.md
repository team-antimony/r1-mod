# data

Tables and seeds derived from the QA 3.3.3.3 client. They are not in git: everything here comes from the game's own
files or memory. Make the whole folder from your own running client with `tools\data\make_all.ps1` (start the client
with r1-mod and join a match first, since a few tables only exist then), copy it from another checkout with
`tools\data\setup_data.ps1`, or regenerate one file with the tool listed below. Most tools attach to a running client
with frida (`uv run --with frida python tools\data\<tool>.py`). A new frida tool must import `tools\fridatmp.py` before
`frida`, as the existing ones do: without it every run leaves a `frida-*` folder in `%TEMP%`.

The client DLL reads `qa_playerdata.bin` and, from next to the DLL, `iw4_quick_xanims.bin`. Everything else is read by r1-server (its `start-server.ps1` and
`tools\dev\run_native.ps1` pass the paths) or by other tools.

| File | Contents | Made by |
|---|---|---|
| `qa_playerdata.bin` | seed playerdata buffer (default classes, tutorial flags), used when `r1-mod_playerdata.bin` is missing | `build_playerdata.py` (needs `tables\mp_coMarketplaceItems.csv`) |
| `qa_playerdata.bin.json` | summary of the seed | `build_playerdata.py` |
| `qa_playerdata_schema.json` | `mp/playerdata.def` structure | `dump_playerdata_def.py` |
| `qa_securedata_schema.json` | `mp/playersecuredata.def` structure | `dump_securedata_def.py` |
| `qa_weapon_classes.tsv` | class layout in the playerdata buffer | `dump_qa_class_layout.py` (from the schema) |
| `qa_client_weapons.tsv` | weapon index -> internal name | `dump_client_weapons.py` |
| `qa_client_weapon_keys.tsv` | ammo/clip pool indices per weapon | `dump_client_weapon_keys.py` |
| `qa_ncs.tsv` | net-const configstrings | `dump_ncs.py` (join a match first: the strings depend on what the client loaded) |
| `qa_operators.tsv` | default outfit per operator | `gen_qa_operators.py` (from `tables\`) |
| `qa_base_parts.tsv` | allowed part ids per weapon | `baseparts.py` |
| `qa_weapon_parts.tsv` | part models and tags per weapon | `dumpparts.py` |
| `qa_weapon_times.tsv` | WeaponDef timings | `dumptimes.py` |
| `qa_gun_bases.tsv` | composed gun bases | `qaguns.py` |
| `qa_replicated_dvars.tsv` | replicated dvars (configstrings 24+ / 344+) | `repldvars.py --tsv` (join a match first) |
| `iw4_quick_xanims.bin` | IW4's quick drop/raise clips from your 1.1.1.13 zones (42 clips), for `bg_weaponSwapStyle 1`; also copied next to the DLL, which reads it | r1-server's `codol_xanim_export` (not part of r1-mod) |
| `qa_cs_types.bin` | the client's configstring type map (7058 bytes), which r1-server needs to build a gamestate | `dump_qa_cs_types.py` (join a match first) |
| `qa_gametypes.tsv` | `mp/coGameTypesTable.csv` rows | a scratch script (not kept); `dump_stringtable.py mp/coGameTypesTable.csv` dumps the same rows as CSV |
| `qa_parts.tsv` | weapon-part id -> name | `dump_qa_parts.py` (loadout screen loaded) |
| `qa_player_anims.tsv` | player-animation index -> name | `dump_qa_player_anims.py` (join a match first; the table is null at the menu) |
| `qa_weapon_camos.tsv` | camo item -> weapon, camo index | `dump_qa_weapon_camos.py` |
| `qa_weapon_defaults.tsv` | default items per weapon | generator not kept (the per-category defaults come from more than `coweapitemswea.csv`); copy with `setup_data.ps1` |
| `stats_obj_blob.bin` | a stats object sample (research) | — |
| `tables\*.csv` | StringTable assets, e.g. `mp_coMarketplaceItems.csv`, `mp_splashTable.csv` | `dump_stringtable.py <asset name>` |
| `ui\scaleform_default_codo.gfx` (+ `.strings.txt`, `.symbols.txt`) | the frontend Scaleform movie and its AS3 strings | `extract_gfx.py` on an inflated zone you supply |
| `ui\codo_as3\` | the movie's 2,477 AS3 classes, decompiled | FFDec `-export script` |
| `server\*.json` | client field tables (playerstate, entities, configstring types) and the gamestate template | exported during the server port |
