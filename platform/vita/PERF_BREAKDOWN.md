# Where the Vita's time goes (Daytona USA, race, October 2026)

What each core spends per emulated frame in a Daytona USA race today, and
where the i960 and TGP time inside "game logic" goes. Written to choose the
next optimisations; numbers from three console runs, explained below.

Budget: one emulated frame is 17.38 ms (57.52 Hz). CPU 444 MHz, GPU 166 MHz,
bus 222 MHz.

## Sources

Three `perf.log` files from the same PS Vita, Daytona USA (daytona93),
`build_vita.py --fast-inaccuracy --diagnostics`, cores pinned (main
0, geometry 1, sound 2), 5 s windows. "Race windows" are the windows with
more than 800 polygons a frame; the averages below are over those.

| Run | Build | Race windows | fps |
|---|---|---|---|
| base | diagnostics only | 33 | 57.3 |
| TGP probe | `--tgp-profile` | 28 | 57.3 |
| i960+TGP probe | `--tgp-profile --i960-profile` (DAYT90093) | 12 | 53.8 |

The TGP probe costs nothing measurable. The i960 probe does: about 3 ms a
frame of its own on core 0 (see "Probe cost"), enough to drop under full
speed. Its counts (operations, bus calls, blocks) are exact; its times are
inflated. The core breakdown below is the base run.

## The three cores

### Core 0 (main): 68 % busy, 11.9 ms a frame

| Section | ms/frame |
|---|---|
| i960+TGP game logic (to vblank) | 5.94 |
| i960+TGP vblank handler | 0.46 |
| polygon recording (vertex writing into vitaGL) | 2.60 |
| 2D video update (System 24 tilemaps) | 1.07 |
| frame end (submit + swap) | 0.55 |
| polygon priority sort | 0.49 |
| geometry on the main core (buffer RAM snapshot) | 0.35 |
| sound sync, input, frame begin, other | ~0.4 |

The i960 and the TGP (6.4 ms together) are more than half of core 0. The
renderer's share (recording, sort, 2D, frame end: ~4.7 ms) is the rest.
Headroom in a race: ~5.5 ms on average, much less in the heaviest scenes
(one window peaked at 88 % busy, 15.4 ms).

In the i960 probe run the game logic is reported almost entirely in the
"vblank handler" line (9.6 ms) instead of "game logic" (0.3 ms); the sum is
what counts.

### Core 1 (geometry): ~50 % busy, 8.6 ms a frame

The display list parse (the geometrizer: transform, clip, light) of ~1,400
polygons a frame, asynchronous and pipelined: the main core waited on it in 1
join out of thousands. Not on the critical path today.

### Core 2 (sound + 2D worker): ~81 % busy, 14.2 ms a frame

| Section | ms/frame |
|---|---|
| sound board (68000 + MultiPCM + YM3438) | 11.51 |
| sample conversion / queue | 1.66 |
| 2D worker (uploads, rectangles) | 0.98 |

The busiest core after the main one, the same in all three runs. Not
limiting (it has its own core and no audio gaps in the base run), but it is
the core with the least headroom (~3 ms).

## Inside "i960+TGP": the TGP

From the TGP probe (identical behaviour to the base run):

- 1.56 ms a frame on average over the race (0.9-3.0 ms by window), 20-36 % of
  the i960+TGP lines;
- **46-54 ns per TGP instruction = 20-24 cycles** (the PC: 1.8-2 ns);
- per frame ~48,000 instructions: 28,000 data RAM reads and 12,000 writes,
  6,000 FP adds and 8,400 multiplies, 2,100 input FIFO words, 450 bank
  switches, ~300 table reads.

FP, table, bank and FIFO costs explain ~6 % of that time; **the remaining
94 % is the code around each instruction**. Likely causes, read from the
generated code (`m2tgprecomp` output, `src/runtime/tgp.h`) but not yet
confirmed in ARM assembly:

- the TGP's registers (`a, b, d, p, st`) and `data[]` are `uint32_t` fields of
  the same struct, so every data RAM store with a computed address may alias
  them: the compiler reloads and stores them around every instruction;
- every ALU op computes the status flags (`stset_fp`, `alu_update_st`),
  which a later instruction rarely reads;
- `alu_r1`, `alu_r2`, `alu_stset`, `alu_stmask` are struct fields too
  (memory, not registers);
- one-instruction `rep` loops (blocks 0x262 and 0x269, 1,872 runs a frame
  each) pay a whole block end per turn.

Hottest blocks: 0x1e8-0x217 (48 instructions, 130 runs a frame, 13 % of
the TGP's instructions), then short FIFO-reading blocks run ~500 times a
frame.

## Inside "i960+TGP": the i960

### Time

The probe's own figure (gen::run minus the board callbacks inside it and
minus the TGP) is 7.1 ms a frame, 180 ns and ~80 cycles per instruction,
but ~3 ms of that is the probe. Estimate from the base run: i960+TGP lines
6.40 ms, TGP ~2.3 ms (47,900 instructions at ~48 ns), so **the i960 is
about 4 ms a frame for ~39,000 instructions: ~105 ns, ~45 cycles per
instruction** (the PC: ~6 ns). The i960 is the largest single cost in the
game logic, ahead of the TGP.

The board callbacks run inside gen::run (vblank, timers, sound commands)
take ~1.8 ms a frame in the probe run, 40-43 calls a frame; on the Vita they
appear to be counted in other lines (gen::run is ~115 % of the i960+TGP
lines).

### Work per frame (probe, race window)

| What | Per frame |
|---|---|
| instructions | 42,900, in 7,136 basic blocks of 6.0 |
| words loaded / stored | 11,700 / 9,950 |
| **bus calls (virtual `M2Board::read_*`/`write_*`)** | **20,150** |
| of which work RAM | 9,000 (6,500 reads, 2,500 writes) |
| of which the TGP's FIFO, status and buffer | 7,700 (1,900 reads, 5,800 writes) |
| of which ROM | 2,100 |
| of which other devices | 1,240 |
| loads/stores made direct (the Vita's work-RAM pass) | 1,490 |
| integer ALU / compares / conditional branches | 12,700 / 4,400 / 4,500 |
| calls / rets (register frame save and restore) | 833 / 834 |
| switch dispatches / chunk entries | 2,925 / 1,360 |
| divides (a library call: the Cortex-A9 has no divide) | 184 |
| FP / libm | 383 / 8 |

What this says:

- **The bus is the i960's main overhead.** 93 % of its loads and stores
  still go through a virtual call. The `--fast-inaccuracy` work-RAM pass
  only rewrites accesses at a fixed address (1,490 a frame); the ~9,000
  work-RAM accesses through a register (`ld 0x10(r4)` and the like) still
  take the call, the page lookup and the fast-path checks.
- The TGP's FIFO is the second bus user (7,700 calls a frame): every word
  the game sends to the TGP is a virtual call into `M2Board::write_dword`.
- Calls and returns (~1,670 a frame) and dispatches (~4,300 a frame) are
  next; divides, FP and libm are negligible.
- **The profile is flat**: the 15 hottest blocks are 37 % of the
  instructions, mostly functions run 39 times a frame (per car or object):
  0x1100c (75 instructions), 0x11450 (55), 0x178cc (28). There is no single
  hot spot to rewrite; the gains are in the code generator.

## Probe cost

The i960 probe adds ~3 ms a frame on core 0 (i960+TGP lines 9.9 ms for
40,000 i960 and 57,000 TGP instructions, against 6.4 ms for 39,000 and
48,000 in the base run). Likely: `M2Board::prof_bus`, which classifies each
of the 20,000 bus calls through the page table that `M2_FAST_BUS` otherwise
skips. To make its times trustworthy it should classify by address ranges
first (not done yet). The TGP probe's clock reads cost 0.69 us each on the
Vita (~0.15-0.2 ms a frame around the TGP's runs, removed from its figure).
The TGP probe's micro-benchmark is not usable on the Vita (some unit costs
read 0.0 ns).

## What to do next, in order

1. **Cheaper i960 probe** (address ranges instead of the page table), then
   a new probe run for reliable cycles per i960 instruction.
2. **i960: inline work-RAM access in the generated code** for computed
   addresses: when the address falls in 0x500000-0x5fffff (and is not the
   frame-wait byte 0x500000), read or write the RAM directly, else call the
   bus. Up to ~9,000 virtual calls a frame less. Parity: the race must stay
   identical to the exact build (hash, frame dumps, WAV).
3. **The same for the TGP's FIFO and buffer** (~7,700 calls a frame).
4. **TGP code generation** (`m2tgprecomp`, bit-exact): block-local copies of
   the registers, flags computed only where read, `rep` as a C loop.
   Expected 1-2 ms a frame in the race, to be measured.
5. Later: the renderer's vertex writing (2.4-2.6 ms) and the sound board
   on core 2 (11.5 ms) if core 2 becomes the limit.

## Reproducing

```sh
python3 scripts/build_vita.py --set daytona93 --fast-inaccuracy --tgp-profile --i960-profile
```

The package is `build/vita-diag/daytona_vita.vpk` (title ID DAYT90093,
"Daytona USA (diagnostics)", installed beside the release). Play a race;
`ux0:data/daytona93/perf.log` gets one report per 5 s with the core table,
then the i960 and TGP sections. On the PC: `scripts/recompile.py
--tgp-profile --i960-profile`, then `m2run` prints the same sections.
