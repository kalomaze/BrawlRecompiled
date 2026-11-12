# Brawl Recompiled

A native PC port of **Super Smash Bros. Brawl** (Wii, 2008), made by statically recompiling the game's PowerPC code
(main.dol and all 126 `.rel` modules) to C++ with [WiiCompiled](https://github.com/patchzyy/Wiicompiled). No
emulator: the game runs as a Windows program.

![Donkey Kong vs the Ice Climbers on Castle Siege, rendered at 4K](screenshots/05_castle_siege.jpg)

**Work in progress.** What works today: boot, saves, every menu tested so far, VS battles, the attract demos,
Classic mode's start, and the Subspace Emissary's opening (cutscenes and the first battle). Project+ and
Project+Ex load from the virtual SD card and play matches with their own characters and stages; REX, Brawl
Minus and Legacy XP load the same way (see Mods).

## Screenshots

Rendered at 4K (shown here at 1080p).

| | |
|---|---|
| ![Opening](screenshots/01_opening.jpg) | ![Main menu](screenshots/02_main_menu.jpg) |
| ![Character select](screenshots/03_character_select.jpg) | ![Stage select](screenshots/04_stage_select.jpg) |
| ![Castle Siege](screenshots/06_castle_siege_2.jpg) | ![The Subspace Emissary: Kirby](screenshots/07_subspace_kirby.jpg) |
| ![The Subspace Emissary: Kirby vs Mario](screenshots/08_subspace_vs.jpg) | ![The Subspace Emissary's first battle](screenshots/09_subspace_battle.jpg) |
| ![REX's roster](screenshots/10_rex_roster.jpg) | ![A REX match: Ivysaur vs Incineroar](screenshots/11_rex_match.jpg) |
| ![The options menu](screenshots/12_options_menu.jpg) | ![Save states and fast-forward](screenshots/13_save_states_fast_forward.jpg) |
| ![Cheats](screenshots/14_cheats.jpg) | ![The giant fighters cheat](screenshots/16_giant_fighters_cheat.jpg) |

![Battlefield on a 32:9 screen: more of the stage, the HUD kept in the middle](screenshots/15_ultrawide_32x9.jpg)

## Features

- Up to 4K and beyond (render resolution up to 8x), the game's own 16:9, and Fill Window for ultrawide screens up
  to 32:9: more of the stage to the sides, menus and HUD kept in proportion
- 120 fps frame interpolation, on by default (180 and 240 too; the game itself runs at 60)
- An options menu inside the game, in Brawl's own style (F10, or Home on a controller): graphics, audio,
  controls, gameplay, cheats, mods, online play and updates. No separate launcher: the game starts straight away
- Save states (F5 / F7, three slots), fast-forward (F8, 2x to 4x) and fast menus
- Cheats: Gecko codes in Dolphin's format, switched on and off while you play; nine tested codes come with it
- Every gamepad is a GameCube controller, with its buttons remappable in the game (Nintendo or by-position
  layouts); keyboard and mouse for any player; real Wii Remotes over Bluetooth
- Installs from your own disc image (.iso, .ciso, .wbfs) or an already extracted game folder, and updates itself
- Saves in `UserData\NAND`, compatible in layout with a Wii's
- A virtual SD card (`UserData\SD`), so the game's own SD features and SD-based mods work
- Gecko codes and Project+-style mods (see below)
- Online play between Brawl Recompiled players (see below)
- Windows (with a build for processors without AVX2) and Linux / Steam Deck

**No game data is included**: the game's files are read from your own copy of the game.

## Mods

Brawl mods are installed the way they are on a Wii's SD card:

- **The SD card** is the folder `Play\UserData\SD`. Copy a mod's SD card contents into it (for Project+: the
  `Project+`, `private` and `apps` folders from the Wii download). At start the game gets a FAT32 card
  (`UserData\Cache\sd_card.raw`, rebuilt only when the folder changes) whose files are read straight from that
  folder, so a mod takes no extra disk space; files the game writes to the card go back into the folder when it
  exits. A card over 2 GiB is an SDHC card, which Brawl reads only with a
  mod's SDHC support (Project+ has it). A Dolphin `sd.raw` placed at `UserData\sd.raw` is used directly instead.
- **Gecko codes**: the `RSBE01.GCT` of the card folder chosen in Options > Mods ("Build to play",
  `[mods] sd_codes` in `Config.toml`), else the first folder with one (`UserData\SD\Project+\RSBE01.GCT`), else
  `UserData\Codes\RSBE01.gct`, is installed with USB Loader GX's code handler, honouring a `gc.txt` beside it
  (code list address, hook type, pokes) exactly as the mods' own loaders do: the handler runs once before the
  game's first instruction and then at every frame.
- Code the codes patch runs in a built-in PowerPC interpreter; everything else stays native. Modules a mod
  replaces (every fighter in Project+, the character select and the whole melee engine in Project+Ex and REX)
  run in the interpreter too, at full speed.

Several builds can share one card; "Build to play" picks the one that runs, and "Vanilla Brawl (no mods)" plays
the unmodded game whatever the card holds (no codes and no card). The builds tried, each copied from
its Wii (console / "hackless") download into `UserData\SD` as the folder named here:

| Build | Card folder | Get it from |
|---|---|---|
| Project+ 3.2.1 (Knuckles, Mewtwo, Roy, …) | `Project+` | [projectplusgame.com](https://projectplusgame.com) (Wii "Lite") |
| Project+Ex 1.7 (BrawlEx: Ridley, Waluigi, Sceptile, Dark Samus, …) | `P+Ex` | [KingJigglypuff/project-plus-ex](https://github.com/KingJigglypuff/project-plus-ex/releases) (Wii zip) |
| Super Smash REX Release 0½ (PMEX Remix's successor, 100+ fighters) | `rex_` | [rexbuild.site/downloads](https://www.rexbuild.site/downloads) (Wii .rar) |
| Brawl Minus 5.1.5 | `Minusery` | [brawlminus.net](https://brawlminus.net/) (Console zip) |
| Legacy XP 2.11 | `LegacyXP` | [unofficialsmashlegacy.weebly.com](https://unofficialsmashlegacy.weebly.com/legacy-xp.html) (Wii) |

Only that folder is needed (the downloads' `apps`, `private`, `codes` and loader files are for a real Wii). Setting
up the card after adding a build takes seconds (the title card shows it). Costumes, stages and movesets from [Brawl Vault](https://forums.kc-mm.com/Gallery/)
install into a build's `pf` folder exactly as on a Wii.

## Online play

Press **F9** in the game (or Options > Online):

- **Host a room** for 2 to 4 players: list it publicly, or keep it to friends, who join with its 5-letter room code.
- **Find rooms** shows public rooms, rooms on your network and rooms over [Tailscale](https://tailscale.com).
- **Join with a code** takes a friend's room code (or an IP address).
- Starting a room hands everyone the host's save and restarts every game into the match together.

Over the internet the games connect directly to each other, with no port forwarding: a free matchmaking server only
introduces them (see `server/`). A few home and mobile connections can't be reached that way; if a code never
connects, Tailscale gets around it.

Every player needs the same build: vanilla, or the same mod (the Mods page; online with a mod is new, and works best
when everyone's `UserData\SD` folder is a copy of the same download). The games run in lockstep: each
frame's controller input is all that is sent, and the games check every frame that they still match. A player's
own controller (port 1) is their player number in the room. Each frame waits for every player's input, so a slow
connection adds input delay (2 to 8 frames, picked from the ping when the room starts). Cross-play with Dolphin
(Brawl Online, Slippi-style netplay) is not possible: those run an emulator, and the games would not stay in step.

## Building from source

You need your own disc image (USA, RSBE01), and CMake 3.25+, Python 3.12+, Git, Clang and Ninja on `PATH`.

On Windows, install [Python 3.12](https://www.python.org/downloads/windows/) (with "Add python.exe to PATH") and
[Git](https://git-scm.com/download/win). Then, in PowerShell in the clone, fetch the pinned llvm-mingw, CMake and Ninja
and put them on `PATH`:

```powershell
powershell -ExecutionPolicy Bypass -File wiicompiled\Launcher\Prepare-PortableTools.ps1 -Destination "$PWD\toolchain"
$env:PATH = "$PWD\toolchain\llvm-mingw\bin;$PWD\toolchain\CMake\bin;$PWD\toolchain\Ninja;$env:PATH"
```

On Ubuntu:

```sh
sudo apt install cmake python3 git clang lld ninja-build build-essential pkg-config libicu-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev libxrender-dev \
  libxinerama-dev libwayland-dev wayland-protocols libdecor-0-dev libxkbcommon-dev libgl-dev libegl-dev libgbm-dev \
  libdrm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libusb-1.0-0-dev libibus-1.0-dev
```

Then, on Linux or Windows:

```sh
cmake -DDISC=/absolute/path/to/brawl.iso -P extract.cmake   # once: extracts the disc to Disc/DATA
cmake -P build.cmake
```

ISO, CISO and split WBFS images work; keep `.wbf1`, `.wbf2`, etc. beside the `.wbfs`. The build downloads what it
needs (so the first one needs internet access), keeps everything it makes in `build/`, and produces
`build/native/WiiCompiled` (`WiiCompiled.exe` on Windows), set up to use the extracted disc, with its settings and
saves in `build/native/UserData`. Rerun it to resume. Options for `build.cmake`:

- `-DJOBS=2` uses less memory (the default is 4 jobs)
- `-DBUILD_DIR=<folder>` builds somewhere other than `build/`
- `-DGAME=<folder>` uses an extracted disc other than `Disc/DATA` (give it to `extract.cmake` too)
- `-DCMAKE_BUILD_TYPE=Debug` or `RelWithDebInfo` builds with debug symbols (the default is `Release`)
- `-DMKW_HOST_ISA=v1` (SSE2) or `v2` (SSE4.2) builds for x86-64 processors older than the default `v3` (AVX2)
- `-DMKW_FLAT_MEMORY=OFF` uses ordinary memory instead of reserving a fixed 4 GiB address range

The translator's tests are described in [`wiicompiled/translator/tests/README.md`](wiicompiled/translator/tests/README.md).

### Release packaging

After `extract.cmake`, `tools\build-brawl.ps1` builds the Windows release into `Play\` (`-OldCpu` adds the build for
processors without AVX2). The release (`Brawl Recompiled.exe` plus a hidden `engine\` folder) is made by
`tools\build-launcher.ps1` (the launcher, Windows and Linux) and `tools\package-brawl.ps1 -Zip`;
`tools/package-linux.sh` makes the Linux download from a `build.cmake` build on Linux.

| Folder | What it is |
|---|---|
| `wiicompiled/translator` | PowerPC to C++ static recompiler (C#) |
| `wiicompiled/runtime` | Runtime and Wii SDK HLE (OS, DVD, NAND, SD, WPAD/KPAD, AX audio, VI, GX) |
| `wiicompiled/aurora-main` | Aurora: Wii GX on WebGPU (Dawn / Direct3D 12) |
| `wiicompiled/projects/brawl` | This game's project: function map, module layout, code handler, tools |
| `launcher` | The launcher (WinForms; `linux\` is the same pages in Avalonia) |
| `tools` | Build and packaging scripts |

## Credits

- **Port:** JosiahG
- **[WiiCompiled](https://github.com/patchzyy/Wiicompiled)** (the recompiler and runtime) by patchzyy and
  contributors, GPLv3
- **Aurora** by Luke Street, MIT; Dawn, SDL3 and the other components listed in
  [`wiicompiled/THIRD-PARTY-NOTICES.md`](wiicompiled/THIRD-PARTY-NOTICES.md)
- **Gecko code handler:** USB Loader GX's `codehandleronly` (Gecko OS), GPL
- **[FatFs](http://elm-chan.org/fsw/ff/)** by ChaN (the SD card image), 1-clause BSD
- **Dolphin Emulator** for the reference behaviour of the SD slot and data files (DSP coefficients, Wii system
  files), GPL-2.0-or-later
- Symbol names from the [Brawl decompilation](https://github.com/doldecomp/brawl) and Dolphin's signature database

## License

GPLv3, see [`wiicompiled/LICENSE`](wiicompiled/LICENSE). Bundled third-party components keep their own licenses.

Super Smash Bros. Brawl and all related characters are trademarks and copyrights of Nintendo, HAL Laboratory and
their respective owners. This is an unofficial fan project, not affiliated with or endorsed by Nintendo. This
repository contains no game code or data; the game's disc data is read from your own copy of the game.
