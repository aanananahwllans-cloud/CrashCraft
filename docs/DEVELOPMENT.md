# CrashCraft: development notes

Follows chasm's SkyCraft (MIT, github.com/chasmlol/SkyCraft), via SM64Craft (the same design for Super Mario 64).
- Host = **c1** (`c1/`, github.com/wurlyfox/c1: Crash 1 ported to C, OpenGL + SDL2), built here for Windows
  with MSYS2 MinGW32 and run on the level streams from the user's own disc.
- Guest = SkyCraft's Fabric mod for Minecraft 26.3, re-pointed at Crash (`fabric/`).
- Link = SkyCraft's shared-memory protocol (`protocol/`), mapping `Local\CrashCraft_v1`.

c1 has **no license file**: never publish c1 sources or builds. This repo ships CrashCraft's own files
(`host/`) plus `patches/c1-crashcraft.patch` against upstream c1; `setup.sh` puts them together.
After changing c1 sources, regenerate the patch with `git -C c1 diff --ignore-cr-at-eol > patches/c1-crashcraft.patch`
and copy new CrashCraft files back into `host/`.

## Status (2026-10-05)
- [x] Recon: developed with the PAL disc (SCES-00344), Minecraft 26.3 + Fabric 0.19.5, Java 25.
- [x] `tools/extract_disc.py <zip|cue|bin>` copies S0-S3 *.NSD/*.NSF from the disc into `c1/streams`
      (lowercase names) and records the serial in `c1/streams/REGION`. Nothing from the disc is shipped.
- [x] c1 builds on Windows: `bash build_c1.sh` (MSYS2, mingw-w64-i686 gcc/SDL2/fluidsynth,
      `-std=gnu11` because GCC 15 defaults to C23). Port fixes: GL >1.1 loader (`src/pc/gfx/glload.*`),
      FluidSynth 2.x callback types, `open_memstream` fallback, Windows tick timer (overflow-free).
- [x] PAL streams boot: Naughty Dog intro and `c1.exe --level 9` (N. Sanity Beach) render and play.
- [x] Real depth buffer: every prim carries homogeneous verts (`hvec`, `PRIM_DEPTH_*` in pcgfx.h); GL clips
      and depth-tests. Free camera draws every polygon of the zone's worlds instead of the rail's SLST list.
- [x] Host (`c1/src/pc/crashcraft/`): link/collision/render from SM64Craft (portal+crater code removed,
      GLEW/miniz replaced by `glload` and `png.h`), new `host.cpp` + `crashcraft_game.c` (c1 half).
- [x] Fabric mod = SkyCraft's, renamed (`fabric/`, `crashcraft-0.1.0.jar`), Discord presence off,
      third person by default (`-Dcrashcraft.firstPerson=true` to keep first person). Runs with fabric-api 0.161.0+26.3.
- [x] End to end on N. Sanity Beach with a real account (skin shows in third person): whole-level
      collision (123k octree cells -> 1.48M tris), Steve walks/jumps on Crash's ground and on crates,
      fruit pickup, Aku Aku, enemies hurt Steve (1 hit/s cooldown, 4 hearts), MC death -> Crash death ->
      respawn -> teleport. Crates only break when attacked (GoolObjectBound skips box<->Crash contact).
- [x] Fullscreen (desktop res, F11 toggles, `CRASHCRAFT_WINDOWED=1`), widescreen for Steve's camera,
      Crash's own camera pillarboxed 4:3. Background = sky color while Steve's camera is on.
- [x] Mouse + keyboard for Crash himself (pc/pad.c): WASD/arrows, Space/LMB = X, Shift/E/RMB = square,
      Q/Ctrl = circle, F/Tab = triangle, Enter/Esc = start. c1 debug GUI moved to F1/F2.
- [x] Crates: `CRASHCRAFT_PROBE=1` (c1 alone) spawns one crate of each subtype and logs its reaction.
      Beach: subtype 2 plain, 6 fruit (multi-hit from below), 10 ?, 9, 8, 4 break on Crash's jump;
      3 = bounce crate (answers GOOL_EVENT_BOUNCE 0x1500, stays); 5, 7, 13 don't break on a jump.
      Crates ignore events unless Crash really lands on them, so an attack runs Crash's own code (hidden)
      for 12 frames, dropped onto the crate (`CrashMove`). Enemies take the spin event (crab dies).
      Standing/walking on crates never breaks them. Landing on a bounce crate -> `kInLaunch` (new protocol
      input 9, 0.8 blocks/tick, no fall damage). Rising into a crate = head bump (Crash's hit from below).
- [x] Stand-ins bigger than the real thing (crates +0.15, enemies +0.3 blocks) so Minecraft's aim hits
      them before the crate's own solid block. Enemy hits: 1 per second, 4 hearts each.
- [x] Void: hazard volumes and zone bounds cached per level; outside every zone and 3+ blocks below the
      last ground -> Crash's fall death. (Teleporting into space with no collision data freezes Minecraft's
      player by design: SkyClient.holdUntilReady.)
- [x] Test commands: `c1/crashcraft.cmd` lines `key <sc> <1|0>`, `click <btn>`, `look <yaw> <pitch>`,
      `tp <x> <y> <z>` (c1 units), `cell <x> <y> <z>` (octree leaves + objects there),
      `event <EID name> <hex event | ffff = attack>`, `shot`.
- [ ] Sky backdrop (clouds) missing with the free camera: background is the flat sky color.
- [ ] Music: newer FluidSynth rejects c1's generated SF2 ("pmod chunk size is not a multiple of 10").
- [ ] Verify: attacking crates/enemies (kEvHitActor -> GOOL spin event), TNT/Nitro, checkpoints, warps,
      level end, other levels (c1's own known issues: doc/issues.md).
- [ ] 30 fps: c1 logic and drawing are tied; mouse look integrates at 30 Hz.
- [ ] Release packaging: patch against upstream c1 + CrashCraft files, never c1 sources or disc data.

## Playing
`run_crashcraft.bat [level id]` starts c1 (fullscreen) and the Prism instance (paths in `local.bat`). In a level: Minecraft's
controls (mouse look, WASD, Space, Shift, clicks, 1-9, E, F5...), Enter/Esc = Crash's pause menu, O =
Minecraft's menu. Debug: `crashcraft.log` next to c1.exe; touch `c1/shot.req` -> `c1/shot.png`.

## Running (dev)
`cd c1 && PATH=<msys2>/mingw32/bin:$PATH ./c1.exe [--level <id>]` (ids in `src/common.h`, LID_*;
0x09 N. Sanity Beach). `tools/capture.ps1 -Out <png>` grabs the window from the screen.

## Rendering facts (c1)
- All 3D is transformed in software (`src/pc/gfx/soft.c`, `SwRotTransPers`): out = screen x/y (pixels, centre
  origin, 512x240 PSX screen) and z = view-space depth. Prims go into a 2048-entry ordering table and are
  painted back to front by `GLDraw` (`src/pc/gfx/gl.c`) with z forced to -1 and
  `glFrustum(-256,256,-120,120,1,1e5)`, i.e. a 2D screen-space draw with no depth buffer.
- Plan: emit each vertex as (x·w, y·w, -w) with w = z/scale. The image stays the same, but the depth buffer
  gets real view depth (PGXP-style). Crash still paints in OT order (depth func ALWAYS, write on), then
  Minecraft draws with LEQUAL against it.
- Camera: `GfxUpdateMatrices` (`src/gfx.c`): `cam_rot` (Z, then Y, then X; 12-bit angles), `cam_trans`
  (world units <<8). `ms_cam_rot` = view rotation with y scaled by -5/8 and z negated; `params.screen_proj`
  = projection distance.
