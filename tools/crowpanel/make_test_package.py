#!/usr/bin/env python3
"""Build the local test package for the unreleased CrowPanel firmware.

This does not upload anything and does not create a GitHub release.
The manifest channel is "test", which the Link app will not treat as stable.
"""
import hashlib
import json
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
VERSION = "v1.9.2-cp7.2"
IMAGE_NAME = f"hd2_macropad_{VERSION}_app_0x10000.bin"
SRC = ROOT / ".pio" / "build" / "crowpanel-adv7" / "firmware.bin"
OUT = ROOT / "tools" / "crowpanel" / "test-package"


def main() -> int:
    if not SRC.is_file():
        print(f"missing {SRC}; build with: pio run -e crowpanel-adv7", file=sys.stderr)
        return 1
    data = SRC.read_bytes()
    if data[:1] != b"\xE9":
        print("firmware.bin is not an ESP image", file=sys.stderr)
        return 1
    if data.find(VERSION.encode()) < 0:
        print(f"{VERSION} is not in the image", file=sys.stderr)
        return 1
    # App descriptor version is the 32 bytes at offset 0x30.
    app_ver = data[0x30:0x50].split(b"\x00", 1)[0].decode("ascii", "replace")
    if app_ver != VERSION:
        print(f"app descriptor version is {app_ver!r}, expected {VERSION}", file=sys.stderr)
        return 1
    digest = hashlib.sha256(data).hexdigest()
    OUT.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(SRC, OUT / IMAGE_NAME)
    manifest = {
        "schema": 1,
        "channel": "test",
        "version": VERSION,
        "product": "hd2-macropad-crowpanel7",
        "chip": "esp32s3",
        "app_offset": 65536,
        "app_slot_end": 10551296,
        "image_name": IMAGE_NAME,
        "image_sha256": digest,
        "image_size": len(data),
        "notes": (
            "Local test package for v1.9.2-cp7.2. This is not a published "
            "GitHub release. Channel is test, so Link will not treat it as "
            "stable. A board on v1.9.1-cp7.2 should show this as an upgrade. "
            "The write is app-only at 0x10000 and leaves NVS and the sound "
            "partition alone. Flash baud is at most 460800. A USB power loss "
            "during the write is not an atomic update; Link keeps a backup "
            "under Local AppData."
        ),
    }
    text = json.dumps(manifest, indent=2) + "\n"
    (OUT / "crowpanel-update.json").write_text(text, encoding="utf-8")
    (OUT / "SHA256SUMS").write_text(f"{digest}  {IMAGE_NAME}\n", encoding="utf-8")
    print(f"wrote {OUT / IMAGE_NAME} ({len(data)} bytes)")
    print(digest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
