#!/usr/bin/env python3
"""Write the cp7 partition table used by the first-install exe.

ESP-IDF / esptool layout: 32-byte entries, then an MD5 footer, padded to 0xC00.
Offsets match partitions/crowpanel_adv7_16MB.csv. NVS is described here and is
not programmed by the installer.
"""
import hashlib
import struct
import sys
from pathlib import Path

def entry(typ, sub, off, size, name):
    raw = name.encode("ascii")
    if len(raw) > 16:
        raise SystemExit(name)
    return struct.pack("<HBBLL16sL", 0x50AA, typ, sub, off, size, raw, 0)

def table():
    raw = b"".join([
        entry(0x01, 0x02, 0x9000, 0x6000, "nvs"),
        entry(0x01, 0x01, 0xF000, 0x1000, "phy_init"),
        entry(0x00, 0x00, 0x10000, 0xA00000, "factory"),
        entry(0x01, 0x81, 0xA10000, 0x400000, "assets"),
    ])
    out = raw + b"\xEB\xEB" + (b"\xFF" * 14) + hashlib.md5(raw).digest()
    if len(out) > 0xC00:
        raise SystemExit("partition table is too long")
    return out + (b"\xFF" * (0xC00 - len(out)))

def main():
    blob = table()
    dest = Path(__file__).resolve().parent / "firmware" / "partitions.bin"
    if len(sys.argv) > 1:
        dest = Path(sys.argv[1])
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(blob)
    print("%s  %s" % (hashlib.sha256(blob).hexdigest(), dest))

if __name__ == "__main__":
    main()
