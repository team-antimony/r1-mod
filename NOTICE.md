# Notice

## Project scope

r1-mod is a client modification and launcher. It is not affiliated with, authorized, or endorsed by Activision or
Tencent; Call of Duty Online and its assets are their property. This repository contains none of the game's files.

This repository does **not** include tools to decrypt, unpack, or otherwise circumvent the protection of any game,
and it does not distribute game content. Running r1-mod requires a copy of the Call of Duty Online 3.3.3.3 QA build
that you supply and prepare yourself; preparing that build is outside the scope of this project.

What the tools here do:

- `tools/data` reads tables and settings from the memory of your own running client (with frida) and writes them to
  `data\`, which is not in git.
- `tools/net` decodes the client-server network protocol (packets, snapshots, rcon) for debugging.
- `backend/` is a local stand-in for the online services the client talks to; its certificates and keys are
  generated on your PC.

Files you may add yourself, and which r1-mod never ships: a translation of the game's text (`r1_english.txt`, read by
`cx_loc`), inspect animations from your own 3.19.26.22 install (`r1_xanims_319.bin`), and the tables in `data\`.

## Third-party code

| Component | Where | License |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.7, with its DX9 and Win32 backends | `deps/imgui` | MIT ([deps/imgui/LICENSE.txt](deps/imgui/LICENSE.txt)) |
| [premake](https://premake.github.io/) 5, which includes Lua 5.3 | `tools/premake5.exe` | BSD-3-Clause ([tools/premake-license.txt](tools/premake-license.txt)); Lua: MIT |
| [Hide 'n Seek](https://github.com/KristofMorva/hns) by Kristóf Morva, ported to this game | `mods/hns` | MIT ([mods/hns/LICENSE](mods/hns/LICENSE)) |
| The movement dvars, a port of [iw4x](https://github.com/iw4x/iw4x-client)'s `PlayerMovement` | `src/client/component/movement.c` | GPL-3.0 |

Everything else is r1-mod's own and licensed under GPL-3.0 ([LICENSE](LICENSE)).
