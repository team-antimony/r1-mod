# hns: Hide and Seek (prop hunt)

Loaded with `cx_mod hns` (see `docs\CLIENT.md`, "GSC mods"). Replaces Team Deathmatch (`g_gametype war`) while it
is loaded; `cx_mod off` brings TDM back.

Port of the core round of iCore's Hide 'n Seek for CoD4 (<https://github.com/KristofMorva/hns>, MIT, see
`LICENSE`). The CoD4 mod also shipped a `mod.ff` with its own models, menus, sounds and weapon files and a stats,
rank, shop and challenge system; none of that can reach the QA client (no client-side `fs_game`, no mod
fastfile), so only the game round is ported.

## Round

1. When at least `scr_hns_minplayers` players (bots count) are on a team, one in `scr_hns_seekers` is drawn as a
   seeker (axis), everyone else hides (allies). Everyone respawns on their team.
2. Hide time (`scr_hns_hidetime`, 30 s): seekers are frozen behind a black screen. Hiders are invisible players
   carrying a prop model, without weapons, in third person.
3. Seek time (`scr_hns_seektime`, 180 s): seekers hunt with their class. A hider that dies joins the seekers.
4. No hider left: the seekers score 1. Time up: the hiders score 1. Six seconds later the next draw.

## Hider controls

The CoD4 mod picked props and settings in script menus; the QA client has none for this, so they are buttons:

| Button | Action |
|---|---|
| Use | next prop |
| Aim | previous prop |
| Melee | rotate 20 degrees around the current axis |
| Fire | change the axis (X, -X, Y, -Y, Z) |
| Smoke (secondary offhand) | lock the prop's angles / let it follow the view |
| Grenade, held 0.5 s | third person on / off |

## Props

The client can only draw models its zones contain. The props are the models of the map's own `script_model`
entities (both server and client have those loaded), minus helpers (`tag_origin`, collision, wire bundles, the C-130
intro rig...), plus the care package crate, plus anything in `scr_hns_models` (space separated, must exist in the
client's zones). `cx_rcon scr_hns_props` prints the list the server picked. On seatown: electric boxes, chickens, an
air conditioner, a ceiling fan, hanging laundry, the crate.

## Known issues

- Props clip into walls and floors (seatown's are wall- and ceiling-mounted, placed by their origin). The fix is an
  offset per model, but this server gives script models no bounds, so `getPointInBounds` measures 0 and
  `scr_hns_offsets` stays at zero; the offsets need the models' bounds from the client's zones.

## Files

| File | |
|---|---|
| `maps\mp\gametypes\war.gsc` | gametype entry (the C1 `war.gsc` with the hns callbacks) |
| `maps\mp\gametypes\_hns.gsc` | rounds, teams, props, controls, hud |
| `mod.cfg` (optional) | server commands `cx_mod` sends before the restart, one per line |
