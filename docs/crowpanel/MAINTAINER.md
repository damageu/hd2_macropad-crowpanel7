# Maintainer checklist

Use this when syncing `unic8s/hd2_macropad` or preparing a CrowPanel build. Hardware confirmation on the DIS02170A comes before any release tag. Do not contact Erik Lembke for this fork. Do not open a paid CI plan or turn on a workflow that builds firmware on every push.

The import recorded here started from `689c67b8c2fb7e9082c2c3a04c3452989586edd3`, which matched upstream `main` and this fork's `main`.

## Upstream sync

1. Fetch `https://github.com/unic8s/hd2_macropad` and compare `689c67b8c2fb7e9082c2c3a04c3452989586edd3` (or the last sync SHA written at the bottom of this file) to upstream `main`.
2. Read the diff before applying it. Keep upstream fixes. Do not reset this fork to an older tag to make an old patch apply.
3. Files the CrowPanel port owns, and that need a careful read when upstream touches them:
   - `platformio.ini` environment `crowpanel-adv7` only. Leave `LVGL-320-480` as upstream wrote it
   - `boards/crowpanel_adv7.json`
   - `partitions/crowpanel_adv7_16MB.csv`
   - `sdkconfig.crowpanel-adv7`
   - `src/board_crowpanel_adv7.c`
   - `src/usb/serial_link_cp7.c`
   - `src/esp_bsp.c` and `src/usb/usb_controller.c` include guards
   - `src/configuration.c` CrowPanel blocks, without removing `setTrigger`
   - `src/main.c` CrowPanel blocks, without removing upstream trigger-mask callers
   - `libraries/audio_player/i2s_config.h` and `i2s_player.c`
   - `src/ble/ble_controller.c` connection-interval request
   - `utils/merge-bin.py` `MERGE_EXTRA_IMAGES`
   - `utils/write-version.py` CrowPanel version override. Do not let a CrowPanel build run `git pull`
   - `link/` and `tools/crowpanel/`
4. If a conflict cannot be resolved without guessing, stop and write down the overlapping hunks. Do not drop the trigger key, the keymap highlight fix, or the licence text to get a clean apply.
5. After a sync, set the About version so it cannot be mistaken for an older tested binary. `v1.8.6-cp7.2` is the historical tested build. `v1.9.1-cp7.2` is this rebase. A later upstream tag needs a new label.
6. Record the new upstream SHA at the bottom of this file in the same commit as the sync.

## Behaviour that must survive a sync

- PCB V1.3 pin: `-DCP_FORCE_REV=13`
- Connectivity USB on this board is `serial_link_cp7.c` at 115200, not TinyUSB
- Press gate is `usb_link_active()`, not a fresh PING
- Sequence hand-off stays race-free (`hidBusy`, mask and tail published before `stratagemCode[0]`)
- Modifier lead time 80 ms. It applies to whichever trigger mask upstream selects, CTRL or ALT
- Sounds run on their own task. Input poll 10 ms. Link icon timeout 5 s
- Assets FAT at `0xA10000`. App-only updates write `0x10000` and do not erase NVS
- Link v1.1 pacing: hold at least 30 ms, gap at least 8 ms, PING every 500 ms
- Flash and backup baud 460800 or lower. Refuse 921600
- Close the Link app before opening the COM port from esptool

## Attribution and licence

- Keep `LICENSE.md` as CC BY-NC-SA 4.0. Do not replace it with a different licence
- Keep the About screen credits and the `github.com/unic8s/hd2_macropad` label
- Keep Erik Lembke named as the original author in the README, with the upstream repository and wiki links
- Evan Ford is credited for the CrowPanel port. Say that fork maintenance is AI-assisted
- Do not add a private email address, a machine name, or a local home-directory path to docs, scripts, or commits

## Donation audit

Before every sync commit, search the tree outside third-party AGG headers. The only project funding destinations are Erik Lembke's existing ones:

- README button: `https://www.buymeacoffee.com/unic8`
- `.github/FUNDING.yml`: `github: unic8s` and `buy_me_a_coffee: unic8`

Do not add `damageu`, Evan, a bot account, a new Buy Me a Coffee username, PayPal, Ko-fi, Patreon, or a `custom:` URL. Do not rewrite the Support section into a request for money. CrowPanel docs may point at the existing button and should say there is no obligation to donate.

`CODE_OF_CONDUCT.md` contact `info [at] unic8.com` stays. It is not a payment address.

The AGG comments under `libraries/eez-framework/` that say "sponsored by" are third-party source. Leave them.

## Firmware updater

The Link app stable channel only accepts a public GitHub release whose tag is `vX.Y.Z-cp7.N` and whose assets include `crowpanel-update.json` at:

`https://github.com/damageu/hd2_macropad-crowpanel7/releases/download/<tag>/crowpanel-update.json`

The manifest `channel` must be `stable`, and `version` must equal that tag. The image file name is `hd2_macropad_<version>_app_0x10000.bin` in the same release. The manifest cannot set an address, a baud rate, or a command. Do not add those fields. A local file named `crowpanel-update.json` is accepted only when `channel` is `test`, from the separate tray item.

Do not publish a release to exercise the button. The test package under `tools/crowpanel/test-package/` is the unreleased hardware-test path. Rebuild it with `python3 tools/crowpanel/make_test_package.py` after `pio run -e crowpanel-adv7`. The app descriptor version and `SW_VER` are both `v1.9.1-cp7.2` (`-DPROJECT_VER` on the CrowPanel board and `utils/write-version.py`).

## Release gate

Do not do these from this fork until the physical test list in `docs/crowpanel/README.md` has passed on the commit you would ship:

- no `git tag`
- no GitHub Release
- no run of `.github/workflows/main.yml`

That workflow is upstream's manual release job. It creates a tag and uploads binaries for the JC3248 environment. Leave it `workflow_dispatch` only. Do not uncomment the push trigger, and do not add a CrowPanel build to it as part of ordinary sync.

An app-only update is the normal field update when the partition table is unchanged. Say so in the release notes, and say that a full flash erases presets. Publish checksums of the files you actually built. Do not reuse the historical `v1.8.6-cp7.2` checksums for a newer binary.

## Last sync

- Date: 2026-11-06
- Fork `main` / upstream `main`: `689c67b8c2fb7e9082c2c3a04c3452989586edd3`
- Historical tested port: upstream `82bba348fc2a2b39182b83ef7b893b1fe5185934` (`v1.8.6`) plus the cp7.2 patches, rebased forward. Not byte-identical to that tested firmware
