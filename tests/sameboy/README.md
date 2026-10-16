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
make gbc GBDK_HOME="$(pwd)/build/tools/gbdk" PYTHON="$(pwd)/build/tools/venv/bin/python"
```

Keep the generated `.noi` files. PyBoy supplies open-source boot ROMs here; its emulator cannot load the camera's `0xFC` mapper, so the camera tests use SameBoy.

Download the unmodified [Photo! v1.12 release](https://github.com/untoxa/gb-photo/releases/tag/v1.12) and extract it into `build/tools/photo-v1.12/`, keeping its `gbc/photo.gbc` and `gbc/photo.sym` together.

The test Makefile downloads and builds SameBoy revision `26a56bd6a122a6318a456abd80558408742d5adf`. The core and harness use matching feature definitions and the core's original headers. This avoids requiring SameBoy's public-header preprocessor or a graphical frontend.

```sh
BOOT_DIR=$(find build/tools/venv/lib -path '*/pyboy/core/bootrom_cgb.bin' -print -quit)
BOOT_DIR=$(dirname "$BOOT_DIR")
make -f tests/sameboy/Makefile test \
  CGB_BOOT="$BOOT_DIR/bootrom_cgb.bin" \
  LEGACY_ROM=build/tools/photo-v1.12/gbc/photo.gbc \
  LEGACY_SYMBOLS=build/tools/photo-v1.12/gbc/photo.sym
```

The scoped `find` above searches only the installed test environment. Alternatively supply the CGB boot ROM path directly. `SAMEBOY_DIR` can point to an already extracted copy of the pinned source; its `.source` stamp records completed extraction.

## What the harness checks

The C harness drives the production ROM with joypad input. SameBoy's camera callbacks host eight fixed RGB color bars and reply through the SRAM handshake. The host defers `GB_camera_updated()` until its update-request callback has returned.

| Scenario | Checks |
| --- | --- |
| `transitions` | Consecutive frames across the title, hub, About, Settings, camera, gallery, and flash storage show complete screens, their fades, or black; the animated hand and live-preview area are excluded |
| `font-boundary` | Both ROM glyph shifters are relocated in disposable emulator memory to force each of their eight offsets onto a page boundary; all rows, stack cleanup, and ROM-bank restoration are checked |
| `ui` | Every title pixel, including sprite overlays, matches the original artist's colors at CGB precision; menu artwork matches its compiled reference; title restoration, About, camera and popup transitions; UI attributes leave the live image untouched |
| `fresh`, `fresh-zero` | FF-filled and zero-filled SRAM initialize an empty roll without corruption; Color, double speed, DMA, fast printing, and IR shutter default to On; CPU speed is applied at boot; ordinary capture, continuing preview, gallery input, and save persistence after a power cycle |
| `fresh-color` | A new installation takes an RGB photo with Color On by default; preview resumes and the photo survives a power cycle without repairs |
| `fresh-sensor` | Nonblank live sensor pixels do not turn fresh SRAM into a repaired, full roll; ordinary capture and save persistence still work |
| `normal` | Preview and shutter request channel 0; one ordinary photo, valid stock checksums, continuing preview, gallery input, and save persistence after a power cycle |
| `color` | Three correctly saved bitplanes, frozen sensor/dither registers, reply clearing, markers, deleted-marker ID collision avoidance, calibration preservation, grouped gallery, exact 128×112 color-bar screenshot, menu cache, real printer packets from green, color thumbnails, selection, default-No/canceled deletion, group deletion, ordinary overwrite clearing |
| `filters`, `bad-version` | Missing or unsupported reply discards the probe; three ordered physical-filter prompts and correctly saved RGB images |
| `cancel-red`, `cancel`, `cancel-blue` | B at each filter prompt leaves every byte of image SRAM unchanged |
| `no-space` | 28 occupied slots reject RGB capture before its first exposure; image SRAM unchanged |
| `shuffled` | RGB members remain grouped after exchanging complete physical records |
| `incomplete`, `duplicate` | Invalid groups appear as three independent grayscale images |
| `neutral` | All four equal-channel levels reproduce the selected grayscale palette exactly |
| `stress` | Deterministic noisy 64-color fixture exercises uncached quantization, color diversity, approximation error and a five-second conversion ceiling |
| `compat` | Unmodified v1.12 lists three grayscale photos; all 128 KiB of SRAM stays byte-identical while viewing them |

The matrix also runs `color`, `fresh`, `ui`, and `transitions` with SameBoy's `GB_MODEL_AGB_A`. The shared open-source CGB bootstrap is given Advance's B=1 register convention at ROM entry, and the harness asserts Photo! DX detects Advance. This tests the Advance compatibility model, not a physical GBA or its original boot ROM. Photo! DX releases one versioned `.gbc` ROM for GBC/GBA; no GB ROM or starter save is packaged.

Every background palette-data write while HiColor is active must occur in LCD mode 0 at double speed. This includes menu transitions and animated thumbnail cursor sprites. A failure saves `hblank-failure.png`. The color-bar assertion checks every image pixel, including the tile-addressing split and first/last lines. The detector caught a real cursor-induced overrun with the slower streaming routine before the stack-fed version passed.

The `neutral` and `stress` fixtures change saved bitplanes in disposable emulator SRAM after the real RGB capture has passed its channel/checksum checks. They isolate rendering from the camera sensor's threshold response.

Individual runs use:

```sh
build/tests/color_camera ROM SYMBOLS SAVE BOOT_ROM SCENARIO [cgb|agb]
```

## Evidence and limits

The October 16, 2026 checks used `gbdk-next` lcc dated July 16, 2026 and SDCC 4.5.1 #15267. The GBC ROM remains 128 KiB, preserving the existing flash-roll layout. The handshake detector rejected the unmodified CGB baseline as expected. The fresh-SRAM regression failed against the first Photo! DX build, whose repair path undeleted all 30 slots. The corrected build passes FF-filled and zero-filled SRAM initialization, ordinary and default-On RGB capture, preview responsiveness, and save persistence after a power cycle. These tests use disposable SRAM; the upstream `assets/photo.sav` contains a full roll and is not a starter save.

The harness rejects a ROM larger or smaller than 128 KiB before boot. The two handshake routines reside in ROM bank 0 to leave enough space in the switchable banks. The GBC/GBA-only build omits Super Game Boy border assets. The release workflow prints the ROM's MD5 and checks that its tag matches the in-ROM version. The ZIP contains `Photo! DX [v1.13-dx.2].gbc` for version 1.13-dx.2; the direct download contains identical bytes under GitHub's normalized filename.

Full-size conversion, including entry/fade, took 210 frames (about 3.5 seconds) for color bars and 275 frames (about 4.6 seconds) for the noisy fixture. The noisy view retained 62 colors with mean squared level error 2.53 per pixel, summed across the three 0-3 components. These measurements apply to the two fixtures. Cached menu redisplay avoids this conversion.

Review `color.png`, `stress.png`, `neutral.png`, `thumbnails.png`, `selected-thumbnail.png`, `gallery-menu.png`, `delete-group.png`, `delete-selected.png`, `filter-1.png` through `filter-3.png`, and `legacy-1.png` through `legacy-3.png`. Logs are named by scenario.

SameBoy emits PPU/APU "odd mode" diagnostics when restoring single speed with the LCD/audio enabled; its core marks those speed-switch timings as incompletely modeled. The active HiColor palette writes still pass the mode-0 assertions. Physical console timing, sound, and real optical filters remain unverified. Other console ports and the remote ROM are outside this change's validation scope.

The title uses seven background palettes and 29 sprite overlays, with at most seven sprites on a scanline. This preserves the artist's pixels without four-color tile bleed. The hub needs no overlays. The UI regression rejects the unchanged ROM and passes on both CGB and AGB. Generated references live beside the object files; the test compares the rendered ROM against them. The reference artwork itself was inspected against the original color sheet, including the lens and Game Boy overlap.

The ROM also passed a direct integration run through Press Any's camera bridge at commit `53741bb`, with its pinned SameBoy core. Fresh SRAM initialized without repairs, all saved channel pixels matched the RGB source bars, and the group opened in the HiColor gallery. That run exposed the live sensor window at bank 0, `0xA100..0xAEFF`: it must be excluded when testing whether persistent SRAM is blank. The corresponding regression fails without that exclusion. This is a host integration test, not an iPhone camera or physical-cartridge test.
