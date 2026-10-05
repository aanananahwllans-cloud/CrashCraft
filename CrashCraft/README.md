# CrashCraft: Minecraft inside Crash Bandicoot

Play the original Crash Bandicoot (PS1) as a Minecraft player. Steve walks, jumps, builds, breaks crates and fights with Minecraft's physics, inventory, HUD and your own skin, inside Crash's real levels. Crash's world still runs itself: fruit, crates, checkpoints, Aku Aku, enemies and deaths all behave as in the original.

Two games run at once:

- **Crash Bandicoot** runs on [c1](https://github.com/wurlyfox/c1), wurlyfox's port of the game to C, built from your own disc's level files. CrashCraft patches c1 to draw everything, including Minecraft's blocks, player and HUD inside Crash's frame with a real depth buffer.
- **Minecraft 26.3 + Fabric** runs hidden with the CrashCraft mod, a re-pointed copy of [SkyCraft](https://github.com/chasmlol/SkyCraft) by chasmlol. Its physics, inventory and combat run unchanged against Crash's level collision.

They talk through shared memory using SkyCraft's protocol. One Crash crate is one Minecraft block.

> **Status: early and experimental.** Developed and tested on N. Sanity Beach with the PAL disc. Other
> levels load, but c1 itself is still a work in progress (see [c1's known issues](https://github.com/wurlyfox/c1/blob/master/doc/issues.md)).

## What works

- **Steve in Crash's levels:** the whole level's collision (Crash's zone octrees) streams to Minecraft, so you walk, jump, sprint and fall on Crash's ground and on his crates.
- **Steve's camera, free:** first person or third person (F5, the default, shows your skin). Crash's world is drawn with every polygon depth-tested instead of the rail camera's precomputed lists, so you can look anywhere. The game is fullscreen and widescreen.
- **Crates:**
  - Attack any crate (sword, fist, arrows, TNT) to break it as Crash would: fruit, ? crates, extra lives, checkpoints and Aku Aku all work.
  - Standing on or walking into crates never breaks them.
  - Arrow (bounce) crates throw you up when you land on them, and hitting a crate from below breaks it.
- **Enemies:** hit them with anything to defeat them. They hurt you (4 hearts a hit, at most once a second).
- **Dying:** Minecraft death, Crash's pits and hazards, or falling off the level gives Crash's death and respawn at the last checkpoint. Steve comes back with him.
- **Minecraft:** place and break blocks, mobs, items and crafting, all drawn inside Crash's world.
- **Mouse and keyboard** for both Steve and Crash's own menus.

## Requirements

- **Windows 10/11.**
- **Your own copy of Crash Bandicoot (PS1)**, dumped from your disc as `.cue/.bin` (or a `.zip` of them). Tested with the PAL disc (SCES-00344). The USA disc (SCUS-94900) should work too, since c1 reads the level files the same way. Nothing from the game is included here.
- **Minecraft: Java Edition** and [Prism Launcher](https://prismlauncher.org/).
- **[MSYS2](https://www.msys2.org/)** to build c1. c1 has no license, so CrashCraft can't ship a prebuilt c1. You build it once.
- **Git** and **Python 3**.

## Installing

### 1. Build Crash (c1 + CrashCraft)

In the **MSYS2 MSYS** shell, install the 32-bit toolchain once:

```bash
pacman -S --needed git make mingw-w64-i686-gcc mingw-w64-i686-SDL2 mingw-w64-i686-fluidsynth
```

Then, from this folder (Git Bash or MSYS2):

```bash
bash setup.sh "C:/path/to/Crash Bandicoot (Europe).cue"
bash build_c1.sh
```

`setup.sh` clones c1 at the tested commit, applies `patches/c1-crashcraft.patch`, adds CrashCraft's sources from `host/`, and copies the level files (`S0`–`S3/*.NSD/*.NSF`) from your disc into `c1/streams`. If MSYS2 isn't in `C:\msys64`, put `MSYS2_ROOT=D:/msys64` in a file named `local.env` next to `build_c1.sh`.

### 2. The Minecraft instance

1. In Prism, create an instance named **CrashCraft**: Minecraft **26.3**, Fabric Loader **0.19.5** or newer.
2. Add [Fabric API](https://modrinth.com/mod/fabric-api) `0.161.0+26.3` and `crashcraft-<version>.jar` (from this repo's Releases, or build it with `bash gradle_mc.sh build`, which needs `JAVA_HOME` set to a Java 25 JDK) to its mods folder.

### 3. Play

```bat
run_crashcraft.bat
```

This starts Crash fullscreen and launches the Prism instance. Use `run_crashcraft.bat 9` to start straight in N. Sanity Beach. If Prism isn't in its default folder, or your instance has another name, create `local.bat` next to it:

```bat
set PRISM=D:\PrismLauncher\prismlauncher.exe
set PRISM_INSTANCE=CrashCraft
set MSYS2_ROOT=D:\msys64
```

Order doesn't matter: the two games find each other within a second. Minecraft hides its window and opens its own world, `CrashCraft`. Closing Crash closes Minecraft.

## Controls

**In a level (Steve):**

| Input | Does |
|---|---|
| Mouse, WASD, Space, Shift, Ctrl, 1-9, E, Q, F5, T... | Minecraft, as usual |
| Left / right click | Attack (break crates, hit enemies) / use, place blocks |
| Enter or Esc | Crash's pause menu |
| O | Minecraft's pause / options menu |
| F11 | Fullscreen on/off |

**Crash's menus (title, map, pause), or Crash without Minecraft:**

| Input | Pad |
|---|---|
| WASD / arrows | D-pad |
| Space, Z, left click | X (confirm, jump) |
| Shift, E, X, right click | Square (spin) |
| Q, Ctrl | Circle |
| F, Tab | Triangle |
| Enter, Esc | Start |
| Backspace | Select |

F1 shows c1's debug window and F2 gives it the keyboard.

## Troubleshooting

- **Logs:** `c1/crashcraft.log` (the Crash side) and the Prism instance's `logs/latest.log` (lines starting `CrashCraft:`).
- **No sound or music:** c1's music needs a soundfont that newer FluidSynth versions refuse. Known issue.
- **Black screen or crash at a level start:** some levels hit c1's own known issues. Try N. Sanity Beach (`run_crashcraft.bat 9`).
- **Start in a window:** `set CRASHCRAFT_WINDOWED=1`. **Play Crash alone** (no Minecraft): `set CRASHCRAFT_OFF=1`, or just don't start Minecraft.

## Known issues

- Only N. Sanity Beach is well tested. Other levels: c1's own issues apply.
- The sky behind Steve's free camera is a flat color (Crash's cloud backdrop isn't drawn yet), and music is silent.
- Crash runs at 30 fps, as on the PS1, so mouse look is 30 Hz.
- Crate types beyond N. Sanity Beach's (iron, Nitro, outline crates) aren't specially handled yet.

## Developers

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md): how the pieces fit, c1's renderer and collision, the crate probe, and the test commands.

## Credits

- **[c1](https://github.com/wurlyfox/c1)** by wurlyfox: Crash Bandicoot ported to C. CrashCraft only ships a patch against it.
- **[SkyCraft](https://github.com/chasmlol/SkyCraft)** by chasmlol (MIT): the Fabric mod, the shared-memory protocol and the host modules (link, collision streaming, renderer) CrashCraft is built on.
- [Fabric](https://fabricmc.net/), [SDL2](https://libsdl.org/), [FluidSynth](https://www.fluidsynth.org/), [Dear ImGui](https://github.com/ocornut/imgui) (bundled in c1).
- Crash Bandicoot © Activision / Naughty Dog / Sony. Minecraft © Mojang / Microsoft. This is a fan project, not affiliated with any of them. You need to own both games.

**AI disclosure:** CrashCraft was built with Claude Code (Anthropic's Claude Opus 5.5), directed and play-tested by a human.

## License

CrashCraft's own code is MIT (see [LICENSE](LICENSE)). The Fabric mod keeps SkyCraft's MIT license ([fabric/LICENSE](fabric/LICENSE)). c1 is not part of this repository. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
