"""scripts/make_input.py: a script drives the same controls on Daytona's and
srallyc's MAME ports (synthetic headers, no game data)."""

import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import make_input  # noqa: E402


def header(game, fields):
    b = b"M2IN" + struct.pack("<I", 1) + struct.pack("<H", len(game)) + game.encode()
    b += struct.pack("<H", len(fields))
    for port, mask, defv, analog in fields:
        b += struct.pack("<H", len(port)) + port.encode() + struct.pack("<IIB", mask, defv, analog)
    return b


DAYTONA = [(":ACCEL", 0xFF, 0x20, 1), (":IN0", 0x10, 0x10, 0), (":IN0", 0x20, 0x20, 0)]
SRALLYC = [(":ACCEL", 0xFF, 0x00, 1), (":IN0", 0x20, 0x20, 0), (":IN0", 0x40, 0x40, 0), (":IN2", 0xFF, 0x00, 1)]


def run(game, fields, script):
    with tempfile.TemporaryDirectory() as d:
        h, s, o = (os.path.join(d, n) for n in ("h.m2in", "s.txt", "o.m2in"))
        open(h, "wb").write(header(game, fields))
        open(s, "w").write(script)
        subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "make_input.py"), h, s, o],
                       check=True, capture_output=True)
        blob = open(o, "rb").read()
    head, _, _ = make_input.read_header(blob)
    n = len(fields)
    body = blob[len(head):]
    return [struct.unpack_from("<%dI" % n, body, i * 4 * n) for i in range(len(body) // (4 * n))]


class MakeInput(unittest.TestCase):
    SCRIPT = "frames 2\n0 start=1\n0 vr1=1\n0-1 accel=0xe0\n"

    def test_daytona(self):
        rows = run("daytona93", DAYTONA, self.SCRIPT)
        self.assertEqual(rows, [(0xE0, 0, 0), (0xE0, 0x10, 0x20)])

    def test_srallyc_ports_and_range(self):
        rows = run("srallyc", SRALLYC, self.SCRIPT + "1 handbrake=0x80\n")
        # start is IN0 0x40, vr1 the VR button IN0 0x20; accel 0xe0 -> 0xff
        self.assertEqual(rows, [(0xFF, 0, 0, 0), (0xFF, 0x20, 0x40, 0x80)])

    def test_full_range(self):
        self.assertEqual([make_input.full_range(v) for v in (0x00, 0x20, 0x80, 0xE0, 0xFF)], [0, 0, 127, 255, 255])


if __name__ == "__main__":
    unittest.main()
