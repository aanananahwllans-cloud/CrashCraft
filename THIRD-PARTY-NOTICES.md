# Third-party notices

## SkyCraft (MIT)

`fabric/` is SkyCraft's Fabric mod, renamed and re-pointed at Crash Bandicoot. `protocol/crashcraft_protocol.h` is SkyCraft's shared-memory protocol with its own magic, mapping name and units. `host/src/pc/crashcraft/{link,collision,render}.*` and `clip.h` are ports of SkyCraft's SKSE plugin modules (via SM64Craft).

Copyright (c) chasmlol and SkyCraft contributors, MIT License: see `fabric/LICENSE`.
Source: https://github.com/chasmlol/SkyCraft

## c1 (no license)

CrashCraft runs on wurlyfox's c1 (https://github.com/wurlyfox/c1), a port of Crash Bandicoot to C. c1 has no license, so this repository doesn't contain it, and no c1 build is distributed. `setup.sh` clones it from its author's repository, and `patches/c1-crashcraft.patch` holds CrashCraft's changes to it. The patch's context lines are c1's.

c1 bundles Dear ImGui / cimgui (MIT, Omar Cornut and contributors).

## Libraries used when building

- SDL2: zlib License.
- FluidSynth: LGPL-2.1 (linked dynamically from MSYS2).
- Fabric Loader / Fabric API: Apache-2.0.

## Games

Crash Bandicoot belongs to Activision, Naughty Dog and Sony Interactive Entertainment. Minecraft belongs to Mojang Studios and Microsoft. No game files are included. Each player uses the level files from their own disc, and their own Minecraft: Java Edition.
