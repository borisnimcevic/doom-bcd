# doom-bcd — DOOM on the BalCCon Cyberdeck 0o27

A bare ESP-IDF port of [doomgeneric](https://github.com/ozkl/doomgeneric) to the
BCD-0o27 badge (ESP32-S3). Shareware DOOM, ~33 fps.

- **Display**: DOOM's 320×200 frame, 2:1 box-downscaled to 160×100 and
  letterboxed on the 160×128 ST7735.
- **Input**: the 8 face buttons (74HC165 shift register) → arrows, A = fire,
  B = use, X = enter, Y = escape.
- **RAM**: DOOM's zone heap (6 MB) and all of doomgeneric's `.bss` live in the
  8 MB octal PSRAM (see `components/doomgeneric/linker.lf`).
- **WAD**: on a 12 MB SPIFFS partition, mounted at `/spiffs`, launched with
  `-iwad /spiffs/doom1.wad`.
- No sound.

## Layout

```
doom-bcd/
  CMakeLists.txt            + spiffs_create_partition_image(storage flash_data ...)
  partitions.csv            2 MB app · 12 MB SPIFFS "storage"
  sdkconfig.defaults        esp32s3, 240 MHz, octal PSRAM, custom partitions
  flash_data/doom1.wad      shareware IWAD (not in git — see below)
  components/doomgeneric/   vendored doomgeneric core (GPLv2) + linker.lf
  main/doomgeneric_bcd.c    the platform layer: display, input, timing
```

`components/doomgeneric/` is the source list from doomgeneric's `Makefile.soso`
(the no-OS target) minus the `doomgeneric_*.c` platform files. Built with `-w
-fcommon` — it's 1993 C.

## The WAD

`flash_data/doom1.wad` is id Software's freely-redistributable **shareware**
DOOM IWAD (~4.2 MB, episode 1). It is git-ignored; fetch it once:

```sh
curl -L -o flash_data/doom1.wad \
  https://github.com/Akbar30Bill/DOOM_wads/raw/master/doom1.wad
```

## Building

Needs the badge's ESP-IDF 6.0.2 with Python 3.13 on PATH (see the
`claude-anim` notes). Then:

```sh
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py -p /dev/cu.usbserial-110 -b 460800 flash monitor
```

First `flash` writes the 12 MB SPIFFS image (~70 s). After that, iterate with
`idf.py app-flash` to skip it.

## Restoring the stock badge firmware

Flash from the badge repo's `firmware/example` (PlatformIO).

## Status / TODO

- [x] boots, loads WAD, runs the demo/title loop at ~33 fps
- [ ] confirm the picture on the panel (orientation / colour)
- [ ] confirm button mapping feels right in-game
- [ ] tune: SPIFFS reads are slow — consider slurping the WAD into PSRAM
- [ ] optional: PC-speaker style audio via the buzzer / SAO
