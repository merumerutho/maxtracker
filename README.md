# maxtracker

An LSDJ-style music tracker for Nintendo DS that creates and edits
.mas files for the maxmod sound system.

## Building

Requires [devkitPro](https://devkitpro.org/) with devkitARM, libnds,
libfat, and libfilesystem.

```sh
make emulator          # .nds with NitroFS (for emulators)
make native            # .nds without embedded data (for flashcarts)
```

## Running

**Emulator** (no$gba): load `release/maxtracker.nds`
directly. Song data is embedded via NitroFS.

**Flashcart** : copy `maxtracker.nds` to the SD
card. Create a `data/` folder next to it for song storage.

## Documentation

Developer guides live in [doc/](doc/):

- [DEVELOPING.md](doc/DEVELOPING.md) — build, run, test, where to start
- [architecture.md](doc/architecture.md) — code structure and IPC contract
- [hardware_quirks.md](doc/hardware_quirks.md) — NDS-specific rules
- [conventions.md](doc/conventions.md) — coding conventions

## Dependencies

| Library | Purpose |
|---------|---------|
| devkitARM + libnds | ARM cross-compiler and DS hardware abstraction |
| libfat | FAT filesystem on SD card |
| libfilesystem | NitroFS support for emulator builds |
| maxmod | Audio playback (patched fork in `lib/maxmod/`) |
| LFE | Offline sample synthesis and effects (`lib/lfe/` submodule) |

## Credits

- Johan Kotlinski for [LSDJ](https://www.littlesounddj.com/lsd/index.php) as inspiration.
- Trash80 for the [M8 tracker](https://dirtywave.com/) as inspiration.
- Mdashdotdashn and djdiskmachine for [LGPT](https://github.com/djdiskmachine/LittleGPTracker) as inspiration.
- [Maxmod](https://maxmod.org/) for the audio engine.
- [devKitPro](https://devkitpro.org/) for libnds and the other development tools.
- [blocksDS](https://blocksds.skylyrac.net/) for maintaining maxmod.
- [roseumteam](https://roseumteam.itch.io/) for "roseumteam" font.
- [int10h.org](https://int10h.org/) for the Atari Portfolio font.

Maxtracker uses a waveform editor called LFE (Lightweight Fixedpoint Engine).
LFE uses algorithms and ideas from:

- [Mutable Instruments Braids](https://mutable-instruments.net/) by
  Emilie Gillet (GPLv3) for Braids macro oscillator shapes
- [Calvario](https://github.com/HydrangeaSystems/Calvario) by
  Hydrangea Systems for Calvario XOR cross-modulation in the synth generator
- [OTT](https://xferrecords.com/freeware) by Steve Duda / Xfer
  Records for the legendary OTT multiband compressor effect
- [Audio EQ Cookbook](https://www.w3.org/2011/audio/audio-eq-cookbook.html)
  by Robert Bristow-Johnson for biquad filter coefficient formulas

## License

maxtracker is **GPL-3.0-or-later**. See [LICENSE](LICENSE) for the
full text. Bundled libraries retain their own licenses: maxmod is ISC,
LFE is GPL-3.0-or-later.
