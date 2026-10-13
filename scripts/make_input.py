#!/usr/bin/env python3
"""Write a scripted M2IN input stream (docs/trace-format.md).

  make_input.py HEADER.m2in SCRIPT OUT.m2in

HEADER is any input stream recorded on the same machine
(M2TRACE_RECORD_INPUT); only its field layout is used, so the scripted stream
matches the machine's fields exactly. SCRIPT is a text file:

  frames N                  total frames to write
  # comment
  <from>-<to> <name>=<v>    hold a control over a frame range (inclusive)
  <at> <name>=<v>           one frame only

Names: coin, service, test, start, vr1..vr4 (1 = pressed), gear0..gear4
(1 = selected), steer, accel, brake (raw 0x20-0xe0 ADC values). Unscripted
controls rest at their recorded defaults. Later lines override earlier ones.

The header's game picks the layout: scripts/games.py's ports for that set
(srallyc: start is IN0 0x40, every vrN its one VR button, handbrake IN2),
and, for a game whose analog inputs are 0x00-0xff ("full_range"),
steer/accel/brake rescaled from 0x20-0xe0 as the runtime does (the game's
Game::io): one script drives every game.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import games  # noqa: E402

ANALOG = {"steer", "accel", "brake"}


def game_entry(game):
    """The scripts/games.py set a header's game name is (or begins with)."""
    if game in games.GAMES:
        return game
    return next((g for g in sorted(games.GAMES, key=len, reverse=True) if game.startswith(g)), game)


def full_range(v):
    """The common ADC range 0x20-0xe0 to 0x00-0xff (src/games/srallyc.cpp)."""
    return max(0, min(255, (v - 0x20) * 255 // 0xC0))


def read_header(blob):
    assert blob[:4] == b"M2IN" and struct.unpack_from("<I", blob, 4)[0] == 1, "not an M2IN v1 stream"
    p = 8
    n = struct.unpack_from("<H", blob, p)[0]
    game = blob[p + 2:p + 2 + n].decode()
    p += 2 + n
    nf = struct.unpack_from("<H", blob, p)[0]
    p += 2
    fields = []
    for _ in range(nf):
        ln = struct.unpack_from("<H", blob, p)[0]
        port = blob[p + 2:p + 2 + ln].decode()
        p += 2 + ln
        mask, defv, analog = struct.unpack_from("<IIB", blob, p)
        p += 9
        fields.append((port, mask, defv, bool(analog)))
    return blob[:p], game, fields


def main():
    header_path, script_path, out_path = sys.argv[1:4]
    header, game, fields = read_header(open(header_path, "rb").read())
    game = game_entry(game)
    controls = games.controls(game)
    rescale = games.GAMES.get(game, {}).get("full_range", False)
    index = {(port, mask): i for i, (port, mask, _, _) in enumerate(fields)}

    total = 0
    events = []  # (from, to, field index, raw value)
    for lineno, line in enumerate(open(script_path), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if parts[0] == "frames":
            total = int(parts[1])
            continue
        rng, assign = parts
        lo, _, hi = rng.partition("-")
        lo, hi = int(lo), int(hi or lo)
        name, _, val = assign.partition("=")
        if name not in controls:
            sys.exit(f"{script_path}:{lineno}: unknown control {name}")
        port, mask = controls[name]
        fi = index[(port, mask)]
        _, _, defv, analog = fields[fi]
        v = int(val, 0)
        if analog:
            raw = (full_range(v) if rescale and name in ANALOG else v) & mask
        else:  # digital: pressed flips the field away from its default
            raw = (defv ^ mask) if v else defv
        events.append((lo, hi, fi, raw))

    if total <= 0:
        sys.exit("script needs 'frames N'")
    frames = []
    for f in range(total):
        row = [defv for (_, _, defv, _) in fields]
        for lo, hi, fi, raw in events:
            if lo <= f <= hi:
                row[fi] = raw
        frames.append(struct.pack("<%dI" % len(row), *row))
    with open(out_path, "wb") as out:
        out.write(header)
        out.write(b"".join(frames))
    print(f"{out_path}: {total} frames, {len(fields)} fields, {len(events)} scripted ranges")


if __name__ == "__main__":
    main()
