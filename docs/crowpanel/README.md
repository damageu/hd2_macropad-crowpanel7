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
- The installer looks for that CH340 and uses whatever COM port Windows assigned. The maintainer's PC uses COM5. Other PCs will differ. The recovery scripts still default to COM5 and take a different port as the first argument

Official size 181.26 x 108.36 x 16 mm. Active area 156 x 87 mm. Four rear M3 holes. Display driver SC7277. The board has a microphone, an RTC, and a replaceable wireless-module slot. Evan's unit is PCB V1.3. Elecrow has shipped V1.3 through V1.5 of this product line. PCB V1.4 and V1.5 have not been tested with this firmware. The build is still pinned to PCB V1.3 (`-DCP_FORCE_REV=13`).

Do not use Bluetooth on this board. Do not expect native USB HID. In Config, set Connectivity to USB. That option is the serial link to HD2 Macropad Link, not TinyUSB. The upstream Bluetooth code is still in the firmware because the tested port did not remove it. Selecting it is not the supported path.

The original JC3248W535 image from upstream must not be flashed onto this panel. It uses a different display, a different partition layout, and TinyUSB.

## Buying the board

Checked 12 November 2026. Buy the **CrowPanel Advance 7.0**, SKU **DIS02170A**, ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB PSRAM), 800x480, display driver SC7277.

- AliExpress, where Evan bought his: https://www.aliexpress.com/item/1005008312813475.html — Evan's unit came with a case. The current listing appears no longer to include one.
- Amazon US, sold by Elecrow, in stock at US$59.99 on 12 Nov 2026. The title says an acrylic case is included: https://www.amazon.com/ELECROW-Interaction-Dual-Core-Processor-Compatible/dp/B0FX5447WD — the included-components line says "CrowPanel Advance ESP32 Display-7.0 inch". A review mentioned a V1.5 board. Evan's is PCB V1.3. Same product line, later revision.
- Amazon Australia, same ASIN, out of stock on 12 Nov 2026: https://www.amazon.com.au/dp/B0FX5447WD — check back rather than buying a different panel.
- Elecrow's store, SKU DIS02170A: https://www.elecrow.com/crowpanel-advance-7-hmi-esp32-ai-display-800x480-ai-ips-touch-screen.html

Do not buy the CrowPanel **Basic** 7 inch. That is a different board (model DIS08070H001, ESP32-S3-WROOM-1-N4R8, 4 MB flash, display chips EK9716BD3 and EK73002ACGB). One listing that is the Basic, and is not this project, is https://www.amazon.com.au/GrowCube-ESP32-S3-CrowPanel-Suitable-Assistant/dp/B0GTVDQMLD. This firmware is not for it.

Do not buy the CrowPanel Advance 7 **ESP32-P4** (1024x600). That is a different product.

## Case

Evan is a beginner at 3D printing. He is working on a printable case and will provide the files if and when he has them worked out. There is no date. There is no shop and no payment link for a case.

## Audio

- Plug a speaker with a PH2.0 connector into the board's SPK header
- Set the DIP switches to S1=0 and S0=0 so the speaker path is selected
- S0=1 with S1=0 selects the wireless module. S1=1 with S0=1 selects the microphone and SD card. Those positions are not the speaker path
- Unmute sound in Config
- The NS4168 amplifier is enabled only while a sound plays (STC8 commands 248 on, 249 off on PCB V1.3)
- I2S pins in this mode are DIN 4, BCK 5, LRCK 6, both slots

Sounds live in a read-only FAT partition named `assets` at `0xA10000` (4 MB), because the SD slot shares pins with the speaker. Missing sounds mute playback. They do not stop the rest of the firmware from booting.

## First install (Windows exe)

The primary path is one Windows program: `HD2MacropadLink-v1.2.5.exe`. It is the tray app and the installer. There is no Mac or Linux installer, and there is no second download for Link. It is not a published GitHub Release yet. Build it with `link/BUILD.txt`, or use the exe attached to the pull request that added this installer.

Plug the panel in before you start the exe. **The USB-C cable must carry data as well as power.** A charge-only (power-only) cable will not work. The panel may light up, but Windows will not see the CH340 COM port, and the installer cannot talk to the board. Use a cable you know can transfer files, or one sold as a data cable. If no COM port appears, try another cable and another USB port. The installer finds the CH340 itself. You do not type COM5.

The first window is **Install HD2 Macropad firmware**.

**Back up and install** is for a stock Elecrow board, or any board that is not already running this HD2 port. In order:

1. It reads the entire 16 MB flash to `%LOCALAPPDATA%\HD2MacropadLink\backups\flash-full-<timestamp>.bin` before it changes the chip. This backup is the long step. Baud is 460800. 921600 is never used. The status line shows bytes and percent the whole time.
2. It writes four regions only: bootloader at `0x0`, the cp7 partition table at `0x8000`, the `v1.9.1-cp7.2` app at `0x10000`, and the sound FAT at `0xA10000` (4 MB).
3. It checks each region (MD5 with the flasher stub, or a read-back if the stub had to fall back to the ROM loader). The log says `FALLBACK` when that happens.
4. It resets the board and reads the app version. Success means that read is `v1.9.1-cp7.2`.

### How long to wait

A real 16 MB first install has not been timed on this board. Do not treat the ranges below as a stopwatch reading.

What was measured, with Link v1.2.3 on this CrowPanel at 460800 and the fast stub, was an app-only job on a board that already had HD2: the 2.4 MB app write took 23.3 seconds, a reinstall took 34.3 seconds in total, and reading that 2.4 MB app region for a backup took 62.6 seconds. That read was not 16 MB. At that rate a 16 MB backup is around 7 minutes.

Expected times for **Back up and install**:

- The 16 MB backup is the long step and typically takes about 7 to 10 minutes.
- Writing and checking the bootloader, partition table, 2.4 MB app, and 4 MB sounds then takes a few more minutes. Those writes are compressed.
- The whole first install is often around 10 to 15 minutes.

A slower PC, or a fallback to the ROM loader (the log says `FALLBACK`), can take longer. If a later run on this hardware times the 16 MB backup or the whole first install, those measured figures replace these estimates.

While the phase name, the byte count, or the percent is moving, wait. Do not unplug the board and do not close the installer, especially during a write.

- Finding the CH340 port and talking to the bootloader should succeed within about a minute. If it cannot find a port, or the bootloader attempt count stops, after that minute treat it as a problem: cable, driver, or the port is in use. See Troubleshooting below.
- The 16 MB backup can stay on that phase for many minutes. The byte count must keep increasing. If the same number or the same progress line has not changed for 3 minutes, treat it as stuck.
- Write and verify are shorter. The same rule applies: frozen progress for 3 minutes means stuck.
- A finished failure is not a hang. The window says it failed and offers **Send the log for checking**. Use that. If it failed mid-write and rollback did not finish, do not keep retrying. Send the log.

It does **not** mass-erase the chip. It does **not** write NVS at `0x9000` (24 KB) or phy_init at `0xF000`. A stock board's NVS is not HD2 presets. Those bytes stay where they are. If a later step fails, the installer writes the backup back over every region it changed, including the sounds, and still does not write NVS. After the board boots, open Config and set Connectivity to USB. The HD2 app itself erases NVS only if `nvs_flash_init` reports no free pages or a new NVS version (`src/configuration.c`). This installer does not do that erase.

The sound image in the exe is the one used for the cp7, cp7.1, and cp7.2 builds (SHA-256 `d4b9bb6a322f6e2480b16544b75971010aa6b7b1445fe67a5fedc6cf3c965c10`). All 20 WAVs match upstream `unic8s/hd2_macropad` `sdcard/assets` at commit `82bba34`. You still need a speaker with a PH2.0-2P plug on the SPK header, DIP S1=0 and S0=0, and mute off, to hear them. v1.2.4 did not write this partition. v1.2.5 does. Do not run Back up and install on a board that already runs HD2.

If the board **already** runs HD2 (cp7, cp7.1, or cp7.2), do not use Back up and install. Use the tray menu **Check for firmware update...** or, until a release exists, **Install local test package...**. That path writes only the app at `0x10000` and leaves NVS, including presets, alone.

If a step fails, the window keeps the log and shows **Send the log for checking**. That opens a pre-filled GitHub issue on `damageu/hd2_macropad-crowpanel7` with the Link version, firmware version, COM port, exit code, and the last error. It also opens the log folder and copies the log path to the clipboard. Drag the `install-*.log` file onto the issue. Do not paste a home-folder path into the issue text. The issue body does not include that path.

Close this exe before a manual esptool run. It holds the COM port. If Windows says the port is busy, quit Link from the tray (right-click, Exit) and close any serial monitor.

## If the board already runs HD2

1. Quit any older HD2 Macropad Link from the tray. The COM port is exclusive.
2. Run `HD2MacropadLink-v1.2.5.exe`. It picks the CH340. Unplug other CH340 devices first if more than one is connected.
3. On the macropad, open Config and set Connectivity to USB. The tray icon should say it is connected. The link stays at 115200 baud.
4. There is still no stable GitHub release. Use **Install local test package...** and open `tools/crowpanel/test-package/crowpanel-update.json`. The window says LOCAL TEST PACKAGE. Press **Update** only when you are not in a mission. **Check board only** restarts the macropad and does not write.
5. The updater backs up the current app under `%LOCALAPPDATA%\HD2MacropadLink\backups\`, writes the new app at `0x10000` only, checks the flash MD5, and confirms the version after reset. NVS is not written. About should show `v1.9.1-cp7.2`.

On the maintainer's board (ESP32-S3, 8 MB PSRAM, MAC `10:b4:1d:c4:09:b4`), already on `v1.9.1-cp7.2`, app MD5 `b0f9ebf600521d2c1fc9fd884a5e34df`, at 460800 with the stub and no ROM fallback: a reinstall took 34.3 seconds total (write 23.3 seconds). A run that read the app for the backup took 93.1 seconds total (read 62.6 seconds).

A USB unplug or a power loss during the write is not an atomic update. If the write or the MD5 check fails while the loader is still answering, the updater writes the previous app back. If it cannot, use the backup file with esptool at `0x10000` and baud 115200 or 460800. Do not erase the chip to recover an app-only update.

## Later updates

When a stable GitHub release exists, **Check for firmware update...** reads the public release list for `damageu/hd2_macropad-crowpanel7` with no token. It shows the version and the notes. Nothing is written until you press **Update**. It will not downgrade, it will not erase the chip, and it ignores draft and prerelease releases.

Until that release exists, the check says that no stable package is published. That is expected. Do not publish a release just to make the button light up.

The program does not download or start a new copy of itself. Replacing HD2 Macropad Link later means installing a new exe you built or that a release explicitly documents.

## HD2 Macropad Link v1.2.5

Source and build steps: `link/BUILD.txt`. The updater behaviour is the hardware-tested v1.2.3 core. v1.2.4 added the first-install window without sounds. v1.2.5 is that program plus the sound partition, so the two exes stay distinguishable. Third-party notices: `link/THIRD-PARTY-NOTICES.txt`. The flasher stub is esp-flasher-stub v0.2.0 (Apache-2.0 OR MIT), not esptool's GPL stub.

The app finds a CH340 serial port, answers PINGs, and turns keyboard frames into `SendInput` scancodes. It keeps each key down for at least 30 ms and leaves at least 8 ms between events, so a burst after the PC wakes cannot become a zero-length tap. It pings every 500 ms. RTS and DTR stay released so opening the port does not reset the board.

The protocol baud rate is 115200. That number is not the flash baud. Do not "speed up" the link by changing either side away from 115200. Do not set the flash baud to 921600.

Close Link before a manual backup, update, or flash. The in-app updater and the first-install window pause Link's own serial connection. A second copy of the exe refuses to start while the tray copy is running.

Headless flags, for the same exe: `--update`, `--probe`, `--install`, `--reinstall`, `--yes`, `--log-stdout`, `--full-backup`, `--rom-only`, `--dry-run`, `--result-file`. Exit codes: 0 ok, 1 failed, 2 usage, 3 port busy or Link running, 4 bad package. `--install` without `--port` uses the only CH340. Writing requires `--yes`.

Each install or update writes a log under `%LOCALAPPDATA%\HD2MacropadLink\logs\`. An optional link log is off unless you set `HKCU\Software\HD2MacropadLink` value `Log` (DWORD) to 1. That file is `link.log` in the same folder.

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

There is no firmware release and no tag for this import. The app-only updater has been run on one board already on `v1.9.1-cp7.2`. A stock-board first install has not. See the last section. Do not tag a release from this import alone.

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

Do not pass `--long_name_support`. The sdkconfig leaves long names off. Use `fatfsgen.py`, not `wl_fatfsgen.py`: the firmware mounts this partition read-only. Run `fatfsgen.py --help` if a future framework moves those flags. A freshly generated image is not what the installer embeds. The installer uses the committed cp7 sound image `link/firmware/assets_fat_0xa10000.img`. Missing sounds mute playback. They do not stop the rest of the firmware from booting.

Merge the app with that image:

```text
MERGE_EXTRA_IMAGES="0xa10000 assets_fat_0xa10000.img" pio run -e crowpanel-adv7 -t mergebin
```

Copy `.pio/build/crowpanel-adv7/firmware-merged.bin` to:

```text
tools/crowpanel/hd2_macropad_v1.9.1-cp7.2_crowpanel_adv7_0x0.bin
```

`utils/merge-bin.py` refuses the merge if `MERGE_EXTRA_IMAGES` is not offset/file pairs or the file is missing.

## Manual flash, update, and backup

These scripts are the advanced and recovery path. They are not the normal first install. Use `HD2MacropadLink-v1.2.5.exe` for that. The scripts are Windows only. They call esptool. They do not install Python packages. Install esptool yourself (`python -m pip install esptool`) if `python -m esptool version` fails.

| Script | What it does |
| --- | --- |
| `tools/crowpanel/backup_COM5.bat` | Reads 16 MB from `0x0`. Does not erase or write |
| `tools/crowpanel/update_COM5.bat` | Writes the app at `0x10000` only. Does not erase the chip |
| `tools/crowpanel/flash_COM5.bat` | Advanced. Takes a full backup, then erases the whole chip only if you type `ERASE`, then writes a merged image at `0x0` |

```text
backup_COM5.bat COM7 460800
update_COM5.bat COM7 460800
flash_COM5.bat COM7 460800
```

The first argument is the COM port. COM5 is only the default, because that is the maintainer's port. Pass the port Device Manager shows. The second argument is the baud rate. Allowed rates are 115200, 230400, and 460800. 921600 is refused. On the maintainer's CH340K, 921600 corrupts reads. If the fast rate fails, the scripts retry at 115200.

Chip is `esp32s3`. Example, with the port you actually have:

```text
python -m esptool --chip esp32s3 --port COM7 --baud 460800 read_flash 0x0 0x1000000 flash-backup.bin
python -m esptool --chip esp32s3 --port COM7 --baud 460800 write_flash 0x10000 hd2_macropad_v1.9.1-cp7.2_app_0x10000.bin
python -m esptool --chip esp32s3 --port COM7 --baud 115200 write_flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 hd2_macropad_v1.9.1-cp7.2_app_0x10000.bin 0xA10000 assets_fat_0xa10000.img
```

The last command is the same four regions as the installer. It does not erase the whole chip and it does not write NVS. Close Link first. `assets_fat_0xa10000.img` is `link/firmware/assets_fat_0xa10000.img`.

### App-only update

Use the Link tray update, or `update_COM5.bat`, when the board is already running a cp7, cp7.1, or cp7.2 layout:

- NVS is at `0x9000` and is not rewritten
- factory app is at `0x10000`, size `0xA00000`, and is the only region this script writes
- assets FAT is at `0xA10000` and is not rewritten by an app-only update
- presets and the Connectivity setting stay

This rebase did not change NVS key names. `trigger` is an additional key from upstream v1.9.1. Older cp7.2 presets still load. The new trigger key defaults to CTRL when the key is absent.

Do not use the app-only update on a stock image, a wokka1 image, or an upstream JC3248 image. The partition layout is different. Use the first-install window.

### Full erase

`flash_COM5.bat` is not the default install. It always reads a 16 MB backup first. If that backup fails, it stops and erases nothing. After a good backup it still waits until you type `ERASE`. There is no way to skip the backup. A full erase deletes presets and settings. After it boots, set Connectivity to USB again. Keep the backup somewhere other than the only copy on the machine.

## Recovery

1. Close HD2 Macropad Link and any serial monitor. If the installer failed, use **Send the log for checking** and keep the `install-*.log` plus the `flash-full-*.bin` backup.
2. Retry the manual command at 115200: `--baud 115200`. Do not switch to 921600.
3. If esptool still cannot reset the chip, put it in download mode: hold BOOT, tap RESET, release BOOT, then run the command again at 115200. That is the usual ESP32-S3 sequence on this panel. Elecrow's DIS02170A documentation is the reference if the buttons on a later PCB differ.
4. A failed app-only update does not erase NVS. Restore the app backup at `0x10000`. A first-install rollback that the exe could not finish is a write of the full-flash backup at `0x0`, still without `erase_flash`: `python -m esptool --chip esp32s3 --port COM7 --baud 115200 write_flash 0x0 flash-full-YYYYMMDD-HHMMSS.bin`
5. Do not recover this board by flashing `hd2_macropad_install.bin` from an upstream release.

## Troubleshooting

Use this when the installer cannot find the board, when a progress line has not changed for the times in [How long to wait](#how-long-to-wait), or when the window says the install failed.

- **No COM port / charge-only cable.** The USB-C socket is a CH340K (VID `1A86`), not native USB. The cable must carry data as well as power. A charge-only (power-only) cable will not work: the panel may light up, but Windows will not see a COM port, and the installer cannot talk to the board. Use a cable you know can transfer files, or one sold as a data cable, and try another USB port on the PC. If Device Manager shows VID `1A86` with no COM port, the CH340 driver is missing. WCH publishes that driver. The installer does not assume COM5.
- **Wrong or missing COM port.** The installer uses whichever CH340 Windows assigned. COM5 is only the maintainer's port. If more than one CH340 is plugged in, unplug the others. The installer will not guess.
- **Port busy.** Quit HD2 Macropad Link from the tray (right-click the icon, Exit), including an older copy such as v1.2.4, and close any serial monitor or esptool. Exit code 3 means the port is busy or Link is still running.
- **921600.** Do not use it. It corrupts reads on this CH340K. The installer and the scripts refuse it. Use 460800, or 115200 if that fails.
- **First install versus update.** Back up and install is for a stock or wrong layout. It writes the bootloader, partition table, app, and sounds, and it does not write NVS. Do not use it on a board that already runs HD2. Check for firmware update / the local test package is that path. It writes only `0x10000` and keeps presets and the sound partition already on the chip.
- **It looks stuck.** If the byte count or percent is still changing, wait, and do not unplug. If the bootloader line has not moved for about a minute, or a backup, write, or verify line has not changed for 3 minutes, treat it as stuck. Use **Send the log for checking**. Do not start another install on top of a write that did not finish.
- **Rollback.** If the installer started a write and the check failed, it puts the backup bytes back over the regions it changed, including sounds, and leaves NVS alone. If the window says rollback did not finish, do not keep retrying. Send the log, and keep the `flash-full-*.bin` file. If the screen stays blank, write that backup at `0x0` as in Recovery. Do not mass-erase to "start clean" unless you already have that backup and you typed `ERASE` in `flash_COM5.bat` on purpose.
- **Sending a failure log.** In the installer, **Send the log for checking** opens a pre-filled GitHub issue on `damageu/hd2_macropad-crowpanel7`. The issue body is a short summary (versions, port, exit code, last error). It also opens the log folder and copies the log path to the clipboard. Drag the `install-*.log` file onto the issue. Do not paste the folder path. It contains your Windows user name.
- **Windows only.** The one-file installer is a Windows exe. There is no Mac or Linux installer. The recovery scripts are Windows batch files. Building the firmware with PlatformIO is separate and is not required to run the exe.
- **Wrong board.** This firmware is for the CrowPanel Advance 7.0, SKU DIS02170A, 800x480, ESP32-S3, 16 MB flash. It is not for the CrowPanel Basic (4 MB flash, including the GrowCube listing) and not for the CrowPanel Advance 7 ESP32-P4 (1024x600).

## Previously tested artifacts

These files are not in the git repository. They are the cp7.2 build that was exercised on hardware against upstream `82bba34` (tag `v1.8.6`). This branch does not reproduce those checksums, because the source now includes the v1.9.1 trigger work and the later licence and keymap commits.

```text
edf46ae04503e88228de06d18203370645b33b509cf985ecd63137a4f0df2436  hd2_macropad_v1.8.6-cp7.2_crowpanel_adv7_0x0.bin
1ebf4ad03be4222444dbedfad27027898d9d22ca6ef0329216508a2a039b7028  hd2_macropad_v1.8.6-cp7.2_app_0x10000.bin
d4b9bb6a322f6e2480b16544b75971010aa6b7b1445fe67a5fedc6cf3c965c10  assets_fat_0xa10000.img
28702e8f4c0fcca7844e4bd5ec9754421629e32468769481a3cfca8d3b7de163  HD2MacropadLink-v1.1.exe
```

The tested `v1.9.1-cp7.2` app that the installer embeds is in the repository:

```text
b0f9ebf600521d2c1fc9fd884a5e34df  MD5
c12dd56c41f1b0d531e12e3eceb8930ff6c0ea46f78bfdf23f3e4c706562aa63  SHA-256
2462928 bytes  tools/crowpanel/test-package/hd2_macropad_v1.9.1-cp7.2_app_0x10000.bin
```

`link/res.rc` now says 1.2.5, so a new Link build will not match `28702e8f...` or the earlier v1.2 cross-build.

## Behaviour carried over from the tested cp7.2 port

- A press is not dropped because one PING was late. The gate is `usb_link_active()` (Connectivity is USB), not "a PING arrived in the last few seconds". The link icon still uses a 5 second timeout
- The modifier and the rest of the sequence are published before `stratagemCode[0]`, and a second press is ignored while a sequence is still being sent
- CTRL or ALT, whichever the trigger setting selects, is held at least 80 ms before the first arrow when the input delay is shorter than that
- Sounds play on their own task. The input poll is 10 ms
- Default input delay on this board is 50 ms when NVS has no value. An existing saved delay is kept
- Label text for delay, brightness, and cooldowns uses stack buffers

## What has and has not been run on hardware

The app-only updater in v1.2.3 was run on Evan's CrowPanel Advance 7.0, already on `v1.9.1-cp7.2`, at 460800, with the stub and no ROM fallback. Numbers are in the section above. The stock-board first install (bootloader, partition table, app, and sounds, after a 16 MB backup) has not been run on a factory panel. Do not run Back up and install on a board that already runs HD2. The bootloader inside the exe was rebuilt from this tree's `crowpanel-adv7` sdkconfig (DIO, 80 MHz, 16 MB). The bootloader bytes that were originally on the tested board were not kept. PCB V1.4 and V1.5 have not been tested. The v1.9.1 trigger control and the keymap highlight have not been confirmed on this panel. Bluetooth was not removed.

Do not tag a release or run `.github/workflows/main.yml` until a stock-board install and the checks below have been seen on the commit you would ship:

1. About shows `v1.9.1-cp7.2`, the original credits, and `github.com/unic8s/hd2_macropad`.
2. The Link tooltip shows the CH340 port, a press works, and the first press after a few minutes idle still works.
3. Assign a stratagem, including TD-110 Maelstrom, with trigger CTRL and again with ALT.
4. DIP S1=0, S0=0, PH2.0-2P speaker on SPK, mute off. v1.2.5 writes the sound partition. Hearing it still needs that speaker.
5. Confirm the keymap dropdown highlight.
