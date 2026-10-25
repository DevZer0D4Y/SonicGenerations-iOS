# Sonic Generations for iPhone and iPad: detailed guide

The iOS app around [Sonic Generations Recompiled](https://github.com/Player124413/Sonic-Generations-recomp-android-and-pc-edition)
(Player124413's ReXGlue edition for PC and Android, made with rexauto), the static recompilation of
*Sonic Generations* (Xbox 360, 2011). The game's PowerPC code is translated ahead of time to C++
and compiled for arm64; the iOS port of the [ReXGlue](https://github.com/rexglue/rexglue-sdk)
runtime in [`../rexglue-sdk`](../rexglue-sdk) (derived from Xenia) supplies the Xbox 360 kernel,
audio and input, and the graphics run through Vulkan on Metal (MoltenVK).

This guide covers what the app does on iOS, mods, building by hand, and the device tools.

## What the app does on iOS

- **Paths.** iOS starts the app with no arguments and its bundle is read-only, so everything lives
  in the app's Documents folder, which the Files app shows: `game/` (the extracted disc:
  `Default.xex`, `disk/`, `movie/`) or `game.iso`, `mods/`, `userdata/`, `cache/` and
  `sonicgenerations.toml` (`src/sonicgenerations_app.h`).
- **Disc images.** With no game data, the app offers **Choose ISO** (`src/iso_import.mm`): the
  chosen ISO/XISO is copied into Documents as `game.iso`, checked for `default.xex`, and read in
  place from then on.
- **Mods.** The **MODS** button of the touch controls opens a HedgeModManager-style mod manager;
  see below.
- **Touch controls.** The on-screen gamepad of the Unleashed Recompiled iOS port (XeniOS layout):
  a stick with a D-pad ring, swipe anywhere else to turn the camera, and pill buttons. **EDIT**
  opens a layout editor (drag to move; SIZE, HIDE, ALPHA, RESET, DONE); the layout is kept. The
  gamepad hides while a controller is connected (`touch_controls`: `auto`, `on`, `off`).
- **60 FPS.** The game presents every second vblank (30 FPS) on the console. A hook on that
  instruction (`0x82DC44E0`, `src/frame_rate.cpp`) sets the presentation interval. By default
  it presents as soon as a frame is ready and the runtime caps it at 60 (`framerate_limit`), so a
  frame that runs a little long costs only its own time instead of a whole extra vblank.
  `sg_frame_rate` in `sonicgenerations.toml`: `"60"` (default), `"vsync"` (every vblank, as the
  Xenia "60 FPS" game patch does) or `"original"` (30).
- **Busy waits.** A warm iPhone parks its performance cores and runs everything on the efficiency
  cores. Two loops that spin on the console then take a core from the threads they wait for: Havok
  waiting for its physics jobs, and D3D's wait for the GPU. Hooks in `src/spin_waits.cpp` make
  them yield or sleep briefly instead, which also keeps the phone cooler.
- **Memory fences.** The Xbox 360's `sync`, `lwsync` and `eieio` became nothing in the
  recompiled code, which only holds on x86. On the iPhone's ARM CPU they are real barriers now:
  without them other threads could see data out of order, and Sonic fell through the ground or
  out of the 2D path.
- **Orientation.** The app is landscape only, and SDL is told so: iOS lays apps out in portrait
  for screenshots and the app switcher, and the window used to stay portrait afterwards, squeezing
  the picture into part of the screen.
- **Settings for an iPhone GPU.** The defaults in `SetDefaults()` come from the Simpsons, NFSMW,
  MC360 and Superstars Tennis ports of this runtime: host render targets, tight render areas,
  direct uploads into shared memory, single-sampled MSAA views, MoltenVK encoding command buffers
  on its own thread, and FIFO presentation capped at 60 FPS.
- **Unseen function pointers.** Like upstream's PC build, a call through a pointer to code the
  recompiler did not find returns instead of aborting (`tolerant_indirect_calls`); each target is
  logged once.
- **Background.** Switching apps or locking the phone pauses audio, the guest clock and the GPU;
  the session resumes where it was.
- **Language.** The console language defaults to the device's when the disc has it (English,
  Japanese, French, German, Spanish, Italian).

## Mods

The app loads mods made for **HedgeModManager's Sonic Generations** profile, the same folders PC
players install. It does not run HedgeModManager itself.

### Where mods go

Each mod is a folder with a `mod.ini` inside, in the app's `mods` folder:

```
Files → On My iPhone → Sonic Generations → mods/
    Some Mod/
        mod.ini
        disk/bb/...        (files replaced or added in bb.cpk; likewise bb2, bb3)
    Another Mod/
        mod.ini
        ...
```

That is `Documents/mods/` inside the app's container. Which mods are on, and their order, is
kept in `Documents/ios-mods.ini`.

### Using the mod manager

1. Extract the mod's archive (Files can open `.zip`) and find the folder that holds `mod.ini`.
   Mods whose archive has an extra folder around it need that one opened first.
2. In game, tap **MODS** (beside **EDIT**, top left). The game pauses. Tap **Import folder** and
   pick the mod folder; the app copies it into `mods/`. Or copy the folder into
   Files → Sonic Generations → `mods` yourself.
3. Tap a mod to enable it, drag to set the order. **The top mod wins** when two mods change the
   same file. Dependencies declared in `mod.ini` are checked (ID and minimum version).
4. Tap **Done**. Mods load when the game starts, so if the selection changed the app offers
   **Quit Game**: open it again and the new selection is in. **Keep Playing** applies it on the
   next launch instead.

### How it works

The Xbox 360 game reads its data from three CRI archives, `disk/bb.cpk`, `bb2.cpk` and
`bb3.cpk`. A mod's `disk/bb`, `disk/bb2` and `disk/bb3` folders mirror the inside of those
archives, as on PC. Instead of extracting or rewriting the 4 GB of archives, the app shows the game
a *virtual* CPK in place of each one it changes (`src/mods/cpk_overlay.cpp`):

- the CPK's table of contents is rebuilt with the replaced files' new sizes and positions, plus
  the files a mod adds, sorted the way CRI's own packer sorts it (the game's file system
  binary-searches it);
- the original archive's data is served unchanged, moved back by whole 2048-byte sectors if the
  table grew, and each mod file is read straight from the mod folder after it.

With no mods enabled nothing is changed at all. With mods, the runtime serves the virtual file
through a small VFS hook in the SDK (`include/rex/filesystem/file_overlay.h`).

**Append archives** (`+Name.ar.00` / `+Name.arl`, HedgeModManager's way of adding files to a game
archive without shipping the whole archive) are merged into the archive group they name
(`src/mods/hedge_archive.cpp`): the original group is unpacked from the CPK (CRILAYLA,
`src/mods/crilayla.cpp`), the mod's entries replace or join it, and the result is written to
`cache/mods/` in splits of up to 10 MB like the game's own. The merge is redone only when an input
changes.

### What works and what does not

- Works: file replacements and additions under `disk/bb`, `disk/bb2`, `disk/bb3` (any subfolder),
  append archives, several include directories, load order, enable/disable, dependency checks.
- **DLLs and code files are skipped** (`DLLFile`, `CodeFile` in `mod.ini`): they are Windows PC
  code that patches the PC executable. Such a mod is loaded **data only**: its files load and the
  manager says what was left out. Whatever the code did is missing: new moves and the mod's
  configuration options (the Classic Sonic Improvement Mod's Drop Dash and Super Peel Out, for
  example), and mods that are code alone (Generations Plus, the Better FxPipeline renderer) do
  nothing. Data written to go with that code can misbehave without it.
- **PC-only data is skipped.** Some files are built per platform, and mods made for the PC
  version ship the PC builds: shaders (Direct3D 9 bytecode, in `shader_r.ar` and the like or as
  loose `.wpu`/`.wvu`) and Havok files (`.hkx` skeletons, animations and collision, which are
  little-endian on PC and big-endian on the Xbox 360). The console game crashes at boot on the
  shaders and hangs on the Havok files, which it loads in place and patches as its own. So the
  loader drops every archive group (or loose file) holding either, logs it ("Skipped bb3/
  SONICCLASSIC.AR: it holds PC Havok files…") and loads the rest. Converting them would need
  Havok's own tools. Checking reads only each archive's entry headers, and the answers are cached
  in `cache/mods/pc-data-scan.txt`, so only new or changed archives are read on a later start.
  The Classic Sonic Improvement Mod is mostly such archives (the Marza Planet model and its
  animations, cutscenes, object animations, some collision): what is left is its sounds and
  parameter tweaks.
- A mod's options (`ConfigSchemaFile`) cannot be changed here; the include folders it ships with
  selected in `mod.ini` are the ones that load (edit `IncludeDir0`… in Files to pick others).
- `SaveFile` redirection is ignored: saves stay in the console profile.
- The `work` folder (a development shortcut on PC) is ignored.
- PC assets are not always Xbox 360 assets. Models, textures, animations, sounds, text and stage
  data are the same formats, but the PC version has its own shader set: a mod that ships new
  shaders, or depends on the PC renderer, can render wrongly or not load.

### Checking the loader

`bash tests/run-mod-tests.sh "<disc folder>/disk"` runs the loader against your own archives on a
Mac (AddressSanitizer + UBSan): every CRI table re-serializes to the same bytes, a CPK with no
mods reads back identical to the original, replaced and added files are served while all others
stay byte-identical, append archives merge, and the database round-trips.

## Building

`bash ../build.sh <default.xex or the extracted disc folder>` does everything below. By hand, on a
Mac with Xcode, CMake 3.25+, Ninja and Python 3, from this folder:

```bash
# 1. The SDK: the code generator for the Mac, and the runtime for iOS.
(cd ../rexglue-sdk &&
 cmake --preset mac-arm64 -DREXGLUE_REGISTER_PACKAGE=OFF -DREXGLUE_ENABLE_TRACY=OFF &&
 cmake --build out/build/mac-arm64 --config Release --target rexglue --parallel 2 &&
 cmake --preset ios-arm64 -DREXGLUE_REGISTER_PACKAGE=OFF &&
 cmake --build out/build/ios-arm64 --config Release --parallel 2 &&
 cmake --install out/build/ios-arm64 --config Release)

# 2. The game code, from your own default.xex in ../game_xex/ (about 360 MB of C++).
export REXGLUE_HOST_TOOL="$PWD/../rexglue-sdk/out/mac-arm64/Release/rexglue"
export REXGLUE_IOS_SDK="$PWD/../rexglue-sdk/out/install/ios-arm64"
"$REXGLUE_HOST_TOOL" codegen sonicgenerations_manifest.toml

# 3. The app and its IPA (out/Sonic-Generations-iOS.ipa).
cmake --preset ios-arm64-release
cmake --build --preset ios-arm64-release --parallel 2
bash ios/make-ipa.sh
```

The code generator finds the same 104,236 functions as upstream's. Upstream's SDK patch adds a
scan that treats every word in the data sections that points into code as a function; here it is
the off-by-default `data_pointer_scan` codegen option, because absolute jump tables look the same
and their case labels split real functions (12 unresolved branches otherwise).

The `mac-arm64-release` preset builds the same app for macOS (set `REXGLUE_MAC_SDK` to a macOS
install of the runtime), to test the port's code paths on a Mac with `--game_data_root=<disc
folder>`; mods there come from the user data folder.

## Device tools

All take `DEVICE` (from `xcrun devicectl list devices`, default: the first connected device).

| Script | |
|---|---|
| `ios/send-to-esign.sh` | Builds the IPA and copies it into ESign's folder on the phone. |
| `ios/push-game-data.sh <disc folder>` | Copies an extracted disc into the app's `game/` folder. |
| `ios/sign-and-install.sh` | Signs the app with your development profile and installs it. |

### Performance counters

`REX_GPU_PROFILE=1` adds the runtime's performance counters and sampling profiler to the log in
`userdata/logs`.
