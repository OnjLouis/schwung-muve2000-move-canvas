# Muve2000 and Move Canvas for Ableton Move

Play Yamaha MU2000 or Roland Sound Canvas arrangements on Move. Each instrument has a **sound generator** for pads, an external keyboard or a Schwung chain, and a **MIDI Player** for playing a complete arrangement from the User Library. You can set up either instrument independently.

## Download and install

Download the paired **Muve2000-and-Move-Canvas-0.1.0.tar.gz** archive. It contains all four ready-built ARM64 modules. Bundle version 0.1.0 includes Muve2000 generator 0.1.4, Muve2000 MIDI Player 0.1.10, and Move Canvas generator and player 0.1.1. [Download the first release](https://github.com/OnjLouis/schwung-muve2000-move-canvas/releases/tag/v0.1.0)

1. Install Schwung on Move and set up SFTP access. If you already run Schwung instruments, use your existing connection. [Schwung setup and file transfer](https://github.com/charlesvestal/schwung#installation)
2. Follow the installation guide to merge the modules into the device and add your own sound data. Muve2000 reads a MU2000 firmware and wave-ROM set. Move Canvas loads a supported **32-bit** Sound Canvas VA engine and its compatibility files. [Install, update or remove the instruments](docs/Installation.md)
3. After restarting Move, choose a generator in **Sound Generators** or open a player under **Tools**. For a first listen, open a player, choose **Open MIDI File**, select a file in your User Library, and press **Play**. The default **Render First** mode prepares the song before you hear it.

## Playing guides

- **Muve2000** reads instrument and kit names from your ROMs. Its MIDI Player follows file tempo and supports ports A through D. Learn how to audition sounds, mix parts and choose between immediate keyboard response and smooth playback of dense files. [Muve2000 controls and playback](docs/Muve2000.md)
- **Move Canvas** has 2,958 bank entries across the SC-55, SC-88, SC-88 Pro and SC-8820 maps, plus 93 editable controls. Learn how to play drums from any keyboard channel, edit effects and play multiport arrangements. [Move Canvas controls and playback](docs/Move-Canvas.md)

These are unofficial community instruments. They use S-MU2000 and Sound Canvas Bridge technology. Engine data is supplied separately by each user. Licenses for the distributed components are in the Licenses directory. [S-MU2000](https://github.com/tarboh/S-MU2000) · [Sound Canvas Bridge](https://github.com/OnjLouis/sound-canvas-bridge) · [Build notes](BUILDING.md)
