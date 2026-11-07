# CrowPanel Advance 7.0

This fork runs Erik Lembke's HD2 Macropad on the Elecrow CrowPanel Advance 7.0. The port was written by Evan Ford. Maintenance of this fork is AI-assisted. It is not an official unic8s release.

The original project, wiki, and on-device credits stay with Erik Lembke (`unic8s`) and Oliver G. (`@Gore66`):

- Upstream: https://github.com/unic8s/hd2_macropad
- Wiki: https://github.com/unic8s/hd2_macropad/wiki
- About screen text, unchanged by this port: `github.com/unic8s/hd2_macropad`, `Oliver G. @Gore66`, `Erik L. @unic8s`

Licence of this repository: CC BY-NC-SA 4.0 (`LICENSE.md`). The About screen still shows the upstream sentence, which does not say ShareAlike. The licence file is the one that applies.

## Donations

Donations support the original author, Erik Lembke. There is no obligation to donate.

The Support section of the root README already contains his Buy Me a Coffee button:

https://www.buymeacoffee.com/unic8

`.github/FUNDING.yml` already lists GitHub Sponsors username `unic8s` and Buy Me a Coffee username `unic8`. This fork adds no funding username, custom payment URL, or other destination. Do not send money to the fork maintainer.

## Hardware this port is built for

- Elecrow CrowPanel Advance 7.0, SKU DIS02170A, PCB V1.3
- ESP32-S3-WROOM-1-N16R8, 16 MB flash, 8 MB octal PSRAM
- 800x480 RGB panel. The macropad UI stays 480x320 and is scaled 1.5x into a centred 720x480 window, with black side borders
- GT911 touch on I2C SDA 15 / SCL 16
- STC8 companion MCU. The build is pinned with `-DCP_FORCE_REV=13`
- USB-C is a CH340K serial bridge (USB VID `1A86`). It is not native USB
- On the maintainer PC the bridge enumerates as COM5. Another PC may use a different COM port. The scripts default to COM5 and take a different port as the first argument

Do not use Bluetooth on this board. Do not expect native USB HID. In Config, set Connectivity to USB. That option is the serial link to HD2 Macropad Link, not TinyUSB. The upstream Bluetooth code is still in the firmware because the tested port did not remove it. Selecting it is not the supported path.

The original JC3248W535 image from upstream must not be flashed onto this panel. It uses a different display, a different partition layout, and TinyUSB.

## Audio

- Plug a speaker with a PH2.0 connector into the board's SPK header
- Set the DIP switches to S1=0 and S0=0 so the speaker path is selected
- S0=1 with S1=0 selects the wireless module. S1=1 with S0=1 selects the microphone and SD card. Those positions are not the speaker path
- Unmute sound in Config
- The NS4168 amplifier is enabled only while a sound plays (STC8 commands 248 on, 249 off on PCB V1.3)
- I2S pins in this mode are DIN 4, BCK 5, LRCK 6, both slots

Sounds live in a read-only FAT partition named `assets` at `0xA10000` (4 MB), because the SD slot shares pins with the speaker. Missing sounds mute playback. They do not stop the rest of the firmware from booting.

## First-time setup

1. Build or use `HD2MacropadLink-v1.2.exe` from `link/BUILD.txt`. Put it somewhere you can leave it. The app starts with Windows the first time you run it. The tray menu can turn that off.
2. Plug the CrowPanel USB-C cable into the PC. Windows shows a CH340 COM port. You do not type the COM number. If more than one CH340 is plugged in, unplug the others before an update.
3. On the macropad, open Config and set Connectivity to USB. The tray icon should say it is connected. The link stays at 115200 baud.
4. This firmware is not a published release yet. Use the tray menu **Install local test package...** and open `tools/crowpanel/test-package/crowpanel-update.json`. The window says LOCAL TEST PACKAGE. Press **Update** only when you are not in a mission. **Check board only** restarts the macropad and does not write.
5. The app backs up the current application under `%LOCALAPPDATA%\HD2MacropadLink\backups\`, writes the new app at `0x10000`, reads it back, and reconnects. Presets stay in NVS. After it finishes, About should show `v1.9.1-cp7.2`.

The batch scripts in `tools/crowpanel/` are the recovery path if the screen stays blank. They are not required for a normal update.

## Later updates

When a stable GitHub release exists, the tray menu **Check for firmware update...** reads the public release list for `damageu/hd2_macropad-crowpanel7` with no token. It shows the version and the notes. Nothing is written until you press **Update**. It will not downgrade, it will not erase the chip, and it ignores draft and prerelease releases.

Until that release exists, the check says that no stable package is published. That is expected. Do not publish a release just to make the button light up. The local test package is the path for this first hardware run.

The updater does not download or start a new copy of itself. Replacing HD2 Macropad Link later means installing a new exe you built or that a release explicitly documents. This version does not do that for you.

A USB unplug or a power loss while the write is in progress is not an atomic update. The backup file on the PC is the recovery image. The app will try to write that previous app back if the new image does not read back, but only while the serial port is still open. It does not promise it can recover every failure.

## HD2 Macropad Link v1.2

Source and build steps: `link/BUILD.txt`.

The app finds a CH340 serial port, answers PINGs, and turns keyboard frames into `SendInput` scancodes. v1.1 keeps each key down for at least 30 ms and leaves at least 8 ms between events, so a burst after the PC wakes cannot become a zero-length tap. It pings every 500 ms. RTS and DTR stay released so opening the port does not reset the board.

The protocol baud rate is 115200. That number is not the esptool flash baud. Do not "speed up" the link by changing either side away from 115200.

Close the Link app before a manual backup, update, or flash. It holds the COM port. The scripts close `HD2MacropadLink.exe`, `HD2MacropadLink-v1.1.exe`, and `HD2MacropadLink-v1.2.exe`, and no other processes. The in-app updater pauses its own serial connection instead.

An optional log is off unless you set `HKCU\Software\HD2MacropadLink` value `Log` (DWORD) to 1. The file is under `%LOCALAPPDATA%\HD2MacropadLink\link.log`.

## What you are building

| Item | Value |
| --- | --- |
| Fork checked | `damageu/hd2_macropad-crowpanel7`, parent `unic8s/hd2_macropad` |
| Import base | `689c67b8c2fb7e9082c2c3a04c3452989586edd3` (`Update LICENSE.md`, 2026-11-03), identical to upstream `main` at import |
| Tested port base | upstream `82bba348fc2a2b39182b83ef7b893b1fe5185934`, tag `v1.8.6` |
| About version of this tree | `v1.9.1-cp7.2` |
| PlatformIO env | `crowpanel-adv7` (`espressif32@6.9.0`) |

`v1.9.1-cp7.2` means the tested cp7.2 behaviour rebased onto current upstream. It is not the hardware-tested `v1.8.6-cp7.2` binary. Upstream commits kept by the rebase:

- `40b0271` trigger key configuration (CTRL or ALT), stored in NVS
- `43c66b9` tag `v1.9.1`, trigger mask resolution
- `5d0fea0` keymap dropdown highlight colour
- `689c67b` full CC BY-NC-SA 4.0 licence text

Those commits did not touch the port files except `src/configuration.c`. The cp7 hunks still applied around the new `setTrigger` helper, and `setTrigger` is still called from load and reset. No upstream behaviour was reverted to force the old patches on.

There is no firmware release and no tag for this import. Hardware confirmation is the release gate.

## Build the firmware

From the repository root, with Python 3 and network access so PlatformIO can download Espressif's platform once:

```text
python -m pip install --user platformio
pio run -e crowpanel-adv7
```

`utils/write-version.py` labels this environment `v1.9.1-cp7.2` and does not run `git pull` for it. Override with `HD2MP_VERSION` if you need a different About string. Leave the original `LVGL-320-480` environment alone. `pio run` with no `-e` still builds that upstream JC3248 target, and its script can run `git pull`.

The app binary is:

```text
.pio/build/crowpanel-adv7/firmware.bin
```

Copy it next to the Windows scripts as:

```text
tools/crowpanel/hd2_macropad_v1.9.1-cp7.2_app_0x10000.bin
```

That file is written at `0x10000`. It does not contain the sound partition.

### Sound image

`sdkconfig.crowpanel-adv7` has long file names off. Every file under `sdcard/assets/sound/` is already an 8.3 name. Generate a FAT image whose root contains the `assets` directory, partition size `0x400000`, sector size 4096. After PlatformIO has installed the framework, `fatfsgen.py` is under the installed `framework-espidf` package:

```text
python ~/.platformio/packages/framework-espidf/components/fatfs/fatfsgen.py sdcard --output_file assets_fat_0xa10000.img --partition_size 0x400000 --sector_size 4096
```

Do not pass `--long_name_support`. The sdkconfig leaves long names off. Use `fatfsgen.py`, not `wl_fatfsgen.py`: the firmware mounts this partition read-only. Run `fatfsgen.py --help` if a future framework moves those flags. The image is not committed. A full flash image without it still boots. Sounds stay silent until the `assets` partition is present.

Merge the app with that image:

```text
MERGE_EXTRA_IMAGES="0xa10000 assets_fat_0xa10000.img" pio run -e crowpanel-adv7 -t mergebin
```

Copy `.pio/build/crowpanel-adv7/firmware-merged.bin` to:

```text
tools/crowpanel/hd2_macropad_v1.9.1-cp7.2_crowpanel_adv7_0x0.bin
```

`utils/merge-bin.py` refuses the merge if `MERGE_EXTRA_IMAGES` is not offset/file pairs or the file is missing.

## Flash, update, backup

Scripts, Windows only:

| Script | What it does |
| --- | --- |
| `tools/crowpanel/backup_COM5.bat` | Reads 16 MB. Does not erase |
| `tools/crowpanel/update_COM5.bat` | Writes the app at `0x10000` only |
| `tools/crowpanel/flash_COM5.bat` | Asks for a backup, then erases the whole chip and writes the merged image at `0x0` |

```text
backup_COM5.bat COM5 460800
update_COM5.bat COM5 460800
flash_COM5.bat COM5 460800
```

The first argument is the COM port (default COM5). The second is the baud rate. Allowed rates are 115200, 230400, and 460800. 921600 is refused. On this PC, 921600 corrupts reads. If the fast rate fails, the scripts retry at 115200.

Close HD2 Macropad Link before any of them. The scripts also close those two executable names. They do not install or upgrade Python packages. Install esptool yourself (`python -m pip install esptool`) if `python -m esptool version` fails.

### App-only update

Use `update_COM5.bat` when the board is already running a cp7, cp7.1, or cp7.2 layout:

- NVS is at `0x9000` and is not rewritten
- factory app is at `0x10000`, size `0xA00000`
- assets FAT is at `0xA10000`
- presets and the Connectivity setting stay

This rebase did not change NVS key names. `trigger` is an additional key from upstream v1.9.1. Older cp7.2 presets still load. The new trigger key defaults to CTRL when the key is absent.

Do not use the app-only update on a stock image, a wokka1 image, or an upstream JC3248 image. The partition layout is different.

### Full flash

Use `flash_COM5.bat` for a first install or a corrupted layout. It erases the chip. Presets and settings are deleted. After it boots, set Connectivity to USB again.

Take a backup first. The script offers one and will not erase if that backup fails. Skip it only by typing `ERASE`. Keep the backup file somewhere other than the only copy on the machine. `backup_COM5.bat` can also be run on its own.

## Recovery

1. Close HD2 Macropad Link and any serial monitor.
2. Retry at 115200: `flash_COM5.bat COM5 115200` or the same form of `update_COM5.bat` / `backup_COM5.bat`.
3. If esptool still cannot reset the chip, put it in download mode with the board's BOOT and RESET buttons: hold BOOT, tap RESET, release BOOT, then run the script again at 115200. That is the usual ESP32-S3 sequence on this panel. Elecrow's DIS02170A documentation is the reference if the buttons on a later PCB differ.
4. A failed app-only update does not erase NVS. A failed full flash may already have erased it. Restore only from a backup you made before the erase, written back with esptool at the same baud limit: `python -m esptool --chip esp32s3 --port COM5 --baud 115200 write_flash 0x0 flash-backup-COM5-YYYYMMDD-HHMMSS.bin`
5. Do not recover this board by flashing `hd2_macropad_install.bin` from an upstream release.

## Previously tested artifacts

These files are not in the git repository. They are the cp7.2 build that was exercised on hardware against upstream `82bba34` (tag `v1.8.6`). This branch does not reproduce those checksums, because the source now includes the v1.9.1 trigger work and the later licence and keymap commits.

```text
edf46ae04503e88228de06d18203370645b33b509cf985ecd63137a4f0df2436  hd2_macropad_v1.8.6-cp7.2_crowpanel_adv7_0x0.bin
1ebf4ad03be4222444dbedfad27027898d9d22ca6ef0329216508a2a039b7028  hd2_macropad_v1.8.6-cp7.2_app_0x10000.bin
d4b9bb6a322f6e2480b16544b75971010aa6b7b1445fe67a5fedc6cf3c965c10  assets_fat_0xa10000.img
28702e8f4c0fcca7844e4bd5ec9754421629e32468769481a3cfca8d3b7de163  HD2MacropadLink-v1.1.exe
```

The old Link executable's version resource said 1.0.0. `link/res.rc` in this tree says 1.1.0, so a new build will not match `28702e8f...`.

## Behaviour carried over from the tested cp7.2 port

- A press is not dropped because one PING was late. The gate is `usb_link_active()` (Connectivity is USB), not "a PING arrived in the last few seconds". The link icon still uses a 5 second timeout
- The modifier and the rest of the sequence are published before `stratagemCode[0]`, and a second press is ignored while a sequence is still being sent
- CTRL or ALT, whichever the trigger setting selects, is held at least 80 ms before the first arrow when the input delay is shorter than that
- Sounds play on their own task. The input poll is 10 ms
- Default input delay on this board is 50 ms when NVS has no value. An existing saved delay is kept
- Label text for delay, brightness, and cooldowns uses stack buffers

## Next physical test

This is the release gate. Nothing here was flashed, and no release tag was created.

1. Build `crowpanel-adv7` from this commit. Do not reuse the v1.8.6-cp7.2 bin as if it were this commit.
2. Build or keep HD2 Macropad Link v1.1. Close it.
3. If the board is already on a cp7.x layout and you want to keep presets, run `backup_COM5.bat` and then `update_COM5.bat` at 460800 or 115200. Otherwise back up and run `flash_COM5.bat`, then set Connectivity to USB.
4. Confirm the About screen shows `v1.9.1-cp7.2`, the original credits, and `github.com/unic8s/hd2_macropad`.
5. Confirm the Link tooltip shows the CH340 port, a press works, and the first press after a few minutes idle still works.
6. Assign a stratagem, including TD-110 Maelstrom, with trigger CTRL and again with ALT.
7. DIP S1=0, S0=0, PH2.0 speaker on SPK, mute off, and confirm a sound plays.
8. Confirm the keymap dropdown highlight. That fix came from upstream after v1.8.6 and has not been seen on this panel.
9. Do not enable the GitHub Actions workflow. It tags a release and uploads binaries. Do not publish a release until this list passes.
