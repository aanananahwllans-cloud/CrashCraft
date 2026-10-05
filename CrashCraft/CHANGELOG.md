# Changelog

## 0.1.0 (2026-10-05)

First release.

- c1 on Windows: MSYS2 32-bit build, GL loader, FluidSynth 2.x, Windows timer. Boots from your own disc's level files (`tools/extract_disc.py`).
- Real depth buffer and free camera for c1: homogeneous vertices, every zone polygon drawn depth-tested.
- Fullscreen and widescreen, a sky-colored background for the free camera, and mouse and keyboard for Crash.
- Minecraft link (SkyCraft's protocol and mod, renamed): whole-level collision from Crash's zone octrees, Steve's camera, Minecraft's blocks, player and HUD drawn in Crash's frame, third person by default.
- The Crash puppet: Crash stands where Steve is, hidden, so fruit, crates, checkpoints and enemies react to Steve.
- Crates break only when attacked. Bounce crates launch (new protocol input `kInLaunch`), and hitting a crate from below breaks it. Enemies take Crash's spin.
- Hazards, pits and falling off the level give Crash's death and respawn. Enemy hits are at most one per second.
- Test commands (`c1/crashcraft.cmd`) and the crate probe (`CRASHCRAFT_PROBE=1`).
