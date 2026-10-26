# Pokémon Snap 3DS: Double Take!!

A native Nintendo 3DS port of **Pokémon Snap** (N64, USA), statically recompiled with
[N64Recomp](https://github.com/N64Recomp/N64Recomp). Its renderer and audio are written for the 3DS,
and there are extras made for the console:
- the touch screen shows each course's best photos and the items
- Camera Check (choosing pictures for Prof. Oak and the album) works on the touch screen, with the
  chosen picture at high resolution on the top screen
- gyro aiming and stereoscopic 3D

**No game data is included.** You need your own dump of Pokémon Snap (USA). Its SHA-1, in .z64 byte
order, is `edc7c49cc568c045fe48be0d18011c30f393cbaf`. Put it in `sdmc:/3ds/psnap/` or
`sdmc:/roms/n64/`. It can have any file name and be .z64, .v64 or .n64. The app checks it on first
start (see `source/port/rom_check.c`).

## What's in the repo
- `source/`: the port (main loop, renderer, audio, input, the touch screen, game hooks)
- `runtime/`: the N64Recomp runtime (librecomp / ultramodern), adapted for the 3DS
- `recomp/`: the N64Recomp configuration, symbols and hooks
- `tools/`: asset scripts (`make_*.py`), banner, test scripts for Azahar
- `docs/`: controls and the development log

## What's not in the repo
These are all made from the game, so they stay out:
- the recompiled code (`recomp/RecompiledFuncs/`, `recomp/rsp/`)
- graphics and sounds cut from the game: `gfx/*.png` except the subtitle, `data/`,
  `assets/banner/` except the subtitle, `source/render/cc_parts_rgba.h`, `icon.png`
- builds, the ROM, and emulator folders

To build it yourself:
1. Regenerate the recompiled code from your ROM: `cd recomp && N64Recomp pokemonsnap.us.3ds.toml`
   (and `aspMain.us.3ds.toml` for the RSP audio microcode).
2. Recreate the art with the `tools/make_*.py` scripts. Each script's header says what it reads.
   Most of them work from RAM and texture snapshots taken while the port runs in Azahar.
3. Build with devkitARM (devkitPro): `make`, `make cia` (needs `makerom` and `bannertool` in
   `tools/cia/`).

## Credits
Pokémon Snap © Nintendo / HAL Laboratory / Creatures / GAME FREAK. This is an unofficial fan
project, not affiliated with or endorsed by them.
Built with N64Recomp and N64ModernRuntime by Mr-Wiseguy and contributors, and with libctru,
citro3d and citro2d by devkitPro.

## License
MIT (see `LICENSE`), for the port's own code, tools and art. `runtime/` is adapted from
N64ModernRuntime (MIT), and its `thirdparty/` libraries keep their own licenses. The license
doesn't cover Pokémon Snap itself or anything made from it.
