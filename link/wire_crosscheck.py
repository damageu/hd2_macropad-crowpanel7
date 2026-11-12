#!/usr/bin/env python3
"""Cross-check Link's stub wire format against esptool 5.2.0 itself.

Runs test_romflash with HD2_WIRE_DUMP set, then replays every stub-related
request Link sent through esptool's own ESP32-S3 loader methods (mem_begin,
mem_block, mem_finish, change_baud, flash_set_parameters, read_reg,
flash_md5sum, read_flash with acks, flash_defl_begin/block/finish) with the
same arguments, and requires byte-identical packets. Also inflates Link's
FLASH_DEFL stream with Python zlib and compares it with the image.

Usage: python wire_crosscheck.py   (needs `pip install esptool==5.2.0`)
"""
import hashlib, os, struct, subprocess, sys, tempfile, zlib

import esptool
from esptool.loader import StubFlasher
from esptool.targets.esp32s3 import ESP32S3ROM, ESP32S3StubLoader

assert esptool.__version__ == "5.2.0", esptool.__version__
StubFlasher.set_stub_subdir("2")


def slip_decode_all(buf):
    out, cur, inside, esc = [], bytearray(), False, False
    for b in buf:
        if not inside:
            if b == 0xC0:
                inside = True
            continue
        if b == 0xC0:
            if cur:
                out.append(bytes(cur))
                cur = bytearray()
            continue
        if esc:
            cur.append({0xDC: 0xC0, 0xDD: 0xDB}[b]); esc = False
        elif b == 0xDB:
            esc = True
        else:
            cur.append(b)
    return out


def slip(p):
    return b"\xc0" + p.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc") + b"\xc0"


class Port:
    """Records what esptool writes and answers like a stub (or ROM)."""
    def __init__(self, flash):
        self.timeout = 3; self.baudrate = 115200; self.write_timeout = 10
        self.rx = bytearray(); self.tx = bytearray(); self.stub = False; self.flash = flash
        self.dtr = self.rts = False
    @property
    def in_waiting(self): return len(self.rx)
    def inWaiting(self): return len(self.rx)
    def read(self, n=1):
        n = min(n, len(self.rx)); d = bytes(self.rx[:n]); del self.rx[:n]; return d
    def flushInput(self): pass
    def reset_input_buffer(self): pass
    def flushOutput(self): pass
    def reset_output_buffer(self): pass
    def flush(self): pass
    def write(self, data):
        self.tx += data
        for p in slip_decode_all(data):
            if len(p) == 4:  # READ_FLASH ack
                continue
            op = p[1]; body = p[8:]
            resp = b""
            if op == 0x13:
                a, s = struct.unpack("<II", body[:8]); resp = hashlib.md5(self.flash(a, s)).digest()
            status = b"\x00\x00" if self.stub else b"\x00\x00\x00\x00"
            val = 9 if op == 0x0A else 0
            self.rx += slip(struct.pack("<BBHI", 1, op, len(resp) + len(status), val) + resp + status)
            if op == 0xD2:
                a, s, sec, _ = struct.unpack("<IIII", body)
                data = self.flash(a, s)
                for i in range(0, s, sec):
                    self.rx += slip(data[i:i + sec])
                self.rx += slip(hashlib.md5(data).digest())
        return len(data)


def take(port):
    pk = slip_decode_all(bytes(port.tx)); port.tx = bytearray(); return pk


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    with tempfile.TemporaryDirectory() as td:
        dump = os.path.join(td, "wire.txt")
        env = dict(os.environ, HD2_WIRE_DUMP=dump)
        subprocess.run([os.path.join(here, "test_romflash")], check=True, env=env, stdout=subprocess.DEVNULL)
        pkts = [bytes.fromhex(l.strip()) for l in open(dump) if l.strip()]
        image = open(dump + ".image", "rb").read()
    flash_mem = {}
    def flash(a, s):
        return bytes(flash_mem.get(a + i, 0xFF) for i in range(s))
    port = Port(flash)
    rom = ESP32S3ROM.__new__(ESP32S3ROM)
    rom._port = port; rom._trace_enabled = False; rom.secure_download_mode = False
    rom._slip_reader = esptool.loader.slip_reader(port, rom.trace); rom.cache = {"uart_no": 0}; rom.in_bootloader = True  # UART0 (CH340), not USB-OTG
    rom.sync_stub_detected = False; rom.stub_is_disabled = False
    stub = None
    i, checked, defl = 0, 0, []
    started = False
    while i < len(pkts):
        p = pkts[i]
        if len(p) < 8 or p[0] != 0:
            i += 1; continue
        op = p[1]; body = p[8:]
        if op == 0x05:
            started = True
        if not started:
            i += 1; continue
        expect = None
        if op == 0x05:
            rom.mem_begin(*struct.unpack("<IIII", body)); expect = take(port)
        elif op == 0x07:
            ln, seq = struct.unpack("<II", body[:8]); rom.mem_block(body[16:16 + ln], seq); expect = take(port)
        elif op == 0x06:
            _, entry = struct.unpack("<II", body); rom.mem_finish(entry); expect = take(port)
            port.stub = True
            stub = ESP32S3StubLoader(rom)
        elif stub is None:
            i += 1; continue
        elif op == 0x0F:
            new, old = struct.unpack("<II", body); port.baudrate = old
            stub._set_port_baudrate = lambda b: setattr(port, "baudrate", b)
            stub.change_baud(new); expect = take(port)
            assert (new, old) == (460800, 115200), (new, old)
        elif op == 0x0B:
            _, size, *_ = struct.unpack("<IIIIII", body); stub.flash_set_parameters(size); expect = take(port)
        elif op == 0x0A:
            stub.read_reg(struct.unpack("<I", body)[0]); expect = take(port)
        elif op == 0x13:
            a, s, _, _ = struct.unpack("<IIII", body); stub.flash_md5sum(a, s); expect = take(port)
        elif op == 0xD2:
            a, s, _, _ = struct.unpack("<IIII", body); stub.read_flash(a, s); expect = take(port)
            mine = [p]; j = i + 1
            while j < len(pkts) and len(pkts[j]) == 4:
                mine.append(pkts[j]); j += 1
            assert mine == expect, f"READ_FLASH/ack sequence differs at packet {i}"
            checked += len(mine); i = j; continue
        elif op == 0x10:
            size, blocks, bs, off, enc = struct.unpack("<IIIII", body)
            stub.flash_defl_begin(size, blocks * bs, off); expect = take(port); defl_size = size
        elif op == 0x11:
            ln, seq = struct.unpack("<II", body[:8]); stub.flash_defl_block(body[16:16 + ln], seq)
            expect = take(port); defl.append(body[16:16 + ln])
        elif op == 0x12:
            stub.flash_defl_finish(reboot=False); expect = take(port)
            data = zlib.decompress(b"".join(defl))
            assert len(data) == defl_size and data == image, "FLASH_DEFL stream does not inflate to the image"
            for k, b in enumerate(data):
                flash_mem[off + k] = b
            print(f"FLASH_DEFL stream inflates to the {len(data)}-byte image (Python zlib, adler32 ok)")
            defl = []
        else:
            i += 1; continue
        assert expect == [p], f"packet {i} op 0x{op:02X} differs from esptool:\n link    {p[:48].hex()}\n esptool {expect[0][:48].hex() if expect else None}"
        checked += 1; i += 1
        if op == 0x12:
            break
    assert checked > 10, checked
    print(f"wire cross-check ok: {checked} Link packets byte-identical to esptool {esptool.__version__} (stub v2)")


if __name__ == "__main__":
    main()
