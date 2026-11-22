# Install on Move

Use a Move running Schwung with its v2 plug-in interface, file browser and screen-reader libraries. The paired download is a manual installation archive. Open it on your computer. In an SFTP client, connect to the same Move and account you use for Schwung file transfers. [Schwung setup](https://github.com/charlesvestal/schwung)

## Copy the modules

1. Save your set. Exit either player with **Shift + Back** and unload these generators before updating. **Back** alone can leave a player running.
2. If a previous version is installed, download a backup of its module folders and any custom ROM-location file. The original folder names below are intentional and allow existing sets to find the renamed instruments.
3. Open the archive's `modules` folder. Merge its `sound_generators` and `tools` folders into the following device directory. When updating, copy files **into** the existing folders. Choose your SFTP client's merge option. Preserve existing ROMs, runtime data and any customized `roms.txt`. If installing only one instrument, copy its generator and matching player together.

```text
/data/UserData/schwung/modules/
  sound_generators/mu2000/
  sound_generators/sound-canvas-bridge/
  tools/mu2000-midi-player/
  tools/sound-canvas-midi-player/
```

4. Add the files for your chosen instrument as described below. Keep the files accessible to the account running Schwung on Move (normally `ableton`).
5. Restart Move. Test one note with the generator, then exit or unload it and try the matching player. Put a MIDI file anywhere under the following device folder and select it with **Open MIDI File**.

```text
/data/UserData/UserLibrary/
```

## Muve2000 sound data

Copy your own MU2000 program and wave ROMs into the generator folder, retaining filenames, lowercase letters and the `dump` subfolder. This is the layout tested on Move. If you already have a working S-MU2000 folder, keep its other support files with it. The upstream project documents how owners can prepare their data. [MU2000 owner’s extraction guide](https://github.com/tarboh/S-MU2000/blob/main/doc/dump/README.en.md)

```text
/data/UserData/schwung/modules/sound_generators/mu2000/roms/
  mu2000_flash.bin
  dump/
    xv364a0.ic49
    xv365a0.ic50
    xw848a0.ic53
    xw849a0.ic54
```

The program file is 4 MiB and each wave file is 8 MiB. Both modules read the same data. The player's supplied `roms.txt` contains the absolute directory path shown above. If the generator works but the player reports missing ROMs, check this file in `tools/mu2000-midi-player/`. It should contain a single path to the ROM directory as seen **on Move**.

## Move Canvas engine files

Put these four files together in the generator's `runtime` directory. Both Move Canvas modules use it.

- **SCCore.dll** is the **32-bit Windows** engine from your own compatible Sound Canvas VA installation. Sound Canvas Bridge for Windows uses a **64-bit** DLL of the same name. The Move requires the 32-bit build. The SHA-256 of the engine tested on hardware is shown below.
- **msvcr100.dll** is the accompanying **32-bit** Microsoft Visual C++ 2010 runtime. Use the x86 copy from your licensed installation or Microsoft runtime.
- **boxedwine.zip** and **apisetschema.dll** are the compatibility files. A verified source for both is the **SoundCanvasBridge-1.4.5-source.zip** asset. Open its `Dependencies/MacRuntime/` directory and copy the two files. Leave `boxedwine.zip` compressed. You can do this on Windows, macOS or Linux. [Sound Canvas Bridge 1.4.5 release assets](https://github.com/OnjLouis/sound-canvas-bridge/releases/tag/v1.4.5)

```text
/data/UserData/schwung/modules/sound_generators/sound-canvas-bridge/runtime/
  SCCore.dll
  msvcr100.dll
  apisetschema.dll
  boxedwine.zip
  CanvasBoxedWine
  SoundCanvasGuest.exe
  start-engine.sh
```

The last three files are supplied in the Move download. If your SFTP client drops Unix permissions, use its file properties to give the **owner** read, write and execute access to the runtime directory, `CanvasBoxedWine` and `start-engine.sh` (mode 0700). The remaining files need owner read access. The account running Schwung must own the private runtime directory.

If you transferred the runtime as root, run these commands in your Move SSH session after copying all seven files. They set ownership and permissions for this runtime only.

```sh
cd /data/UserData/schwung/modules/sound_generators/sound-canvas-bridge/runtime || exit 1
chown ableton:users . CanvasBoxedWine SoundCanvasGuest.exe start-engine.sh SCCore.dll msvcr100.dll boxedwine.zip apisetschema.dll
chmod 700 . CanvasBoxedWine start-engine.sh
chmod 600 SoundCanvasGuest.exe SCCore.dll msvcr100.dll boxedwine.zip apisetschema.dll
```

The tested 32-bit SCCore.dll has this SHA-256.

```text
8ddb29d5e1c20ba5a262084e96fbc4f4de8c70db988873b80a1a6bd3ee244f29
```

## If something goes wrong

- **Neither module appears.** Check that `module.json` and `dsp.so` sit directly inside each named module folder. An extra `modules/modules/` or archive-name folder prevents discovery. Restart Move after correcting the layout.
- **Move Canvas cannot start or reports an unsupported engine.** Check the DLL architecture, the four exact filenames, the executable permission on the worker, and the directory owner. Compare the engine hash above. Close and reopen the module after a correction.
- **Move Canvas asks you to close another instance.** The generator and its player share one engine slot. Unload the generator from its chain or fully exit the player with **Shift + Back**, then reopen the instance you want.
- **A MIDI file opens but you hear silence.** Try a note in the generator to verify the sound data. In the player, check render progress with **Shift + Click**. In Live mode, check the part's mute, solo and level, and the player's master level where available.
- **Undo an update.** Exit the players and unload the generators. Restore the module folders and custom paths you backed up, then restart. For removal, first back up your sound data and remove only the module folders you installed. Your MIDI files live separately in the User Library.
