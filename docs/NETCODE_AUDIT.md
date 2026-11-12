# r1-server net code audit, 2026-11-10

What limits how smoothly and cheaply r1-server talks to QA 3.3.3.3 clients, what was fixed, and what is left. Every
number here was measured on one Windows 11 machine with the server, one QA client and three bots on mp_afghan at
`sv_fps 60`; the tools that produced them are listed so the same checks can be run on another host.

## Summary

| Finding | Effect | State |
|---|---|---|
| Frame pacing slept at Windows' 15.6 ms timer resolution | frames landed on timer ticks and slipped a whole tick at random; snapshot arrival jitter sd 7.0 ms, 15 % of snapshots over 1.5x late | **fixed** |
| Entity fields were found by linear search, twice (61 x 66 lookups an entity) | a third of all server time with one QA client | **fixed** |
| The bdDTLS session file was written with a disk flush inside the send and receive paths | send times of 8..13 ms every few seconds per player, far worse on slow disks | **fixed** |
| A model's QA slot was found by comparing its name with up to 1024 names, per entity per snapshot | 4-5 % of server time | **fixed** (cached) |
| No frame-time or bandwidth measurement in the server | stalls under 500 ms were invisible | **added** (`sv_frametimes`) |
| Snapshots for QA clients were sent whole, never as a delta | **195 kB/s per player** at `sv_fps 60` (3 packets of ~1080 bytes per snapshot) | **fixed for the entity list** (`codol_qaDeltaEntities 1`, off by default until play-tested): 38.5 kB/s, one packet per snapshot. The player state is still sent whole |
| Building one QA snapshot costs ~0.95 ms | ~15 ms a frame for 16 players at `sv_fps 60`: over budget | open |
| `rcon status` and other console work run inside the frame with a synchronous console log | up to ~20 ms in one frame under the development launch flags | open; use the production flags below |
| Game step spikes of 14..22 ms a few times a minute | one late frame each | open (bots or script, not traced) |

Result so far, same machine and load:

| Measure | Before | After |
|---|---|---|
| Snapshot arrival at the client, standard deviation | 6.97 ms | 1.9-4.5 ms (run to run) |
| Snapshots more than 1.5x the median late | 14.9 % | 0.7-2.0 % |
| Server frame start, 95th / 99th percentile (target 16.67) | not measurable | 17.4 / 17.6-18.5 ms |
| Time to send to one QA client, mean / 99th percentile | 1.70 / 7.7 ms | 0.95 / 1.3 ms |
| Server thread busy share (1 player, 3 bots) | 15.0 % | 10.7 % |
| Sent to one player, with `codol_qaDeltaEntities 1` | 195 kB/s in 180 packets/s | 38.5 kB/s in 60 packets/s (643 bytes each) |
| Time to send to one QA client with deltas, mean / 99th percentile | 0.95 / 1.3 ms | 0.79 / 1.06 ms |

The spread in the "after" column is the remaining game-step and console spikes, which fall into some 15 s windows
and not others.

## How it was measured

- **`tools\dev\snap_cadence.py [seconds]`** (r1-mod): reads the running client's memory (no injection) and stamps
  every change of the newest snapshot's server time with the performance counter. It prints the arrival interval
  (mean, sd, percentiles, histogram), the server-time step per snapshot and the share of late snapshots. The client
  only reads packets once per frame, so its own frame time is a floor under the spread.
- **`sv_frametimes` / `sv_frametimes reset`** (r1-server console or rcon): the last 8192 server frames. `interval`
  is the time between frame starts, `work` the frame's own time, `game` the game step, `send` building and sending
  to all clients, `net over` how far packet handling and sleeping ran past their timeouts between frames (rcon
  commands run there), `loop` the rest of the main loop. Then the traffic since the reset and the frames that
  started more than half a frame late or ran more than one game step.
- **`tools\dev\sample_server.py [seconds]`** (r1-mod): a sampling profile of the server's busiest thread (brief
  suspends, instruction pointer and stack, symbols from `nm`). It found the two linear searches and the name search.
- **`obj\_tmp\timertest.c`** (r1-server, scratch): the clock step and the real length of `select()` timeouts with
  and without `timeBeginPeriod(1)`.

## Findings in detail

### 1. Timer resolution (fixed, `src/win32/sys_win32.c`)

The server paces frames by sleeping in `select()` (`NET_Sleep` from `SV_Frame`) and nothing raised the Windows
timer resolution. Measured: a timeout of 1 to 12 ms took 15.5 ms and a 16 ms one 28.9 ms; with `timeBeginPeriod(1)`
they took 1.3 to 16.2 ms. The clock itself (`gettimeofday`) already moves in sub-0.1 ms steps. At `sv_fps 60` a
frame is 16.67 ms, so frames ran on 15.6 ms ticks and regularly missed one, and two game steps then ran back to
back before one snapshot. At `sv_fps 20` the same rounding gives +-15 ms on a 50 ms frame. This is what was reported
as "all players bounce around while moving in one direction".

`Sys_PlatformInit` now calls `timeBeginPeriod(1)` and `Sys_TermProcess` releases it. Since Windows 10 2004 the
request is per process, so another program asking for 1 ms no longer helps a server that does not.

### 2. Entity field lookups (fixed, `src/sv_snapshot.c`, `src/codol/snapshot/codol_engine_state.c`)

Translating an entity for a QA client looked up each of its 61 QA fields by scanning the 66-entry C1 field table
from the start, and filling the C1 state did the same for each of ~86 fields. The profile showed
`CODOL_EntityField` 17 %, its caller 15 % and `put_entity` 8 % of the server's time. Both now use a table from
byte offset to field index, built once per entity type. Time to send to one client fell from 1.70 to 1.12 ms.

### 3. Session file writes (fixed, `src/codol/net/codol_atomic_file.c`, `codol_network.c`)

To survive a restart the server saves each client's bdDTLS association. It did so with `_commit` and a
write-through rename (a disk flush), in the send path each time a client's counter used up its reserved range of
256 records (every ~4 s at 60 packets a second) and every 5 s in the receive path. The file only has to outlive the
process, not a power cut, so it is now written without the flush, and the reserved range is 4096 so the send path
no longer saves at all in normal play. The 99th-percentile send time fell from 7.7 to 1.4 ms. On a VM disk, a
network drive or an SD card the old flush would cost tens to hundreds of milliseconds per player every few seconds.

### 4. Model name lookups (fixed, `src/codol/snapshot/codol_qa_csmap.c`)

Each entity's model is translated to the QA client's slot by name; the search compared the name with up to 1024
constants and then with the names the server placed. The result is now kept per C1 slot with the name it was worked
out for and a table generation number, so one `strcmp` replaces the search until a name or the tables change.

### 5. Whole snapshots (entity list fixed, `src/codol/snapshot/codol_qa_entities.c`, `src/sv_snapshot.c`)

The QA writer sent every snapshot "from zero": the complete player state and every visible entity, each time.
One player received 195 kB/s in 180 packets a second (about 3.2 kB a snapshot in three packets) at `sv_fps 60`.

With `codol_qaDeltaEntities 1` the entity list is now the difference from the snapshot the client last
acknowledged. The server keeps, per client, what each of its last 32 messages carried; when the acknowledged one
is still held and at most 12 messages old, the snapshot names it (`deltaNum`) and lists only entities that
changed, appeared or went away. A changed entity carries only its changed fields, in the forms the QA client's
`MSG_ReadDeltaField` (0x5A27F0) decodes against the old value. Anything the writer cannot express sends that one
snapshot whole instead, and `sv_frametimes` counts why. Measured on mp_afghan (1 player, 3 bots, 150 s): 9215 delta
snapshots and 4 whole ones, 6.4 entities written and 117 carried over per snapshot, **38.5 kB/s in 60 packets a
second** (one 643-byte packet per snapshot instead of three), send time 0.79 ms.

The setting is off by default until it has been played on more maps and modes. `2` is `1` plus a trace of every
field written (`qa_delta_trace.log` in the server's working directory).

How it was checked, since a wrong bit here shows up as a client crash a few seconds later ("156" glass state,
access violations in cgame):

- **`tools\dev\trace_delta.py [seconds]`** (r1-mod, frida) hooks the client's `CL_ParseSnapshot` and
  `MSG_ReadDeltaEntity` and records which entities each snapshot decoded, how far the message was read, and the
  decoded entity itself.
- **`tools\dev\compare_delta.py <server trace>`** matches the two traces by server time: the same entities on both
  sides, no overrun, and every written field equal in the client's memory. Last run: 1521 delta snapshots, entity
  lists equal in all, 1714 values compared, 0 wrong.

The traces found the one real bug in minutes after reading the decoder had found none: the list writer rewinds
after an entity that turns out unchanged, and the bit writer ORs into the byte it is filling, so the rewound
entity-number bits stayed and corrupted the next number (107 arrived as 127). The same rewind in the whole-list
writer (an entity left out for an unrepresentable value) had the same fault. `rewind_to` now clears those bits.

Still sent whole or refused:

- the **player state** and its arrays (weapons, ammunition, HUD), about 500 of the 643 bytes. The client reads a
  bit for "from the old player state"; using it is the next reduction and needs the same from-old forms for the
  235 player-state fields.
- an entity whose type changes in place, and changed plane entities (table 14 has fields the client reads
  unconditionally): that snapshot goes out whole.

The QA client keeps its clock 16 ms behind the newest snapshot (`CL_AdjustTimeDelta`, 0x4FA5B0), so below 60
snapshots a second it extrapolates between them and movement looks coarser; with deltas on, `sv_fps 60` costs
about a fifth of what it did, so the trade is easier.

### 6. Console and rcon work inside the frame (open)

Rcon commands run where packets are handled, inside the frame loop. `status` (asked by admin tools every few
seconds) and every developer print go through the console log, which `tools\dev\run_native.ps1` opens with
`logfile 2` (flushed per line) and `developer 1` for debugging: about 15 lines a second with one player. `net
over` showed up to 20 ms in a frame. Changing these flags on a running server made no difference to the average
send time, so they are a spike source, not a steady cost.

## Running it well

**Launch flags for a real server** (the development script uses the opposite on purpose):

- `+set developer 0`, `+set logfile 1` (buffered) or `0`
- no `codol_qaDumpSnapshots`
- `+set codol_qaDeltaEntities 1` once it has been tried on your maps (a fifth of the bandwidth per player)
- `+set g_logsync 1` only when an admin tool reads the game log (IW4M-Admin); otherwise `0`
- `+set sv_fps 60` for a few players on a good link, `30` or `20` as the player count or the link requires

**By environment.**

- *Windows, native.* Covered by the timer fix. Check with `sv_frametimes`: `interval` p99 should stay within about
  1 ms of the frame length.
- *Linux, native build.* `select()` and `clock_gettime(CLOCK_MONOTONIC)` are already precise; the timer fix is
  Windows-only and not needed. The session-file and lookup fixes apply. Not run here.
- *Wine / Proton (the Windows build on Linux).* `timeBeginPeriod` is accepted and sleeps are already ~1 ms. File
  flushes are real `fsync` calls there, so finding 3 mattered most on this path. Not run here.
- *Virtual machines.* Hypervisors coalesce timers and steal time: expect `interval` p99 to widen first. Give the
  server a dedicated vCPU, keep `sv_fps` at 20-30 if `interval` p99 exceeds 1.5 frames, and keep logs off slow
  virtual disks. Not run here.
- *Translation layers on ARM or embedded devices (x86 emulation).* The per-client snapshot cost (finding 5) is CPU
  bound and will scale with the emulation overhead: measure `send` with one player and multiply. `sv_fps 20` and a
  small player limit are the safe start; delta snapshots cut both the time and the packets. Not run here.

The three "not run here" environments are reasoned from the code and the measurements above; `sv_frametimes` and
`snap_cadence.py` are the way to confirm them on the actual host.

## Client side

- r1-mod's background loop (`timescale.c`) wakes every 50 ms to keep a handful of values; it does no network work.
- The QA client's 16 ms clock offset is fixed in the client; widening it (an interpolation delay as a match
  variable) would let lower snapshot rates look smooth and is the client-side half of finding 5.
- `cx_frametime` reports the client's own frame times, which bound how evenly it can read snapshots.
