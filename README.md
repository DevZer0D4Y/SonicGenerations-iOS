# Sonic Generations for iPhone and iPad

A static recompilation of *Sonic Generations* (Xbox 360, 2011) for iOS, based on
[Sonic Generations Recompiled](https://github.com/Player124413/Sonic-Generations-recomp-android-and-pc-edition)
by Player124413. The game's PowerPC code is translated ahead of time to C++ and compiled for
arm64, and runs on the [ReXGlue](https://github.com/rexglue/rexglue-sdk) runtime (derived from
Xenia), which provides the Xbox 360 kernel, audio, input and graphics (Vulkan on Metal through
MoltenVK). It runs at 60 FPS, with touch controls and a HedgeModManager-style mod loader.

**No game content is included. You need your own copy of the Xbox 360 game.**

## Requirements

- An iPhone or iPad on iOS 15 or later (developed and tested on an iPhone 16 Pro Max).
- *Sonic Generations* for Xbox 360: a disc image (ISO/XISO) or a folder with the disc's files
  (`Default.xex`, `disk/`, `movie/`). Developed with the World release (Redump), whose
  `default.xex` has SHA-256 `b892fc6cf18062908bde392b9845b41b67ad755dfec88c686e4c4647fcb62d76`.
- About 8 GB of free space for the game (a disc image is kept as it is and read in place).

## How to play

1. **Download** `Sonic-Generations-iOS.ipa` from the
   [latest release](https://github.com/DevZer0D4Y/SonicGenerations-iOS/releases/latest) (or build it
   yourself, see [Building](#building)).
2. **Install it** with a sideloading tool: AltStore, SideStore, Sideloadly, ESign or TrollStore.
   The IPA is unsigned; the tool signs it. Your game files and saves are kept when you re-sign it.
3. **Give it the game**, either way:
   - open the app and tap **Choose ISO** to pick a disc image (it is copied into the app), or
   - copy the extracted disc into Files → On My iPhone → Sonic Generations → `game/`.
4. Play.

Saves, settings (`sonicgenerations.toml`) and logs are in the app's Documents folder, visible in
the Files app.

## Controls

Without a controller, an on-screen gamepad covers the Xbox 360 pad: a stick with a D-pad ring,
swipe anywhere else to turn the camera, and the face, shoulder, trigger, BACK and START buttons.
**EDIT** opens a layout editor (drag to move; size, hide, opacity, reset); the layout is kept. The
gamepad hides while an MFi, Xbox or PlayStation controller is connected.

## Mods

Mods made for **HedgeModManager's Sonic Generations** profile go in Files → Sonic Generations →
`mods/<Mod name>/mod.ini`. In game, the **MODS** button (beside EDIT) opens the mod manager: import
folders, enable, reorder (the top mod wins). Mods load when the game starts.

File replacements, additions and append archives (`+Name.ar`) work. Windows code (`DLLFile`,
`CodeFile`) and PC-only data (Direct3D 9 shaders, little-endian Havok files) cannot run on the
Xbox 360 game and are skipped; the rest of such a mod still loads. Details in the
[guide](ios/sonicgenerations/README.md#mods).

## Settings

In `sonicgenerations.toml` (takes effect on the next launch):

| Setting | Values |
|---|---|
| `sg_frame_rate` | `"60"` (default, capped at 60), `"vsync"` (60, locked to the display refresh), `"original"` (30) |
| `present_letterbox` | `false` stretches to the screen, `true` keeps 16:9 with side bars |
| `touch_controls` | `"auto"`, `"on"`, `"off"`; `touch_controls_opacity` 0.1 to 1.0 |

## Building

On an Apple Silicon Mac with Xcode, CMake 3.25+, Ninja and Python 3, with your own disc:

```bash
bash ios/build.sh "path/to/Sonic Generations"   # default.xex, or the extracted disc folder
```

The IPA is written to `ios/sonicgenerations/out/Sonic-Generations-iOS.ipa`. The game's code is
generated from your `default.xex` at build time and never committed. Details, the mod loader's
design and the device scripts: [ios/sonicgenerations/README.md](ios/sonicgenerations/README.md).

## Credits

- [Sonic Generations Recompiled](https://github.com/Player124413/Sonic-Generations-recomp-android-and-pc-edition)
  by Player124413: the recompilation this port builds on.
- [ReXGlue](https://github.com/rexglue/rexglue-sdk) and [Xenia](https://github.com/xenia-project/xenia):
  the recompiler and runtime.
- The touch controls follow the Unleashed Recompiled iOS port's layout.
- [MoltenVK](https://github.com/KhronosGroup/MoltenVK), [SDL](https://github.com/libsdl-org/SDL)
  and the libraries in `ios/rexglue-sdk/thirdparty`.

*Sonic Generations* is a trademark of SEGA. This project is not affiliated with or endorsed by
SEGA, and includes none of the game's content.
