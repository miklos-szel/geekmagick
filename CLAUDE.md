# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this workspace is

Two things live here:

1. **`mkv2geekgif.sh`** — converts videos (mkv/mp4/…) into GIFs that actually play on a
   GeekMagic cube (HelloCubic Lite / SmallTV-Ultra, ESP8266 + ST7789 240×240). The source
   videos and reference GIFs in the root are test assets.
2. **`GeekMagic-Open-Firmware/`** — clone of the open firmware (PlatformIO, GPL-3.0) that
   runs on the device. It is its own git repo; the outer directory is not.

## Device GIF constraints (hard-won; violating these "freezes" or corrupts the screen)

Derived from `GeekMagic-Open-Firmware/src/display/Gif.cpp` — read it before changing the
GIF pipeline:

- **No per-file time limit**: the firmware has no hard playback cutoff. Looping GIFs
  (`playGifFullScreen(path, 0)`) run indefinitely.
- **No transparency, no background/previous disposal**: the transparency draw path erases
  the previous frame's rectangle to black → sparse "fuzzy" screen. Use partial
  bounding-box frames *without* transparent pixels (like ezgif.com output).
- Tooling consequence: ffmpeg needs `-gifflags -transdiff` (keep default offsetting);
  gifsicle must be `-O1` (never `-O2`/`-O3`, those add transparency). `--lossy` is safe.
- ~10 fps plays smoothly; storage free space is ~1.5 MB, so keep files under that.

`mkv2geekgif.sh input.mp4 [max_bytes] [start-end]` encodes all of this: 240×240
force-scale, 10 fps, ≤24-color palette, no dither, auto size-fit ladder
(raise `--lossy`, then shrink palette — never lowers fps, which would balloon the frame
diffs), and prints firmware-compat warnings. Output is `input_N.gif` (auto-increment,
never overwrites). Requires `ffmpeg`, `gifsicle`, ImageMagick `identify`.

Verify a GIF's device-compatibility with `gifsicle --info file.gif`: frame #0 full-canvas,
all others partial rects, zero `transparent`, disposal `asis` only, `loop forever`.

## Firmware (GeekMagic-Open-Firmware/)

All commands run from inside `GeekMagic-Open-Firmware/`.

```bash
pio run                          # build firmware (env: esp12e)
pio run --target buildfs         # build LittleFS image from data/
pio run --target upload          # flash over serial (or use scripts/build-with-docker.sh)
pio check --fail-on-defect high  # clang-tidy static analysis — this is the lint gate
                                 # (same command runs in CI and the pre-commit hook)
python3 test/webServerTest.py    # emulates the device API on localhost:8080 to develop
                                 # the web UI (data/web/) without hardware
./scripts/openapi.sh             # regenerate swagger.yml from Api.cpp annotations
./scripts/install-githooks.sh    # enable the pio-check pre-commit hook
```

CI (`.github/workflows/ci.yml`) runs ShellCheck on `scripts/`, `pio check`, then the
build. clang-tidy enforces `modernize-*`/`readability-*` including trailing return types
(`auto f() -> bool`) — match that style in new code.

### Architecture

Cooperative single-loop design (no RTOS): `main.cpp` `loop()` pumps, in order, RescueMode
→ Webserver → NTPClient → `DisplayManager::update()`. Long operations must call `yield()`
or the ESP8266 watchdog resets.

- **DisplayManager** (`src/display/`) — static facade over Arduino_GFX (ST7789, hardware
  SPI 40 MHz, batched `startWrite()`/`endWrite()`). Owns the screen; everything draws
  through it.
- **Gif** (`src/display/Gif.cpp`) — wraps bitbank2/AnimatedGIF, streams frames line-by-line
  from LittleFS straight to the panel (no framebuffer — only a single line buffer).
  Singleton via `s_instance` because AnimatedGIF takes C callbacks.
- **Webserver/Api** (`src/web/`) — REST under `/api/v1/...` (wifi, ntp, display, ota, gif
  upload/play/stop/list). Routes registered in `Api.cpp`; static UI served from
  `data/web/` (LittleFS). GIFs upload to `/gifs/`. Token-protected; see SecureStorage.
- **ConfigManager/SecureStorage** (`src/config/`) — JSON config on LittleFS
  (`data/config.json`, examples for cube vs smallTV variants).
- **RescueMode** (`src/boot/`) — boot-loop protection; if boot doesn't survive
  `BOOT_STABLE_MS` it takes over `loop()` entirely for recovery/OTA.

Flash layout is 4 MB with 2 MB LittleFS (`eagle.flash.4m2m.ld`). `data/` is the LittleFS
image source — web UI, default config, bundled GIFs.

Device peculiarity: display CS is tied to GND (always selected) and the backlight is
active-LOW on GPIO 5; the cube's panel is mounted upside-down (rotation handled in
firmware). Full hardware notes are in `GeekMagic-Open-Firmware/readme.md`.
