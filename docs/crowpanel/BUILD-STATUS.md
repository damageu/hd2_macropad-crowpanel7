# Build record for this import

No board was flashed. No tag was created. No GitHub Release was published. The firmware files below were build products on the review machine and are not committed.

## Firmware

Command:

```text
pio run -e crowpanel-adv7
```

Result: SUCCESS in 58.63 seconds.

| Tool | Version |
| --- | --- |
| PlatformIO | 6.2.0 |
| platform | espressif32 6.9.0 |
| framework-espidf | 3.50301.0 (ESP-IDF 5.3.1) |
| toolchain-xtensa-esp-elf | 13.2.0+20240530 |
| esptool.py | 4.5.1 |

`src/version.h` written by `utils/write-version.py` for this environment:

```text
#define SW_VER "v1.9.1-cp7.2"
```

The CrowPanel environment did not run `git pull`.

Size check from the build:

- RAM: 53276 of 327680 bytes (16.3%). That figure is internal SRAM from the board description, not PSRAM.
- Flash: 2462556 of 10485760 bytes (23.5%). 10485760 is the 10 MB factory partition (`0xA00000`).

`firmware.bin`: 2462928 bytes, SHA-256 `18f9d2ad71274ef0169f11733a0c243b245ed8c1f0cc9b0ecfe2e306ae410b52`.

The built partition table decoded from `partitions.bin` is:

| Name | Offset | Size |
| --- | --- | --- |
| nvs | `0x9000` | `0x6000` |
| phy_init | `0xf000` | `0x1000` |
| factory | `0x10000` | `0xA00000` |
| assets | `0xA10000` | `0x400000` |

`sdkconfig.crowpanel-adv7` still contains the upstream-style `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE` text from the tested port. PlatformIO's `board_build.partitions` replaced it at build time. The decoded table above is the one that was produced. Do not assume the sdkconfig choice string is the flashed layout.

Compiler warnings were only the existing ones in `src/ui/ui_events.c` (incompatible pointer and discarded const). `board_crowpanel_adv7.c`, `serial_link_cp7.c`, `configuration.c`, `main.c`, and `i2s_player.c` compiled with no warnings of their own.

This machine needed the `python3.12-venv` package before PlatformIO could create the ESP-IDF virtualenv. The first attempt failed on that and was rerun.

## Sound image and merged bin

`sdcard/` is unchanged from upstream tag `v1.8.6`. `fatfsgen.py` from ESP-IDF 5.3.1, without long-name support:

```text
fatfsgen.py sdcard --output_file assets_fat_0xa10000.img --partition_size 0x400000 --sector_size 4096
```

Image: 4194304 bytes, SHA-256 `8617a51dcdf23e19ef856e0509e3af72b1cb749b5ecbb7f75aef4b0eca9c71f6`. That hash includes directory timestamps from this run (`fatfsgen.py` was not passed `--use_default_datetime`), so it is evidence the tool worked, not a release identifier.

Directory entries for `ASSETS`, `SOUND`, `INTRO.WAV`, `_SWT`, `REINF`, and `SUPP` are present as 8.3 names. This checksum is not the historical tested image `d4b9bb6a...`. Same WAV bytes can still pack differently.

Merge, not flashed:

```text
MERGE_EXTRA_IMAGES="0xa10000 assets_fat_0xa10000.img" pio run -e crowpanel-adv7 -t mergebin
```

Result: SUCCESS in 5.59 seconds. `firmware-merged.bin` is 14745600 bytes (`0xE10000`), SHA-256 `cd2b5e90e5546f799510cfefd0eb1b026be0de4304ec914f074c9da49cb98611`. The same timestamp caveat applies to this merged file. It was not flashed. The `ASSETS` directory entry is at file offset `0xA10000` plus 12320, which is where it sits inside the FAT image. Esptool was invoked with `--chip esp32s3`, flash size 16MB, and the app at `0x10000`.

## Link unit test

```text
gcc -Wall -Wextra -Werror -O2 -o test_core link/test_core.c && ./test_core
```

Exit 0.

```text
frames=9 pongs=1
+01D +011 -011 +01F -01F +020 -020 -01D 
```

That is Ctrl down, then W, S, and D pressed and released, then Ctrl up. A garbage log line and a broken frame in the middle were ignored. One PONG was counted. `link_release_all` after that sequence added no extra events.

The Windows executable was not cross-built. MinGW-w64 was not required for the parser test, and no `.exe` is committed. The historical v1.1 executable checksum remains `28702e8f...` and will not match a rebuild, because `link/res.rc` now says 1.1.0.

## Updater rebuild

`boards/crowpanel_adv7.json` now passes `-DPROJECT_VER=v1.9.1-cp7.2`. Rebuilt with `pio run -e crowpanel-adv7` after deleting the previous build directory: SUCCESS in 53.03 seconds. The app descriptor at offset `0x30` is `v1.9.1-cp7.2`, and the same string is also the UI version. `upd_image_length` and `upd_image_version` accept this file.

`firmware.bin` / test-package image: 2462928 bytes, SHA-256 `c12dd56c41f1b0d531e12e3eceb8930ff6c0ea46f78bfdf23f3e4c706562aa63`.

The earlier hash `18f9d2ad...` was the pre-updater build, whose app descriptor still said `v1.9.1-2-g689c67b-dirty`. Do not flash that file for this test.

Linux tests, all exit 0, no hardware and no serial port:

- `test_core`: `frames=9 pongs=1` and `+01D +011 -011 +01F -01F +020 -020 -01D `
- `test_update`: manifest schema, hash, downgrade, evil host, prerelease skip, partition layout
- `test_romflash`: mock ROM. Upgrade keeps the NVS byte, dry run does not call flash-begin, downgrade does not write, a failed write restores the backup, cancel and disconnect happen before any write, a bad partition table never starts a write, and no erase-chip opcode is sent

The real `crowpanel-update.json` in `tools/crowpanel/test-package/` parses as channel `test` and is an upgrade from `v1.8.6-cp7.2`.

`HD2MacropadLink-v1.2.exe` was cross-compiled with `x86_64-w64-mingw32-gcc` 13 on Linux and was not run on Windows. Size 505326 bytes, SHA-256 `b14b0431e13184df4ca36f972dd01106e86795c4fdd388458ee7d08781874655`. The only compiler warning was the existing cast in `wWinMain`.

## Not done

- No flash of COM5 or any other port
- The in-app updater was not run against a board or a Windows desktop. Mock serial only
- No GitHub release exists for the stable button, on purpose
- No run of `.github/workflows/main.yml`
- No tag and no GitHub Release
- The v1.9.1 trigger control and keymap highlight have not been seen on the 7 inch panel
- Bluetooth was not removed from the firmware. It is not the supported path on this board
