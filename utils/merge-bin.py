#!/usr/bin/python3

# Adds PlatformIO post-processing to merge all the ESP flash images into a single image.

import os

Import("env", "projenv")

board_config = env.BoardConfig()
firmware_bin = "${BUILD_DIR}/${PROGNAME}.bin"
merged_bin = os.environ.get("MERGED_BIN_PATH", "${BUILD_DIR}/${PROGNAME}-merged.bin")


def merge_bin_action(source, target, env):
    flash_images = [
        *env.Flatten(env.get("FLASH_EXTRA_IMAGES", [])),
        "$ESP32_APP_OFFSET",
        source[0].get_abspath(),
    ]
    # Optional extra "<offset> <file>" pairs, e.g. the CrowPanel read-only FAT assets image:
    # MERGE_EXTRA_IMAGES="0xa10000 /path/assets.img"
    extra = os.environ.get("MERGE_EXTRA_IMAGES", "").split()
    if len(extra) % 2:
        raise ValueError("MERGE_EXTRA_IMAGES must be <offset> <file> pairs")
    for i in range(0, len(extra), 2):
        if not os.path.isfile(extra[i + 1]):
            raise FileNotFoundError(extra[i + 1])
        flash_images += [extra[i], '"%s"' % extra[i + 1]]
    merge_cmd = " ".join(
        [
            '"$PYTHONEXE"',
            '"$OBJCOPY"',
            "--chip",
            board_config.get("build.mcu", "esp32"),
            "merge_bin",
            "-o",
            merged_bin,
            "--flash_mode",
            board_config.get("build.flash_mode", "dio"),
            "--flash_freq",
            "${__get_board_f_flash(__env__)}",
            "--flash_size",
            board_config.get("upload.flash_size", "16MB"),
            *flash_images,
        ]
    )
    env.Execute(merge_cmd)


env.AddCustomTarget(
    name="mergebin",
    dependencies=firmware_bin,
    actions=merge_bin_action,
    title="Merge binary",
    description="Build combined image",
    always_build=True,
)