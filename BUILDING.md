# Building the Move modules

The release archive includes runnable ARM64 modules. Building from source requires a C++20 Linux AArch64 toolchain and Schwung's v2 ABI header, named `plugin_api_v1.h` in `src/host`. Target the Move's Cortex-A72 and glibc 2.34. [Schwung host sources](https://github.com/charlesvestal/schwung/tree/main/src/host)

## Muve2000

Use a clean working copy of the S-MU2000 fork at the revision shown below. For the player, apply `Swp30Move.patch` and then `MoveMidiOnly.patch` from `Muve2000/Schwung` to the engine tree. The patches specialize MIDI-only rendering for Move. [Pinned S-MU2000 engine](https://github.com/OnjLouis/mu2026-smu-engine/tree/5ed88bf2afd9c4aae83b3cc123c26be6691e40b9)

```text
5ed88bf2afd9c4aae83b3cc123c26be6691e40b9
```

Configure CMake with source directory `Muve2000/Schwung`, an out-of-tree build directory, `SMU_SOURCE` set to the engine checkout and `SCHWUNG_INCLUDE` to the host header directory. The supplied Windows-hosted Zig toolchain file takes `ZIG` and `LLVM_BIN` paths. Use Release, `MU2000_THREADED_ENGINE=ON` and `MU2000_ABI_PROBE=OFF` for the player. The tested player used Zig 0.15.2, ThinLTO, `-fno-semantic-interposition` and `-ffp-contract=off`. Build target `mu2000-midi-player`. The standalone generator target is `mu2000`; the first bundle carries its previously tested DSP. Keep each target's `dsp.so` alongside its own manifest, UI and help files.

## Move Canvas

- **Generator and player DSP.** Configure CMake in `MoveCanvas/Schwung` with your ARM64 toolchain and `SCHWUNG_SDK_INCLUDE` pointing to the host header directory. Build target `CanvasSchwung`. Run the following command using the resulting shared object to create both modules. They share DSP code and use separate Schwung identities.

```sh
python3 MoveCanvas/Schwung/stage.py build/modules build/dsp.so
```

- **Linux compatibility worker.** The release archive's `Source/Move-Canvas-Worker/` includes the corresponding BoxedWine sources, overrides and CMake files. Extract the nested source archive, overlay the supplied `Overrides` directory and follow its `Build.md`. The upstream revision is `9ca68a48ed92a6bc1cc02d24bf27399aa2a59760`. [BoxedWine source](https://github.com/danoon2/Boxedwine/tree/9ca68a48ed92a6bc1cc02d24bf27399aa2a59760)
- **Windows guest.** Build `MoveCanvas/Source/Translated/Guest.cpp` for Win32 i686 with MinGW, the `MoveCanvas/Source` include directory, C++17, `-O2 -static -municode`, and the `SCBRIDGE_VERIFIED_TRANSLATED_CORE` and `SCBRIDGE_MOVE_GUEST` definitions. Place the result as `SoundCanvasGuest.exe` alongside `CanvasBoxedWine` and `start-engine.sh` in the generator's runtime directory.

The sound data stays on the user's device. To test a build with your own data, follow the hardware installation guide. [Installation and first note](docs/Installation.md)
