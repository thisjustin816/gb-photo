# Camera and HiColor verification

Run these commands from the repository root on Linux or WSL. They use disposable SRAM in the emulator and write evidence under the ignored `build/tests/` directory. No cartridge or personal save is modified.

## Build

Install GCC, GNU Make, curl, tar, and Python with venv support. Build the ROM with the same `gbdk-next` distribution as the repository workflow:

```sh
mkdir -p build/tools
curl --fail --location https://github.com/gbdk-2020/gbdk-2020/releases/download/gbdk-next/gbdk-linux64.tar.gz -o build/tools/gbdk.tar.gz
tar -xzf build/tools/gbdk.tar.gz -C build/tools
python3 -m venv build/tools/venv
build/tools/venv/bin/python -m pip install Pillow PyBoy==2.8.1
make gb gbc GBDK_HOME="$(pwd)/build/tools/gbdk" PYTHON="$(pwd)/build/tools/venv/bin/python"
```

Keep the generated `.noi` files. PyBoy supplies open-source boot ROMs here; its emulator cannot load the camera's `0xFC` mapper, so the camera tests use SameBoy.

Download the unmodified [Photo! v1.12 release](https://github.com/untoxa/gb-photo/releases/tag/v1.12) and extract it into `build/tools/photo-v1.12/`, keeping its `gbc/photo.gbc` and `gbc/photo.sym` together.

The test Makefile downloads and builds SameBoy revision `26a56bd6a122a6318a456abd80558408742d5adf`. The core and harness use matching feature definitions and the core's original headers. This avoids requiring SameBoy's public-header preprocessor or a graphical frontend.

```sh
BOOT_DIR=$(find build/tools/venv/lib -path '*/pyboy/core/bootrom_cgb.bin' -print -quit)
BOOT_DIR=$(dirname "$BOOT_DIR")
make -f tests/sameboy/Makefile test \
  CGB_BOOT="$BOOT_DIR/bootrom_cgb.bin" \
  DMG_BOOT="$BOOT_DIR/bootrom_dmg.bin" \
  LEGACY_ROM=build/tools/photo-v1.12/gbc/photo.gbc \
  LEGACY_SYMBOLS=build/tools/photo-v1.12/gbc/photo.sym
```

The scoped `find` above searches only the installed test environment. Alternatively supply the two boot ROM paths directly. `SAMEBOY_DIR` can point to an already extracted copy of the pinned source; its `.source` stamp records completed extraction.

## What the harness checks

The C harness drives the production ROM with joypad input. SameBoy's camera callbacks host eight fixed RGB color bars and reply through the SRAM handshake. The host defers `GB_camera_updated()` until its update-request callback has returned.

| Scenario | Checks |
| --- | --- |
| `normal` | Preview and shutter request channel 0; one ordinary photo and valid stock checksums |
| `color` | Three correctly saved bitplanes, frozen sensor/dither registers, reply clearing, markers, deleted-marker ID collision avoidance, calibration preservation, grouped gallery, exact 128×112 color-bar screenshot, menu cache, real printer packets from green, color thumbnails, selection, default-No/canceled deletion, group deletion, ordinary overwrite clearing |
| `filters`, `bad-version` | Missing or unsupported reply discards the probe; three ordered physical-filter prompts and correctly saved RGB images |
| `cancel-red`, `cancel`, `cancel-blue` | B at each filter prompt leaves every byte of image SRAM unchanged |
| `no-space` | 28 occupied slots reject RGB capture before its first exposure; image SRAM unchanged |
| `shuffled` | RGB members remain grouped after exchanging complete physical records |
| `incomplete`, `duplicate` | Invalid groups appear as three independent grayscale images |
| `neutral` | All four equal-channel levels reproduce the selected grayscale palette exactly |
| `stress` | Deterministic noisy 64-color fixture exercises uncached quantization, color diversity, approximation error and a five-second conversion ceiling |
| `compat` | Unmodified v1.12 lists three grayscale photos; all 128 KiB of SRAM stays byte-identical while viewing them |
| `gray` | GB/DMG ordinary capture and stock checksums |

The matrix also runs `color` with SameBoy's `GB_MODEL_AGB_A`. The shared open-source CGB bootstrap is given Advance's B=1 register convention at ROM entry, and the harness asserts Photo! detects Advance. This tests the Advance compatibility model, not a physical GBA or its original boot ROM.

Every background palette-data write while HiColor is active must occur in LCD mode 0 at double speed. This includes menu transitions and animated thumbnail cursor sprites. A failure saves `hblank-failure.png`. The color-bar assertion checks every image pixel, including the tile-addressing split and first/last lines. The detector caught a real cursor-induced overrun with the slower streaming routine before the stack-fed version passed.

The `neutral` and `stress` fixtures change saved bitplanes in disposable emulator SRAM after the real RGB capture has passed its channel/checksum checks. They isolate rendering from the camera sensor's threshold response.

Individual runs use:

```sh
build/tests/color_camera ROM SYMBOLS SAVE BOOT_ROM SCENARIO [cgb|agb|dmg]
```

## Evidence and limits

The October 16, 2026 run used `gbdk-next` lcc dated July 16, 2026 and SDCC 4.5.1 #15267. GB and CGB builds completed without new compiler warnings. The ROMs remain 128 KiB, preserving the existing flash-roll layout. The unmodified GB baseline passed the same ordinary capture checks as the changed GB ROM. The handshake detector rejected the unmodified CGB baseline as expected.

The harness rejects a ROM larger or smaller than 128 KiB before boot. The two handshake routines reside in ROM bank 0 to leave enough space in the switchable banks. The following test fixtures used branch labels `master/gb` and `master/gbc`, with commit label `582e284`. Those labels are embedded in the ROM, so builds from another branch or commit have different hashes:

| ROM | SHA-256 |
| --- | --- |
| `build/gbc/photo.gbc` | `5c4d42d43320c67af7114df4af0debe102d1764d45e94cf32dfa3caaffd98ebc` |
| `build/gb/photo.gb` | `0f1c1d61ab12c73cc8d0a2b905b9afe4a09c126d79916946bb898dcc2ab3783b` |

Full-size conversion, including entry/fade, took 210 frames (about 3.5 seconds) for color bars and 275 frames (about 4.6 seconds) for the noisy fixture. The noisy view retained 62 colors with mean squared level error 2.53 per pixel, summed across the three 0-3 components. These measurements apply to the two fixtures. Cached menu redisplay avoids this conversion.

Review `color.png`, `stress.png`, `neutral.png`, `thumbnails.png`, `selected-thumbnail.png`, `gallery-menu.png`, `delete-group.png`, `delete-selected.png`, `filter-1.png` through `filter-3.png`, and `legacy-1.png` through `legacy-3.png`. Logs are named by scenario.

SameBoy emits PPU/APU "odd mode" diagnostics when restoring single speed with the LCD/audio enabled; its core marks those speed-switch timings as incompletely modeled. The active HiColor palette writes still pass the mode-0 assertions. Physical console timing, sound, and real optical filters remain unverified. Other console ports and the remote ROM are outside this change's validation scope.
