# Handoff

## README: compatible sets by game; a Limits section (2026-10-19)

README.md lists each game's compatible MAME set (version, board, the
archive in roms/, the file on the Vita: from scripts/games.py and
HOW_TO_COMPILE_RELEASE.md) and has a Limits section: only the original
Model 2 and Model 2A boards run on the Vita; Model 2B (SHARC) and 2C
(TGPx4, MB86235) are too heavy with the current code; Fighting Vipers is a
2B test (~26 fps in a fight); 2C is not supported (its coprocessor is not
recompiled; expected to hit the same wall, not measured).

## Tidy: traces in one folder per ROM set (2026-10-19)

scripts/m2_check.sh writes and reads traces/<set>/: hdr.m2in (the input
stream header, was traces/hdr_<set>.m2in; daytona93's traces/hdr.m2in) and
check_<scenario> with its .irq/.geo/.snd/.tgp/.m2in (was traces/check_<set>_
<scenario>; a scenario named after its set loses the prefix: fvipers_fight
-> traces/fvipers/check_fight). The local traces were moved so (fvipers'
check_attract, check_fight and the SHARC logs sharc_*.sharc/.sharcpc;
hdr.m2in for manxttc, srallyc, vcop, vcop2, vf2): commands in older entries
name the old paths. DIAG02-validation.txt (a September validation note)
moved from the root to platform/vita/docs/. .vscode/settings.json: the
extern/ clones read for ideas (PS2Recomp, PCSX_AwO, N64Recomp, rexglue-sdk)
in git.ignoredRepositories.

## Vita: the polygon sort moved to the geometry thread (2026-10-19)

Core 0's "polygon priority sort" (VF2 fight, 17 Oct log: 0.66 ms of 17.0,
~4 %) now runs in rt::Geo's job on core 1, right after GeoCore::parse while
the list is in that core's cache: render/polygon_order.h polygon_depth (the
renderer's former code: PolygonOrder, arcade or hybrid depth, z_band_key,
kZDepthBands moved there) fills job_depth_, published with the list
(front_depth_ swapped with it, no copy). The renderer takes
rt::Geo::depth_of(list, option) and sorts itself only when that is null
(Sync mode, another list, the frame the POLYGON ORDER option changes: the
job records the option it sorted for; gl_set_upgrade_z_depth also sets
Geo::set_depth_hybrid). Cross-core traffic: ~6 KB of depths a frame
(~1,500 floats), less than the main core's former reads of the polygons'
window and depth. Vita compiler syntax check of the three files; release
VPKs built for Daytona and VF2; not yet measured on the console (expected:
"polygon priority sort" near 0 on core 0, core 1's parse ~0.7 ms longer).
Other independent candidates for leaving core 0 (VF2, same log): the sound
wait (2.9 ms: two sound frames in flight instead of one), the perf.log
formatting (diagnostics; a 34 ms frame every report). The buffer RAM
snapshot (0.35 ms) cannot become a buffer swap: buffer RAM holds data the
i960/TGP read back across frames (a dirty-block copy could halve it).
Vertex writing (2.1 ms) depends on the main core's textures and palette.

## SHARC on the Vita stopped: Fighting Vipers stays a test; lighter run boundaries (2026-10-19)

Decision (user): no more SHARC work for the Vita; it will not reach speed
there as it is. README: Fighting Vipers is a test of the Model 2B board,
not playable on the Vita (~26 fps in a fight, the SHARC ~20 ms of a 17.4 ms
frame on core 0).
Last change, kept (generic, exact): m2sharcrecomp's plain-run boundaries (a
run's end, a pinned branch taken, a counter loop looping back) no longer
run step_pre nor update m_instructions: nothing step_pre tests can change
between full steps inside a run_generated call (interrupts, idle, the IOP
delay, DMA only move through full steps or from outside, which does not run
during the call); they add to done and test the budget. run_generated sets
m_instructions = start + done on exit; the polling exit counts its
instruction (it had been lost: 613,709,678 instead of 679). ~30 ARM
instructions fewer a boundary, ~150 boundaries per 1000 SHARC instructions.
Checks: m2sharccheck_gen MATCH, fight 613,709,679 and attract 459,076
instructions; m2native attract MATCH (596 of 596 screens); fvipers_fight
6,000 frames, last screen 2fcd2223eeb056c0 as before. Not measured on the
Vita.
Registers as locals (the N64Recomp/ReXGlue technique), analysed and not
done: in the hot chunk's Vita code (marked, -O3), of 3,899 loads 18 re-read
a value stored in the same straight-line stretch and 1 a value already
read; of 1,954 stores 177 (9 %, mostly ASTAT) are overwritten before a
read. GCC already keeps the SHARC's registers in ARM registers within a
run; locals would save a few per cent. A distinct type for internal memory
(TBAA) gave byte-identical code. The direct board accesses (M2_FAST_SHARC)
measured on the Vita: no change beyond session noise (22.5 -> 22.7 ms at
~130 k instructions).
Cleanup: the flag analysis (m2sharcrecomp's probe_flags, keep[], the flag
statistics) and its probe API (adsp21062_device::probe, ProbeState) removed:
measurement code for a transformation ruled out; m2sharcrecomp no longer
links the sharc library. The generated code is byte-identical.
Read for ideas: extern/N64Recomp (MIT; RSPRecomp: GPRs and the vector state
as locals, delay slots duplicated, goto branches), extern/rexglue-sdk
(BSD-3; *_as_local options per register family, skip_lr/skip_msr).

## Vita menu: input settings on Controls, Options by group, resume latency (2026-10-19)

Controls holds every input setting (dead zone, the wheel's invert, curve and
smoothing, the bindings, hold Test button, a reset of them all); Options is
ordered by group under headings (game, display, audio, performance, link
play). Returning from the Start+Select menu blanked the whole pad, sticks too,
until every button was released: now only the buttons still held are
ignored, each until released (frontend/main.cpp held_from_menu). Details in
platform/vita/HANDOFF.md and README.md. Not yet checked on the console.

## SHARC: branches in plain runs, full steps only where needed, as on the TGP; --sharc-profile (PC +46 %, Vita +51 %) (2026-10-19)

Vita, three versions of the recompiled SHARC in one diagnostics package,
alternated a perf window each (same session; extern/result_perf_vita/
fvipers/perf_three_versions.log): specialised (MAME's handlers templated on
each opcode, step pieces inline) 3.22 M SHARC instructions/s, 40 ms a frame;
compact (shared handlers, step pieces called; a few dozen bytes an
instruction) 2.37 M, 55 ms. The instruction cache was not the limit (the
previous entry's analysis was wrong): the Vita is ~40x the PC uniformly,
~140 cycles a SHARC instruction. Only the specialised version is kept
(compact, mixed, set_variant, the *_ool step pieces, --variant and the Vita's
alternation removed).
Where the cycles go (arm-vita-eabi-g++ -O3 of the 40 hottest instructions of
the fight, 35 % of the work; m2sharcprof's per-address counts): a handler is
2-47 ARM instructions (~15 on average); the full step (step_fetch, step_after,
step_pre) ~45 on its fast path, ~35 of them loads/stores (pipeline and ASTAT
history shifts, the opcode, the latency/DMA/IOP/interrupt tests, a 64-bit
count). 57.5 % of executed instructions ran a full step, ~10 % by nature.
The TGP's recompiler already had none of this (constant pc, no per-
instruction checks, repeats as native loops; ~30 cycles an instruction on
the Vita): the SHARC's is now aligned on it, generically (decided from the
opcodes and MAME's handler sources, no per-game profile):
- Delay slots: the two after a branch only when it is delayed (J bit, opcode
  bit 26; the generator finds the handlers calling CHANGE_PC_DELAYED in
  sharcops.hxx and refuses one not reading op_get_jump_j).
- ASTAT history: the three before the end of a loop on a condition (do until,
  loop_type 0) stay plain and only shift it; counter loops never read it.
- Counter loop ends (types 1-3) plain, step_loop_end's counter part inline;
  the run ends where it loops back, with a direct goto to the top when the
  loop has one do-until in the same chunk.
- What no opcode tells (a MODE1 write's 2-instruction latency, from a
  register write, sysreg_bitop or a status stack pop; a non-linear pipeline,
  e.g. a delay slot entered across chunks) is checked where a plain
  instruction is entered after a full step or a dispatch (plain_ready); the
  interpreter's steps (step_body, shared with step()) run until it is gone:
  54,761 times, 88,489 instructions of 613.7 M in the fight check.
- Full steps fetch with step_fetch_gen: the opcode only for the trace (the
  handlers have it as a template parameter).
- Branches "pinned" in plain runs (m2tgprecomp's direct gotos): direct,
  relative and indirect jumps and calls (and their compute forms), rts and
  the do-untils run MAME's handler on the pipeline a linear arrival leaves
  (four constants), inside the run; taken (daddr moved), the count, budget
  and step_pre as at a run's end, then a goto to a constant target in the
  chunk, else the dispatch. A delayed branch keeps its two delay slots in the
  run when they are plain, in its chunk and no loop end (to a fixed point;
  else the three are full steps as before): `pending` holds the target
  CHANGE_PC_DELAYED left in nfaddr, taken after the second slot with the
  pipeline MAME's fetch of the target finds (pc = second slot, daddr = target).
  Not pinned: a jump with CI (status stack pop, MODE1 latency,
  interrupt_active), rti, a branch at a loop end. The generator refuses a
  pinned handler whose upstream body touches SET_UREG, idle, imask,
  irq_pending, the latency, DMA or the IOP, or interrupt_active without CI.
Executed instructions in plain runs: 95.9 % (42.5 % this morning; 15.9 %
pinned branches, 5.1 % shifting the ASTAT history); full steps: 2.1 %
control flow (CI jumps, rti, system register writes), 2.0 % ends of loops on
a condition (m2sharcrecomp prints this with M2SHARC_PROFILE_FILE=<m2sharcprof
output>). Step by step on the PC fight check (M SHARC instructions/s): 145.6;
narrower full steps 182.5; counter loop ends plain 186-190; branches pinned
209-216.
Checks after each step: m2sharccheck_gen MATCH on fvipers_fight (613,709,679
instructions, the same count each time) and the attract (459,076); fvipers
ctest 23/23. The per-instruction log (SHARC.pc) cannot follow the recompiled
code since plain runs (no trace hook in them): the event log is the check.
Vita (user, diagnostics, fast-inaccuracy; before the pinned branches; against
perf_plain_runs.log, another session): fight 3.22 -> 3.77 M instructions/s,
40 -> 34.4 ms a frame of SHARC for ~130 k instructions, 18.2-18.6 -> 20.1-
20.7 fps; ~75 k-instruction windows 2.76 -> 3.25. Still ~118 cycles a SHARC
instruction for ~25 ARM instructions estimated: stalls (memory, caches)
dominate, not measured yet. Full speed needs the SHARC on its own core (core
1: geometry, a few %) and ~2x more speed (17.4 ms a frame for ~130 k
instructions: ~7.7 M/s). An approximate SHARC (fast-math, skipped flags,
per-block counts) would gain a few % at most: the cost is the instructions
and their bookkeeping, not their precision; replacing the program by native
geometry would be per game (not wanted: the recompiler stays generic).
Flag liveness, measured and not applied: 64 % of the flag writes of plain
instructions are dead statically but only 1.14 dead writes an executed
instruction (1.78 written): a few per cent at best (the generator still
prints the statistics; ProbeState/probe run a handler on a chosen state).
Vita with the pinned branches (user, perf.log, same scenes): fight 4.81-4.93
M SHARC instructions/s (3.77 before them, 3.22 this morning), ~27 ms a frame
of SHARC for ~130 k instructions, 24.1-24.6 fps (20.1-20.7, 18.4).
SHARC profiling, --sharc-profile (build_vita.py, recompile.py; CMake
M2_SHARC_PROFILE / DAYTONA_VITA_SHARC_PROFILE; src/runtime/sharc/
sharc_profile.h): the same generated code (probes are macros: M2SHARC_EV,
M2SHARC_SIZE, M2SHARC_EXIT, M2SHARC_COUNT; sharc_gen.cpp also gets each
instruction's handler name and kind under the macro). perf.log and m2run get:
time by caller (run to the wait, need an output word, 256-instruction slice),
run_generated calls by size and exit, dispatches, run ends, branches taken,
loop backs, full and interpreter steps per 1000 instructions, the data space
by region, program memory data (and writes into the code's range: the
rewrite check reads twice), the board by kind, the handlers' shares, the hot
set and the 16 hottest instructions. PerfLog's report buffer 12 -> 24 KB.
First PC run (fvipers_fight, 6,000 frames, 101.8 k instructions a frame):
99 % of the SHARC's instructions run from need_output, ~500 calls a frame
making 4,490 run_generated calls of ~21 instructions (budget 16: 4,185
budget exits a frame); 110 dispatches, 55 branches taken, 58 loop backs,
47 full steps per 1000 instructions; memory per 1000: internal r 164 w 143,
board r 55 w 32, PM data r 24 w 39, none into the code's range; the hottest
instruction is the input wait loop's nop (0x20133, full: a one-nop do-until).
extern/PS2Recomp (GPL: ideas only) read for ideas: delay slots inlined in
the branch's path (done here as pinned branches), budget checks on backward
edges only (not done: the SHARC's budget slices it against the i960), an
inline RAM fast path (not done; it gave nothing on the i960).
Vita profile (user, --i960-profile --sharc-profile, fight, 20 fps, a frame
50.7 ms): vblank handler 41.4 ms, of which SHARC 32.4 ms for 130.7 k
instructions (248 ns, 110 cycles each, counting builds); need_output 574
calls making 5,712 run_generated calls (21 instructions each), slices 384
calls of ~2 instructions at ~2.4 us (two clock reads of 0.73 us included):
~1 us of bookkeeping a run_generated call, ~6 ms a frame. The ARM code of
the hottest chunk (vitasdk g++ -O3, a marker a SHARC instruction) showed
why the Vita is ~50x the PC: GCC called most templated handlers (203 bl to
sharcop_*_t in one chunk) and IF_CONDITION_CODE, GET_UREG_i/SET_UREG_i,
FMUL/FADD, compute_*; each call saves registers and reloads m_core.
Changes (generic):
- Every templated handler, COMPUTE_t, compute.hxx's member functions and
  sharcops.hxx's inline helpers copied by the generator are SHARC_INLINE
  (always_inline); the handlers' dm_read32/dm_write32 become dm_read32_i/
  dm_write32_i (sharc_step.h): internal memory (0x20000-0x3ffff) inline,
  the rest through data_space. The hot chunk: 0 handler calls (203), twice
  the ARM code (10,171 instructions against 5,769), 9.4 s to compile.
- need_output: one run_generated call with a 1 M budget, ended by the output
  FIFO's write (TgpBoard::stop_on_output_ -> adsp21062_device::end_run:
  m_budget = 0, which the chunks now read instead of a parameter): the run
  returns at its next check (a plain run's end, a branch taken, a full step)
  instead of after 16-instruction calls. run_sharc_slice skips while the
  SHARC waits on an empty input FIFO (sharc_starved_, cleared by FIFO and
  IOP writes and the boot): its polls are not run (not counted).
PC: fight check 209-216 -> 300-320 M SHARC instructions/s (inlining; MATCH,
613,709,679 instructions; attract MATCH); m2native attract MATCH (596 of
596 screens, 5,631 SHARC words); fvipers_fight 6,000 frames native, 184
fps; profile: run_generated 6,100 -> 675 calls a frame (150 instructions a
call), slices 385 -> 5; ctest 23/23. Vita: not measured yet.
Vita (user, same profiled build, fight): SHARC 32.4 -> 22.5 ms a frame (110
-> 75 cycles an instruction), run_generated 6,100 -> 717 calls a frame,
slices 384 -> 3, vblank handler 41.4 -> 31.2 ms, a frame 50.7 -> 38.7 ms,
20 -> 26.0-27.4 fps. The doubled code did not hurt the instruction cache.
Compared with the TGP (m2tgprecomp/tgp.h, VF2's tgp_gen.cpp compiled for the
Vita: no handler calls, only Tgp::io_r/io_w), what the SHARC still lacked:
- M2_FAST_TGP's direct FIFOs: now M2_FAST_SHARC (fast_paths.h; the Vita
  defines it; needs M2_FAST_TGP): the recompiled code's board accesses
  (dm_read32_i / dm_write32_i above 0x400000: input FIFO with FLAG0, output
  FIFO with need_output's end_run, buffer RAM, the copro data ROM) go to
  TgpBoard's WordFifos and arrays through pointers SharcCopro sets
  (adsp21062_device::set_direct_board) instead of data_space and the Bus's
  virtual call; m2sharccheck sets none (its log bus). WordFifo moved to
  src/runtime/tgp/word_fifo.h. Profile: ~11,400 such accesses a frame.
  Also, always: the handlers' pm_read32/pm_write32 inline (pm_data32: the
  internal word program_space and pm_r/pm_w reach with the 32-bit mask;
  writes into the code's range still go through the rewrite check).
  Checks: m2sharccheck_gen MATCH (fight, attract; same counts); a PC build
  with -DM2_FAST_TGP=1 -DM2_FAST_SHARC=1: m2native attract MATCH (596 of
  596 screens), fvipers_fight 6,000 frames identical to the build without
  it (291,664,897 i960 instructions, last screen 2fcd2223eeb056c0).
- One function for the whole program: chunks stay at 256 instructions
  (m2sharcrecomp --chunk, a power of two). Bigger chunks cut the chunk
  calls (4,116 a frame at 256, 2,300 at 1024, 1,279 at 2048) but the PC
  fight check got slower (alternated on a quiet machine: 256 309-317 M
  instructions/s, 1024 280-286; 2048 269-273), the Vita's compiler takes
  133 s for a 1024 chunk and 183 s / 800 MB for a 2048 one (desktop build
  14 s -> 43 s -> 5 min 47 s).
Read for ideas, extern/PCSX_AwO (GPL v2, ideas only; PCSX ReARMed for the
Vita: a MIPS->ARM JIT, NEON GTE): its JIT keeps guest registers in host
registers across a block (ours reload m_core->r[] after every store: a
uint32_t store may alias the int32_t registers); -marm (tried in thought
only: Thumb-2 is the Vita compiler's default, ARM code ~25 % bigger; the
user dropped it); no unwind tables (our handlers throw on stack faults).
Next: the Vita profile of the direct board; the SHARC on its own core; the
registers kept in host registers across a plain run; the TGP's flag
liveness.

## SHARC: plain runs and inline register accessors (PC x1.9; Vita unchanged) (2026-10-19)

m2sharcrecomp: plain instructions (compute, the dm/pm moves, immediate loads,
modify, nop: handlers that touch neither the pipeline, the stacks, loops,
interrupts nor system registers; a ureg write to class 6x/7x is not plain)
run without the interpreter's per-instruction bookkeeping: the pipeline is
written once when the run ends (arrival is always linear), their count kept
in a register; the full step for every other instruction, the two after one
(delay slots, MODE1 latency) and the three before a loop end (ASTAT history).
1,997 of 4,351 instructions. step_pre's checks are skipped inside a run:
run_generated refuses a pending DMA or delayed IOP write (never after the boot
in fvipers' logs). --full-steps: the old code (comparison, debugging).
GET_UREG, SET_UREG, SHIFT_OPERATION_IMM copied inline (GET_UREG_i...) into
sharc_handlers.h: constant register numbers fold. The SHARC builds in 15 s.
PC, fight check, on a quiet machine (two MAME runs left over from screenshot
probes had been taking 2.7 cores for 5 h: earlier timings were noisy): full
steps 103 M SHARC instructions/s, plain runs 143, + inline accessors 156;
m2sharccheck_gen MATCH (attract, fight); native fight 159 fps (91-100), the
same last screen hash. No integer division in the SHARC's hot path (the Vita
has no hardware divider): only the boot's external_dma_write uses %.
Vita perf.log: a SHARC line (instructions, ms a frame, M instructions/s;
TgpBoard::set_profile_clock, diagnostics builds). Measured (user,
extern/result_perf_vita/fvipers/perf_plain_runs.log): fights 18-29 fps; the
SHARC 63-77 k instructions a frame, 23-28 ms, 2.74-2.76 M instructions/s:
unchanged by these optimisations (~160 cycles a SHARC instruction at 444 MHz).
Analysis: not the per-instruction work; the generated SHARC is 1.26 MB of ARM
code (~290 bytes an instruction, every instruction its own copy of MAME's
handlers) against the Cortex-A9's 32 KB instruction cache. Next: a SHARC
benchmark on the Vita (code variants in one package, measured on the console)
and a per-address profile (the hot set's size), then compact code with only
the hottest loops specialised.

## Fighting Vipers runs natively (PC 90+ fps; Vita 13-20 fps in a fight) (2026-10-19)

The SHARC on the board: TgpBoard holds it on Model 2B (SharcCopro, its data
space: the input FIFO, an empty read is 0 as MAME's; the output FIFO; buffer
RAM; the copro data ROM; FLAG0 follows the input FIFO, FLAG1 stays 0: MAME
sets it to the input FIFO's fullness at 16 words, ours has no limit), the
upload through external_dma_write, the IOP port, the copro status (-1 until
the upload's first word). M2Board: 2B's map (texture RAM 0x11000000/0x11200000
as plain RAM, its pages' generations kept by ram_written for the Vita's
texture cache; luma RAM 0x11400000 in 2A's layout; the UART at 0x009c0000,
the i8251 status/data code shared with 2A's; the IOP port; 0x980014).
Run behind the i960 (TgpBoard::need_output): the i960 needing an output word
runs the SHARC only until that word is there (MAME's SHARC runs in slices
behind the i960, which stalls on an empty FIFO for one word); no catch-up
before i960 buffer writes; a buffer RAM read runs a 256-instruction slice;
vblank runs it to the end. Found in the fight: run to the end, the SHARC
processed commands the i960 had pushed before clearing its work area (word
0x7c5f: written by the SHARC, cleared by the i960, read back 3,000 input words
later; in MAME the clear comes first). Wrong turn: no catch-up on buffer writes
alone did not change it (the SHARC had run to the end on a FIFO read).
Parity: attract 600 frames m2native MATCH with everything native (5,631 SHARC
output words, 597 geometrizer frames, 596 screens identical). The fight
diverges at 136 M i960 instructions on a SHARC word off by one bit (3ed15076,
MAME 3ed15077): the same race, MAME's exact interleaving (its scheduler's
1/18,000 s slices) not reproduced; counted as measured over the whole fight,
10,650 words differ and the i960 (fed MAME's) and our SHARC drift apart. Not
fixed: a timing model of MAME's slices would be needed. Native runs are self-
consistent: fvipers_fight 6,000 frames on the PC (a whole fight, Grace vs
Bahn, rendered right), 91-100 fps.
M2_SPIN_SKIP checked (fvipers_fight, ON against OFF): 291.7 M i960
instructions, 12,852 interrupts, last screen, 60/60 dumps and the WAV
identical; 14.9 M instructions skipped.
Seeds: 1,213 (from 345): seed_scan.py (now also a bare ret as a handler:
0x6d0c0, after data in a two-entry data ROM table), seeds/fvipers_inline.txt
(0x4e6f0, bx 0x8(r14): two words of arguments after its 53 balx), 136 stubs
(lda/shlo/mov imm,g0; b T to a shared T: 0x1eb8c 92, as VF2's script stubs),
MAME harvests of a 9,000-frame attract and two test-mode walks
(scripts/inputs/fvipers_test.txt, fvipers_test2.txt; built from MAME
screenshots of each item: Service moves from EXIT and wraps, Test enters and
leaves; the memory test outlasts the first walk's wait, the second covers what
it skipped): 53 and 42 new targets. Sound: every address its 68000 ran in the
walks already recompiled. games.py: fvipers' joystick as up/down/left/right
(IN1), vita_fast fast_inaccuracy.
--fast-inaccuracy checked on the PC: fvipers_fight 6,000 frames, both
test walks (13,730 and 6,730 frames) and 9,000 frames of attract run through.
SHARC recompiled in files: sharc_gen.cpp (the core, compiled once, and the
dispatch), sharc_handlers.h (sharcops.hxx's macros and 9 inline helpers,
compute.hxx without its two lookup tables, the templated handlers), one
sharc_chunk_NNN.cpp per 256 instructions (17); sharc_step.h (CHANGE_PC and
the step pieces, inline). The single file took over 20 min with the Vita's
compiler (-O3, one core; 1 min 45 s on the PC); now 41 s on the PC, all
cores; and 83.2 M SHARC instructions/s on the fight check (62.5 before):
m2sharccheck_gen MATCH on the attract (instruction log) and the fight.
Vita (user, diagnostics VPK, fast-inaccuracy; extern/result_perf_vita/
fvipers/): no fault. 50 fps on the warning screen, 20-22 in the first
fights, 13 in heavier scenes. Core 0 at 100 % in attract and fights: the
vblank handler 41-43 ms a frame (4 ms on the warning screen): the SHARC,
~102 k instructions a frame, at ~2.7 M a second (30x slower than this PC).
Core 2 (sound) saturates only while loading (SCSP DSP 16 ms a frame: no
recompiled DSP program for fvipers yet); 52-58 % in a fight. Core 1 4-12 %.
Next: the SHARC on core 0: why 30x (ARM), its own core, less per-instruction
overhead; then fvipers' DSP program (m2dsprecomp) for the loading.

## The SHARC recompiled: m2sharcrecomp, identical to MAME (2026-10-19)

tools/m2sharcrecomp SHARC_PROGRAM.bin OUT.cpp [--ops sharcops_table.inc]
[--sharcops sharcops.hxx], run by recompile.py when the set has a SHARC
program (gen/<set>_sharc/sharc_gen.cpp; CMake SHARC_GEN, m2sharccheck_gen).
- Every instruction of the upload is recompiled (no reachability analysis):
  per instruction the interpreter's step split into shared pieces
  (src/runtime/sharc/sharc.cpp step_pre, step_fetch, step_loop_end, step_after;
  step() uses the same pieces, always_inline) and the handler called on a
  constant opcode. step_loop_end only at do-until targets (76; laddr is only
  ever a do-until's target); a one-nop do-until loop that loops again (2:
  the SHARC polling its flags) returns "waiting".
- The handlers: the generator copies every sharcop_* of sharcops.hxx and
  COMPUTE into the output as templates on the opcode (m_core->opcode and
  COMPUTE's parameter replaced by the template parameter, COMPUTE(x) calls
  by COMPUTE_t<x>()), at each run from the file as it is: MAME's code word
  for word, its decoding folded per instruction (sharc.h declares the 45
  templates).
- The output includes sharc.cpp (the core's other build: link it instead of
  the `sharc` library), compares program memory with its program on the
  first run, and makes a later write to its code fatal (pm write guard).
- 256-instruction chunks, one function each: one function for the program
  took 9 min 14 s to compile (-O2), the chunks 1 min 45 s, one file.
Checks (m2sharccheck_gen; FLAG0 now updated by the checker's bus after each
event, as the recompiled code runs many instructions a call): attract MATCH
with the instruction log (458,948 lines); fvipers_fight MATCH (613.7 M
instructions, 50.1 M events). Speed, fight, with the checker's bus: the
interpreter 24.8 M instructions/s, recompiled with plain handler calls 44.5,
helpers inlined and DMA calls only while a DMA runs 50.2, templated handlers
63.3, chunked 62.5 (x2.5). The game needs ~102 k SHARC instructions a frame
(5.9 M/s; the i960 runs ~107 k): the Vita, ~10-15x slower a core, is at the
edge.
Executed instructions by family in the fight (a temporary probe, removed):
compute 20.7 %, compute+ureg dm/pm postmod 13.0, ureg to dm indirect 8.4,
compute+ureg premod 8.3, direct jump 6.4, imm to ureg 6.1, imm shift 3.9,
compute dm to dreg immmod 3.9, relative jump 3.5, compute ureg to ureg 3.3.
Next: the SHARC on the board (M2Board's copro on 2B: the FIFOs, buffer RAM,
copro data ROM, FLAG0/FLAG1 as MAME sets them, run on demand as the TGP),
the 2B video, the game on the PC, the spin check; then the Vita's speed
(GET_UREG/SET_UREG and the conditions as templates too, the chunks spread
over several files to compile in parallel).

## build_vita.py --game all (2026-10-19)

Builds, with the same options, every game whose main set's archive is in
roms/ (games.rom_archives: .zip or .7z under any of the set's names) and whose
Vita package works (GAMES "vita", default True; fightingvipers False until its
SHARC runs on the board). A game whose host code is missing is recompiled
first (recompile.py --game G --recomp-only); one the requested --fast-* mode
was not checked on (SETS vita_fast) is skipped; a failure does not stop the
others; a summary lists each game's package or why not, exit 1 if any failed.
--set, --title-id, --build-dir and --revision-a-self are refused with all (one
game's package). tests/scripts/test_build_vita.py: 31 tests pass (3 new).
Not run end to end here (each game's own build is the one already used).

## Fighting Vipers (fvipers; Model 2B-CRX): the i960 side matches MAME (2026-10-19)

First Model 2B game (the user's choice: no Virtua Fighter runs on 2B; MAME
marks fvipers NOT_WORKING but draws its attract right under -nodrc). Plan and
board facts: design doc, "Model 2B (Fighting Vipers): the SHARC copro".
Done (step 1 of the plan):
- rt::BoardType::Model2b (crx() covers 2A and 2B); src/games/fvipers.cpp,
  scripts/games.py (game fightingvipers, Vita FVIP00099; vita_fast empty),
  platform/vita/logo_livearea/fightingvipers/. Game::mirrors (main_data's
  two ROM_COPY blocks above 16 MB) and Game::sharc; m2import writes
  sharc_program.bin (13,053 16-bit words at main_data 0x100000c, CRC
  0x92DCEC90, equal to every word MAME's FIFO took in boot mode) and an empty
  tgp_program.bin (m2tgprecomp now accepts an empty program: every call is
  "control outside the recompiled program"). Our program.bin and 32 MB
  main_data are byte-identical to MAME's regions (Lua dump).
- Trace plugin and replay bus: 2B's own ranges, only when the machine has a
  SHARC (WRITE_TAPS_2B: 0x980010-0x98003f, the IOP port 0x8c0000, the UART
  0x9c0000; docs/trace-format.md), its RAM (texture 0x11000000/0x11200000,
  luma 0x11400000 as plain RAM), and no BURST on the geometrizer program port
  and the function port (MAME maps them without it on 2B: a stq writes every
  word at 0x804000; found as a divergence 91 M instructions into the fight).
  m2native runs M1 on 2B (copro, buffer RAM, geometrizer replayed);
  m2_check.sh passes -nodrc to MAME for a model2b set.
- Seeds: seeds/fvipers.txt (345, MAME harvest of attract and
  scripts/inputs/fvipers_fight.txt), seeds/fvipers_snd.txt (five "jmp
  (d16,pc)" tables, 145 entries; 22 of their entries were the only seeds the
  2,042 addresses MAME's 68000 ran needed, found by adding the lowest
  uncovered executed address until none was left).
- Checks: attract 600 frames m2native and m2replay MATCH (73.8 M
  instructions, 6.95 M device events, 1,320 interrupts); fvipers_fight 6,000
  frames m2native MATCH (641.1 M instructions, 43.7 M device events, 12,286
  interrupts); m2sndcheck MATCH (72.0 M 68000 instructions, 206,179
  checkpoints, 188,636 interrupts). Stream header traces/hdr_fvipers.m2in.
- Wait loop: 0x118dc ldob 0x500000,r3 / cmpibe r3,g0 (as Daytona's 0x1394),
  where MAME takes 1,887 of 2,992 vblank interrupts in 3,000 frames of
  attract: Game::idle and a ByteIsReg spin entry. NOT checked with
  M2_SPIN_SKIP OFF against ON: the board cannot run the game before the SHARC.
  0x4c5bc-0x4c8ac (also often interrupted) are decompression loops.
- MAME: its SHARC DRC (the default) runs fvipers at 15.6 % here, -nodrc at
  152 %. A Lua screen:snapshot() works under -video none; video:snapshot()
  wrote nothing. cpu.state["IP"] is not the i960's state name (use "ip"), and
  an error in a tap callback is silent.
The SHARC program: 4,351 instructions, 27 of the 43 opcode groups of MAME's
table (788 compute, 473 ureg to dm indirect, 423 compute with dm/pm premod,
353 relative jumps, 343 immediate to ureg, ... 112 rti, 73 do-until loops);
some may be data, reachability will say. Packing: MSWF 0, an instruction is
w0 | w1 << 16 | w2 << 32.
Step 2, started: the SHARC's reference, checked against MAME.
- patches/mame/0005-sharc-log.patch: $M2TRACE_SHARCLOG (IOP writes, the
  upload, every FIFO pop/push and buffer RAM access the SHARC makes, in
  order) and $M2TRACE_SHARCPC (PC, opcode, ASTAT, MODE1, R0-R15 before each
  interpreter instruction; a run at one address folded into "rep N").
- src/runtime/sharc/: MAME's SHARC interpreter lifted (BSD-3; THIRD_PARTY.md):
  sharcops.hxx, compute.hxx, sharcdma.hxx, sharcinternal.ipp byte-identical
  to upstream, sharcops_table.inc without its includes, sharc.h/sharc.cpp
  without the device framework (our program/data spaces, rt::sharc::Bus for
  the board's map, step() = one pass of the interpreter loop), sharc_shim.h.
  CMake library `sharc` (-w: upstream's warnings).
- tools/m2sharccheck IMAGES SHARC.log [SHARC.pc]: boots it through MAME's IOP
  writes and the upload, then FIFO reads get MAME's next "in", buffer reads
  MAME's value, writes must match; FLAG0 (input FIFO empty) is clear exactly
  when MAME's next event is a read (on demand, as the board will run it).
  Results: attract 600 frames MATCH (25,031 words in, 5,631 out, 458,942
  instruction lines identical); fvipers_fight 6,000 frames MATCH (613.7 M
  SHARC instructions, 50.1 M events: 21.7 M in, 5.8 M out, 9.1 M buffer
  reads, 13.5 M writes; the first 40 M instruction lines identical too).
  24.8 M SHARC instructions/s on this PC; the game needs ~102 k a frame
  (5.9 M/s): fine for the PC, the Vita needs the recompiler.
- Findings: after the boot the SHARC waits in a one-nop do-until loop at
  0x20133 (MAME: 3.46 M passes before the first input); no interrupt reaches
  it on 2B (MAME drives only its HALT line), so its rti's are not reached in
  these runs; its host boot starts no DMA (external_iop_write's write to
  0x1c sets the control word without iop_w); ASTAT's FLG0-3 bits are a copy
  of the flag inputs taken when the program rewrites ASTAT (0x20096 clears
  FLG2/3 through GET_UREG/SET_UREG): they depend on when the i960 fills the
  FIFO, so m2sharccheck compares ASTAT without them; MAME sets FLAG1 from
  the input FIFO's fullness, and only from the output FIFO's callbacks.
Next: m2sharcrecomp (the program recompiled: per instruction, the lifted
helpers with the opcode's fields constant; the pipeline registers kept as
MAME's so delayed branches and loops stay exact), checked with m2sharccheck;
then the board side (M2Board's copro on 2B), the 2B video (texture and luma
RAM), the game on the PC, the spin check, the Vita.

## Manx TT's lean mirrored; Vita steering smoothing (2026-10-19)

Lean fix: MAME declares manxtt's BANK PORT_REVERSE (a lean right reads low);
manxttc.cpp sent rt::Inputs::steer unmirrored, so the bike leaned left on a
right input (reported on the Vita). Channel 2 is now full_range(0x100 - steer)
(0x80 stays the centre). The lockstep never saw it: the trace replays MAME's
port values, not our analog(). The PC and Vita builds compile; the lean's
direction is still to be checked on the Vita.
Vita steering smoothing (platform/vita/frontend/controls.h steer_step,
settings, launcher): STEERING SMOOTHING: OFF / LOW / HIGH for any game with a
wheel, the stick and the d-pad's left/right alike, so it turns like a wheel:
each board frame it moves d / (1 + |d| / r) of the distance d to the input
(a short correction near all at once, a long one at up to r a frame), r
halving from the centre to full lock (slower at the ends); r 0.15 at LOW,
0.07 at HIGH. From the centre to within one step: LOW 0.1 in 3 frames (52
ms), 0.3 in 5, full lock in 13 (226 ms); HIGH 4, 8, 24 frames (417 ms).
Wrong turns the same day: a proportional ease (a fixed share of the way each
frame: the same time for any distance, not what a wheel does) and a plain
rate limit (the user wanted the speed to vary continuously with the distance,
not a threshold). New rt::FrontendDefaults steer_curve and
steer_smoothing: LOW for every racing game by default (Daytona, Sega Rally),
Manx TT SOFT curve and HIGH. vita.cfg key steer_smoothing. The PC frontend is
unchanged (its own rate limit on digital steering, src/app/controls.cpp).
Wrong turns, removed the same day at the user's request, don't re-add: a
touch screen steering mode (the finger's distance from the centre line) and
a tilt mode (sceMotion's acceleration along the screen's x axis, full lock at
30 degrees, the sign taken from reVita, GPL-3, read only; its clone in
extern/ deleted).
Next: on the Vita, check the lean's direction and whether LOW and HIGH feel
right in Daytona, Sega Rally and Manx TT.
In the same commit, the user's Daytona (daytona) settings: its frontend
defaults written out (music 100 %, effects 20 %, as FrontendDefaults' comment
already said) and update_z_depth_local_weight 0.8 (default 0.9).

## README: which Manx TT archive loads (2026-10-19)

The user's merged archive did not load on the Vita. The README now says
the archive must hold the parent manxtt and the Twin's Revision C files
(root or a manxttc/ folder: epr-18822c-18825c, epr-18826.30, mpr-18827.31,
manxttc_twin_nvran), and that such an archive only needs renaming to
manxttc.zip on the Vita (ux0:data/manxttc/). Not yet known why the user's
archive did not load (the message or vita-diag.log was asked for).

## Manx TT Superbike, Twin (manxttc; Model 2A-CRX), one player (2026-10-19)

The user's choice after comparing the two cabinets in MAME (18 Oct 2026): the
DX (manxtt, Revision D) adds a Model 1 sound board (68000 epr-18742, one
MultiPCM with 75 samples in mpr-18743; its YM3438 never keys on) that plays
only the engine (two families of 8 samples whose pitch follows the revs, plus
two short sounds), and a moving bike whose check screen MAME never leaves.
The Twin (manxttc, Revision C, its EEPROM set to Twin) has the SCSP board
only: its driver plays the engine itself (37 % more SCSP key-ons and 55 %
more pitch writes than the DX's in a race); the game is the same (course and
transmission select, the race, the ranking). Captures: extern/test_son/manxtt/.
Added as the other games: src/games/manxttc.cpp (the Twin's sound program
epr-18826.30 and sample ROM mpr-18827.31; the program in ROM, main_data
mirrored from 0xc00000; TGP program at 0x787e8, 1,782 words; DSP program at
0x8f14, 74 steps; both equal to MAME's after boot), scripts/games.py (game
manxtt, Vita MANX00098, ux0:data/manxttc/manxttc.zip; set manxttc in MAME's
merged manxtt.zip, whose manxttc/ folder the importer reads by file name),
seeds/manxttc.txt (MAME harvest of attract, a race and a test-mode walk, 413
targets, then seed_scan.py: 1,325), seeds/manxttc_snd.txt (six jump tables,
153 entries, and 53 function starts after rts/rte from the harvest's
M2TRACE_SNDPCS), scripts/inputs/manxttc_race.txt, manxttc_boot.txt.
Controls: THROTTLE, BRAKE, BANK on the 315-5649's channels 0-2 (accel,
brake, steer, rescaled to 0x00-0xff as Sega Rally's), Start 0x40, shift
up/down on IN1 0x10/0x20 from the view buttons (Control::View1/View2).
The m2trace plugin now records :THROTTLE and :BANK, and skips IN1's gear
bits only for a set with a :GEARS port (Manx TT's shift buttons are there).
Wait loop 0x3f1c-0x3f33 (videoctl's frame bit, as Sega Rally's; 87 % of the
interrupts), idle only.
Sound: the SCSP voice table {0x2800, 0x10, 4}: channels 0-9 the music
(owner 0), the engine, brakes, effects and voices on 0x0e and 0xff:
music_channels 0x03ff; effects at 100 % by default (the engine is one).
output_gain 3.4 (race peak -11.7 dBFS against Daytona's -1.0). POLYGON ORDER
scale 10.0 (race w p99.9 20,900, max 23,100; temporary probe, removed).
Wrong turns: without the default EEPROM the game boots as the DX and waits
on the moving bike's check (below); the first sound seeds missed the
function starts after rts (0x6048a8) and the sixth table (0x6048a4 jmp (A4)).
Checked: lockstep against MAME after the fixes below: the boot (1,200
frames) and manxttc_race (6,000 frames, the throttle, brake and lean: 1.06 G
i960 and 134 M TGP instructions, 185 M device events, 4.3 M polygons, 4,622
3D layers and 5,996 screens identical, buffer RAM hash at 5,938 of 5,938
samples, sound MATCH). On the PC the race runs through (two laps, ranking,
attract); the test mode opens, moves, opens SOUND TEST and GAME ASSIGNMENTS
(CABINET TYPE TWIN) and leaves them, as in MAME. --fast-inaccuracy
(vita_fast): the race (the same 9,079 sound bytes), 8,000 frames of attract
and the test walk run through. Music/effects split: m2run --volumes 1,0 and
0,1 on the race: the menus' effects and the engine follow the effects
volume, the music the music volume (extern/test_son/manxtt/twin_pc_*.wav).

## A set's default EEPROM; the TGP catches up before the vblank parse and i960 buffer writes (2026-10-19)

Default EEPROM: LoadRegion::Eeprom (MAME's "eeprom" region, 128 bytes):
rom_import keeps it in Images::eeprom, m2import writes eeprom.bin when the
set has one, GameLoop loads it if present, and M2Board starts the 93C46
from it; a frontend's saved EEPROM still replaces it. Only manxttc uses it
(manxttc_twin_nvran: the Twin cabinet).
TGP: M2Board::vblank_start (and m2native's geometrizer check) now runs the
TGP to its FIFO wait before the geometrizer parses buffer RAM. MAME's TGP
runs alongside the i960; ours on demand, and Manx TT has the TGP write
display list words to buffer RAM with nothing else making it catch up:
frame 834 of its boot read a stale 0 (MAME 1) and the geometrizer looped on
a texture data block's garbage count.
Second case, in a race (frame 1015): TgpBoard::buffer_w now runs the TGP on
the input it already has before an i960 write to buffer RAM. Manx TT keeps
the TGP's display list write pointer in buffer RAM word 0x6000: the i960
writes 0x128, pushes the command that reads it, then writes 0x2cc; MAME's
TGP read 0x128 in between, ours (run later) read 0x2cc twice and wrote its
matrix at 0xb3 instead of 0x4a. Found with temporary logs of the buffer
writes and a MAME Lua dump of buffer RAM (removed). i960 reads of buffer RAM
already caught the TGP up.
Checked, both: daytona93 race_basic, srallyc_race, vf2_fight and vcop_shoot
m2run hash and WAV identical before and after.

## Virtua Cop 2: the bal g14 inline texts; its level raised to Daytona's (2026-10-18)

The user, on the console: 0x38254 has no code (after "SCENE"). A second kind
of inline text: the routines 0x79c0, 0x7a70 and 0x7adc are called as bal
...,g14 with a NUL-terminated text after the call, read byte by byte, and
return to the next 4-byte boundary past the NUL (subo g14,0,r15; and
3,r15,r15; addo r15,g14,g14; bx (g14)). Every return point from the 32
calls: 29 new seeds (seeds/vcop2.txt 1,620). Searched every game for a bx
(g14) after a change of g14: only these three (VF2's three restore g14 from
the stack: plain returns). Checked: the scripted 9,000-frame game and the
14,000-frame continue run go through.
Level: m2run WAVs, a 9,000-frame game RMS -32.6 dBFS, peak -15.8; attract
-32.5, -16.5; Daytona (daytona93) race_basic -16.4, peak -1.0, attract
-22.0, -6.0. Game::output_gain 5.5 (+14.8 dB): its game's peaks at
Daytona's race. As for Sega Rally and VF2, the frontends only (m2run's WAV
and the parity checks stay at the board's output).

## Virtua Cop 2 and VF2: the code after inline texts (2026-10-18)

The user, on the console: Virtua Cop 2 stops on 0x2faac (no code) when the
player dies and continues (coin or Start). MAME harvests (a game without
shooting and coin + Start every 300 frames, 14,000 frames; one left to game
over) found 0x2faac and 0x2fad0, both from 0x4408 bx (r14), and 0x485e0 (a
bx (g14) return). 0x4350-0x4408 is a print routine called as balx
0x43xx,r14 with its text inline after the call; it skips the words up to
the first one <= 0xffffff and returns past them, so the code after each text
is reached only from there and only when that text is shown. Every return
point computed from the program ROM (193 calls, 187 new seeds; all decode as
code), plus 0x485e0: seeds/vcop2.txt 1,591. Checked: the continue run (14,000
frames: CONTINUE?, play resumes, dies, continues again) and the game-over run
go through on the PC.
VF2 has the same routines (0x93c4-0x947c, 461 calls; and 0x4ec00, returning
8 bytes past the call, 42 calls): 488 new seeds (15 were already seeds) and
0xa440 from seed_scan.py: seeds/vf2.txt 1,858. vf2_fight 4,000 frames hash
and WAV identical before and after; 8,000 frames of attract run through.
Daytona, Sega Rally and Virtua Cop have no such return (no bx (r14)).
For a new game: grep the disassembly for bx (r14) after ld (r14) and compute
the returns the same way.

## POLYGON ORDER hybrid: one depth band per Model 2 window (2026-10-18)

The user, on the console: HYBRID makes some UI elements vanish. Measured on
the PC (temporary probe in Video::screen_update, polygons and w range per
window every 60th frame; removed): the UI lives in its own windows above the
3D, e.g. Virtua Cop in play w1 ~1,500 polygons (w 10-470), w4 13 polygons at
w 1,170-1,230, w5 2 at w 1; Sega Rally w2-w4 (1-24 polygons); Virtua Cop 2
w3-w6; Daytona w3 (2). The window only counted through the rank, which
HYBRID weights 10 %: w4's far distance (nearness ~0) sent it behind the 3D.
Fix, the user's choice among the ones proposed: in HYBRID only, the CPU
(model2_polygons.cpp) codes each polygon's depth as 1 + band + its rank in
its window (in (0, 1)); the order runs window by window, front first, the
front window band 31 (kZDepthBands 32; past 32 windows the backmost share
band 0). kVertexShader decodes (floor), mixes rank and nearness as before
inside the band, clamped to [0.001, 0.999] of it, and scales by 0.9999/32;
the 2D layers (0, 0.99999994) are below 1 and untouched. ARCADE: the same
depths and shaders as before (one bool test a frame).
Effect: each window keeps the arcade's priority over the others; the hybrid
mixes inside a window only. Sega Rally's road, cars and shadows are all in
w1 (unchanged); in VF2 the stage (w6) and the fighters (w7) no longer cut
each other, as on the board.
Not taken: the UI alone at 0.9999 (no UI flag on the Model 2; VF2's top
window holds the fighters); "windows above the largest one are UI" (proposed,
then the user chose the per-window bands).
Checked: Vita host tests 38/38. Not checked: the shader compile and the image
on the console (no GLSL validator here).
Then, the user on the console: VF2's scenery and fighters should mix (they
did before the bands), and Sega Rally's third course "seemed" to have its
scenery in another window. Per-game window groups,
Game::update_z_depth_window_groups (up to 4 ranges, first-last inclusive; a
code parameter, not a menu option): the grouped windows share one band
(model2_polygons.cpp z_band_key). Found by drawing each window alone on the
PC (temporary M2_WINDOWS filter and per-window log in Video::screen_update,
removed); the window lists are the geometrizer's, which matches MAME
(m2_check lockstep, polygons MATCH).
- VF2 (vf2_fight, every frame of 9,000): windows 1 scenery, 5 ring floor, 6
  shadows, 7 fighters (2: 32 polygons in 25 frames; 0 alone on the select
  screen). Group {1, 7}.
- Sega Rally, the Mountain (practice, script below): w1 the whole world, w2
  the rev counter's needle (w 128), w3 the mirror's frame, w4 the mirror's
  view (its own viewport); at the start w2 holds the cars (~350 polygons).
  Third-person view (the VR button): w2 the player's car (~300 polygons, w
  4-8), w3 the needle. Group {1, 2} (the user's choice, to be checked on the
  console): the car mixes with the world; in the cockpit view w2 is the
  needle (one polygon, w 128), which may then go behind near polygons. A
  polygon-count condition per group was proposed for that and not taken.
- Daytona race_basic: the race in w1 alone (4,532 of 6,000 frames); w2-w3
  only in the menus. Virtua Cop / Virtua Cop 2: the scene in w1, the other
  windows UI (sight, cylinder, the tutorial's frame and gun).
Reaching Sega Rally's practice in a script (MAME does the same): hold the
wheel right *before* Start (the SELECT GAME screen confirms itself after ~50
frames, at the wheel's position: PRACTICE right); the course follows the
wheel's position (EXPERT = the Mountain at the right) and must be confirmed
with the accelerator while the wheel is still right: 1420-1560 steer=0xe0,
1450 start=1, 1600-1610 accel=0xe0, 1700-2420 steer=0xe0, 2400-2410
accel=0xe0, then the accelerator from 2700.

## Virtua Cop 2 (vcop2; Model 2A-CRX), one player (2026-10-18)

Added as the other games: src/games/vcop2.cpp, scripts/games.py (game
virtuacop2, Vita title ID VCOP00097, ux0:data/vcop2/vcop2.zip; set vcop2:
model2a, --no-mirror, --scsp), seeds/vcop2.txt (1,403: MAME harvests of
attract, a scripted game and walks through the test mode, then
seed_scan.py), seeds/vcop2_snd.txt (six jump tables of the 68000 driver, 153
entries), scripts/inputs/vcop2_shoot.txt (vcop_shoot with the reload at gunx
0x089: vcop2's range starts at 137, so vcop's 0x083 is not off-screen there
and the MAME replay "diverged").
The program runs from ROM (two pairs, 2 MB); main_data's last pair is
mirrored from 0xc00000. TGP program at 0x5d480 (1,778 words) and SCSP DSP
program at 0x8cc8 (84 steps), both equal to MAME's after boot.
The gun: no I/O board on the 2A-CRX; the 315-5649's serial channel 2 carries
it behind a multiplexer (MAME lightgun_mux_w at offset 0x0a, lightgun_mux_r at
0x0c: 0-7 P1 Y, P1 X, P2 Y, P2 X, low byte then high; 8 and up 0xfc | the
off-screen bits). IoBoard::gun_byte builds the report for both boards
(latch_gun for vcop's DPRAM, crx_read for vcop2); GunRange now holds player
2's fixed aim (p2_x, p2_y: MAME's P2 defaults, vcop 0x179/0x0e8, vcop2
0x17c/0x0e6), which latch_gun had as constants. Range: MAME vcop2 P1
PORT_MINMAX 137-630 x 36-425.
Wait loop (M2_SPIN_SKIP): 0x36fc ldob 0x500000,r3; cmpibe r3,g0 (ByteIsReg,
as Daytona's), the idle range too: 9.1M of 69M instructions skipped in
attract, 23M in a game.
Sound: the SCSP driver's voice table is VF2's ({0x2800, 0x10, 4}), but the
channels differ: in 9,000 frames of play every voice on 0-9 is the music's
sequence (owner 0; 9 the drums), 10-15 the shots and voices (owners 1-0x13,
18 of 268 owner 0), 15 included. music_channels 0x03ff. Checked with m2run
--volumes 1,0 and 0,1 on vcop2_shoot: the music and the shots land in
separate files; the first second (the boot sound, before the driver fills
its table) is in both.
POLYGON ORDER scale 7.3 (scripted game, every 20th frame's vertex w: p50 38,
p99.9 1,230, max 1,420; ln max 7.26; temporary probe, removed).
Checked against MAME (m2_check.sh lockstep): attract 600 frames MATCH (593
interrupts, 596/596 screens); vcop2_shoot 2,400 frames MATCH (270M
instructions, 3.7M device events with the gun serial reads, 2,511
interrupts, 1.7M polygons, 2,396/2,396 screens, sound MATCH). MAME delays
irq_enable_w by 80 ns ("vcop2 needs this"); ours applies it at once and
still matches.
--fast-inaccuracy (vita_fast) checked on the PC: vcop2_shoot 2,400, a
scripted game 9,000 (the same 898 sound command bytes as the exact code) and
attract 8,000 frames, no new seed. Test mode: a 24,000-frame walk runs
through (2.46B instructions: the memory test loops with no vblank wait).
Regression: daytona93 race_basic, srallyc_race, vf2_fight, vcop_shoot m2run
hash and WAV identical before and after the IoBoard change; ctest 23/23 on
daytona93 and vcop2.
Not done: the Vita VPK on the console; player 2.

## Virtua Cop: the test mode's missing seeds (2026-10-18)

Entering the test mode stopped on a jump with no code (as VF2's had). Added
to seeds/vcop.txt: a MAME harvest of a 12,000-frame walk through the test
mode, seed_scan.py again, 0x28dc0, 0x28e10, and the handler tables at
main_data 0x82c3b0 (16 entries) and 0x82c8c0 (7). Wrong turn: a table read 8
bytes off added 0x20 and 0x20202020 (text), removed. Checked: a 24,000-frame
test-mode walk and the scripted game run through (977 seeds).

## Vita: POLYGON ORDER help text without Sega Rally's example (2026-10-18)

The user: "(SHADOW / WHEEL)" is Sega Rally's case; the option is every
game's. Sega Rally's local weight is now 0.91 (the user's setting).

## Vita option POLYGON ORDER: a hybrid depth (2026-10-18)

The user's idea, from the user's own shader test on Sega Rally (overlapping
shapes, a car's shadow over its wheel). The Vita's depth buffer only holds
each polygon's rank (one flat depth per polygon, render/gl_internal.h). The
kVertexShader branch under `#ifdef upgrade_z_depth`: for Model 2 polygons
(depth strictly inside (0, 0.9999); the 2D layers keep 0 and kForegroundDepth)
depth = clamp(rank * (0.9999 - W) + nearness * W, 1e-5, 0.9999), nearness =
clamp(1 - ln(max(w, 1)) / S, 0, 1); W = update_z_depth_LOCAL_WEIGHT, S =
update_z_depth_SCALE, both #defined by build_program from the game's record
(Game::update_z_depth_local_weight, default 0.9, Sega Rally 0.95 by the user;
Game::update_z_depth_scale, default 12.5).
Scales measured on the PC (temporary probe in Video::screen_update, every
20th frame's vertex w; removed): daytona93 race_basic p50 25, p99.9 348 ->
6.0; vf2_fight p50 5, p99.9 71 -> 4.5; vcop scripted game p50 36, p99.9
1,230 -> 7.2; srallyc_race p50 106, p99.9 1.9e5 (the sky) -> 12.5.
Wrong turns, on the console: the first version added w * 0.0005 to every
program's depth: the HUD (depth 0.99999994, w 1) went past 1 and was clipped
away; w is the vertex's distance (1 to 2e5 in Sega Rally), so w * k also
pushed far polygons in front.
Option (every game): Options > POLYGON ORDER: ARCADE (default) / HYBRID,
VitaSettings::upgrade_z_depth (vita.cfg upgrade_z_depth=). gl_init compiles
both sets of the 7 programs (g.program, g.program_z; a failed hybrid one
falls back to the plain one); draw_list picks the set per frame
(gl_set_upgrade_z_depth, from main.cpp before gpu.draw). Off: the same
shaders and image as before (the plain set has the #define lines but not the
branch); the first boot after this compiles 14 programs.
Checked: Vita host tests 15/15, Vita compile check, the four PC builds. Not
measured on the console (cost: a log and a few operations per vertex, nothing
per pixel). Not on the desktop (m2.hlsl): to port if kept.

## Music channels: a game parameter (2026-10-18)

The user asked for the music/effects split to be each game's data, as its
controls are, instead of rules in the code. Before: Model 1 board and native
engine `channel <= 9 || channel == 15` (NativeSoundSequencer::music_channel,
static), SCSP board `channel > 8 && channel != 15`. Now Game::music_channels,
a mask (bit n: channel n is music; any other channel, or a voice no channel
owns, is an effect), passed to snd::SoundBoard, snd::ScspBoard and the native
engine/sequencer (constructor parameters, default rt::kDaytonaMusic 0x83ff;
the sequencer cannot call rt::game(): its unit test links no game list).
Values: daytona93, daytona, vcop 0x83ff (0-9, 15); srallyc, vf2 0x81ff
(0-8, 15). has_music_effects also needs a nonzero mask.
Checked: m2run with split volumes, before/after, WAV byte-identical on the
four games (daytona93 race_basic 3,000 frames at 1/0.2, srallyc_race 3,000 at
1/0.85, vf2_fight 4,000 at 0.7/1, vcop_shoot 2,400 at 0.7/1); ctest 23/23 on
each; Vita compile check. A wrong turn: the sequencer first read
rt::game() itself, which broke test_native_sound_sequencer's link.

## Virtua Cop: music/effects split by its own voice pools (2026-10-18)

The user, on the Vita: only part of the music followed the MUSIC volume, the
rest the EFFECTS one. SoundBoard::voice_channel looked for each key-on's voice
in Daytona's driver's pools (0xf01500 and 0xf01618, 10-byte records): on vcop
1,307 of 1,676 key-ons (78 %) matched none, and an unmatched voice counts as
an effect. Virtua Cop's driver keeps 12-byte records at 0xf01500 and 0xf01650
(the voice searches at 0xa12-0xa44 and 0xd5a-0xdbe: lea $f01500/$f01650,
moveq #$1b, adda.l #$c; byte 3 the channel, as Daytona's). Now
rt::Model1Voices (pools, stride, Daytona's byte-6 reserved records) per game
(Game::model1_voices; nullptr: Daytona's, kDaytonaVoices), passed to
snd::SoundBoard. vcop: 0 of 4,357 key-ons unmatched (scripted 9,000-frame
game); channels 0-8, 11, 12, 15. The channel rule (music 0-9 and 15) holds:
a shot is command ae 20 01 and a reload ae 20 07, each starting one voice on
channel 11 (chip 1) right after; channel 15 is the music's beat.
Measured (vcop_shoot, --volumes): effects only in the attract (no shots,
frames 300-1400) -27.0 dBFS before, -35.9 after (music only -26.0): the
attract's demo shots are what is left; around the stage's shots effects only
-21.7. Volumes 1/1: WAV byte-identical before/after. ctest 23/23 (vcop,
daytona93).

## Virtua Cop (vcop, Revision B; original Model 2), one player (2026-10-18)

New game `virtuacop` (scripts/games.py; build_virtuacop/, Vita virtuacop.vpk,
title ID VCOP00096, images platform/vita/logo_livearea/virtuacop/):
src/games/vcop.cpp, seeds/vcop.txt (826), seeds/vcop_snd.txt (136),
scripts/inputs/vcop_shoot.txt. Daytona's board (Model 1 sound board, the I/O
board behind the dual-port RAM): the program is two ROM pairs (512 KB), no
main_data mirror, the TGP program in the program ROM at 0x70c0 (2,024 words,
CRC 0x77D05CE6: MAME's copro_tgp_program after boot, Lua dump).

Light gun. MAME runs vcop with a Model 1 I/O 2 board (model1io2, BIOS
epr-17181, its own CPU). MAME needs hd44780_a00.bin (the I/O board's LCD
font) to start vcop: a zero-filled 4 KB stub in a scratch rompath folder
(ROMPATH="roms;stub") is enough (bad CRC warning; the LCD is cosmetic). The
i960's DPRAM traffic (Lua taps, 1,500 frames): the same protocol as Daytona's
board (write 1 at 0x20, poll it back to 0: 4.9 M reads), then reads 0x08-0x0a
(IN0-2), 0x0e (0x0f: the board's DIP switches), 0x0f, 0x11, and 0x80-0x88.
Moving MAME's P1_X/P1_Y and dumping the DPRAM: 0x80-0x81 player 1's Y,
0x82-0x83 X (16 bits LE), 0x84-0x87 player 2's, 0x88 off-screen bits (1, 2).
IoBoard::latch_gun writes them (Game::gun: player 1's range, MAME's
PORT_MINMAX 0x083-0x276 x 0x024-0x1a9; the off-screen bit by MAME's rule:
within 5 % of an edge); player 2 at MAME's defaults (one player only); bytes
0-7 0xff (no ADC), in1 | 0x70 (Daytona's gear bits unused). Controls:
Control::Trigger (IN1 0x01); rt::Inputs::gun_x/gun_y; rt::aim_gun maps a
pointer in the 496x384 picture to the range, the margins to the left edge
(off screen: a shot there reloads). Vita: the front touch screen (scaled from
sceTouchGetPanelInfo's display corners, not a fixed x2; the picture at 544/384
scale, centred), a touch fires, Cross too (at the last aim); PC: the mouse
aims, left button fires, right button reloads (src/app/main.cpp mouse_gun).
m2run scripts: gunx, guny, trigger; make_input.py: P1_X/P1_Y; the m2trace
plugin records/replays :P1_X/:P1_Y.

Wait loops. The vblank wait 0x9124-0x9137 (89 % of MAME's interrupts):
ld 0x98000c (videoctl), counts its passes in g0 (stored at 0x5016b4, read
only by 0x2760, a load meter nothing calls), loops while the frame bit is
unchanged: Game::idle (1,800 frames of attract: 273 M -> 47.6 M i960
instructions, same last screen hash) and M2_SPIN_SKIP with a new kind,
SpinWait::WordBitsCount (a 32-bit device read at `ip`, `mask` compared with
`reg`, the skipped passes added to `counter`; hooked on the Dev read in
read32): 4.1 M of the 47.6 M skipped, frames identical to the run without it.

Seeds. MAME harvest (run_trace.sh): 3,600 and 8,000 frames of attract and a
scripted 9,000-frame game (shots across a grid, reloads), then seed_scan.py.
Function pointers in main_data (0x33250 at 0x86ac64) sit after a ret and
zero padding (16-byte alignment): seed_scan's data-ROM rule now skips up to
3 zero words before the ret. Sound: the 68000 dispatches commands through
"movea.l #$f4e,A5; lsl.l #2,D2; jmp (A5,D2.l)": 128 jmp/bra entries
0xf4e-0x114a, all seeded, plus 8 starts from M2TRACE_SNDPCS.

Checked against MAME (SET=vcop scripts/m2_check.sh, ROMPATH with the stub;
traces/hdr_vcop.m2in): attract 600 frames: m2native, m2replay, m2tgpcheck,
m2sndcheck MATCH (95.8 M instructions); vcop_shoot 2,400 frames (stage 2,
four shots, a reload): m2native MATCH (391.8 M instructions, 92.9 M device
events incl. the gun's DPRAM reads, 2,443 interrupts), TGP 28.8 M, 1.07 M
polygons, 3D 775/775, sound 31.9 M MATCH; screen 2,386/2,396 (10 composed
frames differ from frame 135, as in the attract: not looked at; MAME's own
picture is not exact for this game, per the user). m2run: the revolver
empties shot by shot and refills on the off-screen shot.
Unchanged elsewhere: daytona93 race_basic 3,000 and vf2_fight 4,000 frames,
counts, hashes and WAV identical before and after; ctest 23/23 (vcop);
Vita host tests 15/15 (test_vita_controls gains the gun); Vita compile check.
--fast-inaccuracy (games.py vita_fast): recompile.py --fast_inaccuracy on the
PC, 13,673 blocks; vcop_shoot 2,400, the scripted game 9,000 and attract 8,000
frames run through with no new seed; the revolver empties and refills as with
the exact code (frames differ: interrupt timing).
MAME needs hd44780_a00.bin only for itself (run_trace.sh, m2_check.sh); the
recompilation and the game (PC, Vita) read vcop.zip alone.
Not done: the Vita VPK on the console; the 10 screen differences.

## SCSP: decay steps only when due, volumes folded into the gains (2026-10-18)

The stepped decay (939af4b) ran DecayStep for every slot and sample, and
VF2's music at 70 % made every slot sample go through a float multiply
(unity_ false). PC, cycles in the slot path (perf, P-core pinned, 20 kHz,
9,000 frames of the kicking fight): yesterday evening's code (b2114b9,
volumes 1/1) 13,642 samples; this noon's (stepped decay, music 0.7) 18,583
(+36 %); now 12,503 and 13,413 (two runs; run-to-run noise ~7 %).
- DecayStep sets eg_next (the next sample that can step: next even one from
  rate 0x30, else the next multiple of the column's period) and eg_gain;
  EG_Apply calls it only then, at an EG state change, or after a register
  write (DecodeSlot sets eg_next 0). Volumes 1/1: WAV byte-identical
  (vf2_fight 6,000 frames, md5 cd5741cd...).
- ScaleGains: the music/effects volume multiplied into dsp/left/right gains
  (Decoded::*_mix), on decode, key-on, owner change and set_volumes; at 1/1
  the gains themselves. At 0.7/1 against the per-sample float: rounding only,
  max 9 LSB, 57.6 dB below the signal.
ctest 23/23 (VF2, Sega Rally). Next if needed: per-slot block rendering
(loop interchange, byte-identical; the bigger gain, longer), no ring-buffer
writes without modulation slots, -O3/ARM mode for scsp.cpp on the Vita.
Not measured on the Vita yet.
Also checked today: no memory leak (m2run draining the audio as the Vita
does: 178.7 MB at 2,000, 7,000 and 14,000 frames; without draining m2run
itself grows, its audio buffer). Core 2 load rises in later fights with
the active voices (kicking fight: 12.6 on average in match 1, 19.3 after,
up to 30); the 2D worker is flat at ~1.4 ms (perf_sound.log windows 9-30).

## Docs: complete MAME ROM sets, full merged (2026-10-18)

README.md (new "ROM sets" section), HOW_TO_COMPILE_RELEASE.md (You need) and
platform/vita/README.md now say the ROM sets must be complete MAME sets,
full merged (every file of the set, the parent's included): a split clone
archive is refused, the importer checking every file by name and CRC.

## SCSP music/effects: by the voice's MIDI channel, not its owner (2026-10-18)

The user heard, on VF2 with the music lowered, the first match's round 1
"fight" and some end-of-fight speech (a character's laugh) go down with
the music, the same sounds later with the effects. The voice table's 16
bytes at key-on (temporary trace, m2run, a 14,000-frame fight kicking all
along: scratchpad input, the player wins the first match): byte 4 is the
voice's MIDI channel (music on 0-8 and 15; hits on 14, announcer and voices
on 13, also 9-12), byte 5 the owner used until now (0 = the music's
sequence). The driver plays some voices from the music's sequence: 43 of
8,300 key-ons on channels 9-14 with owner 0 (the "fight" at 27.3 s, channel
13 TL 1; speech TL 7 on channel 14; ~30 hits). Sega Rally's race: the
same convention (music 0-7 and 15, effects 9-14 and 0xff; 2 of ~3,700
key-ons differ). Now ScspVoices::channel (byte 4) for both games; effect =
channel 9-14 or above 15.
Also: the driver rewrites a playing slot's table entry without a key-on
(owner changes during play: 47 in vf2_fight 6,000 frames, 164 in the
kicking fight; 41 followed by the slot's key-on within 10 ms, 107 not
within 100 ms). Scsp::render now asks again every 32 samples (not every
call: ~1,000 calls a frame; only when the volumes are not both 100 %).
Measured, effects only (--volumes 0,1, kicking fight): owner rule -> channel
rule, 25 seconds louder (sounds that were silent, e.g. +112 dB at 29 s),
none quieter. Every voice moved has TL 0-7 (music is 13+). Volumes 100/100:
WAV identical (vf2_fight 6,000 frames, md5 cd5741cd...). ctest 23/23 x2.
A first idea, wrong: the owner written just after the key-on (none of
the changes came within 100 samples of one).

## Vita menu: the build date in the title line; VF2's music at 70 % (2026-10-18)

The menu's first line was "title | x.x FPS | CPU | GPU | VITAGL"; now
"title | YYYY-MM-DD | CPU | GPU". The date comes from build_date.h, written
on every build by platform/vita/build_date.cmake (target vita_build_date,
copy_if_different: launcher.cpp recompiles only when the date changes).
Diagnostics::display_fps() (only the menu read it) removed; perf.log keeps
its fps. A wrong turn: -DOUT="..." with VERBATIM passed the quotes, the
header landed in a folder named `"` under the build dir (removed).
VF2's frontend defaults: music 70 %, effects 100 % (the owner's choice; a
saved settings file keeps its value until RESET DEFAULTS). Built: VF2 and
Sega Rally diagnostics VPKs; not run on the console.

## SCSP: stepped note decay and the DSP at 25 %, fixed by default (2026-10-18)

The user's choice after a listening test (files listed at the end), not a MAME or PCB
measurement. snd::Scsp, constants in scsp.h, no runtime option:
- kSteppedDecay: decay 1/2 and release by Yabause's newer SCSP steps
  (Scsp::DecayStep, EG_Apply; MAME's attack kept). Rate = 2R + 2KRS + OCT +
  FNS bit 9 (2R for KRS 0xf; precomputed per register write in
  Decoded::decay_base), periods as powers of two (masks, no division: the
  Vita's A9 has no divider). D2R 0 still decays (Yabause's rule; MAME holds).
- kDspReturnPercent 25: the DSP's EFREG returns in the output mix.
- -DM2_SCSP_MAME=ON (CMake option, root build): MAME's output again. rules.md
  and the design doc (SCSP section) record the exception.
A bug on the way: a sentinel -1 for "KRS 0xf" in decay_base collided with
real negative bases (OCT 13, KRS 0: -3); found by comparing with the
experiment's WAV. Checks: default build, m2run 4,000 frames of vf2_attract
and srallyc_attract, WAV byte-identical to the listened files
(extern/test_son/vf2_attract_decay_dsp25.wav, srallyc_attract_decay.wav,
with the games' gain); -DM2_SCSP_MAME=ON: vf2 WAV md5 67e52193 as before the change, srallyc identical to the listened mame file.
Listening files kept in extern/test_son (git-ignored):
{vf2,srallyc}_attract_{mame,decay,old,both}.wav, vf2_attract_{mame,decay}_
{dsp50,dsp25}.wav (old: Yabause's default SCSP model, no DSP; both: old with
the stepped decay). Measured, before the games' gain (VF2 x1.5, Sega Rally
x10): VF2 RMS -22.5 dBFS at DSP 100 %, -26.7 at 50 %, -29.5 at 25 %, -32.6
without (old); decay alone 36.6 dB below the signal on VF2, 19.6 on Sega
Rally. Sega Rally's attract does not use the DSP return (byte-identical at
any level): to hear the 25 % on it, a race recording is needed.
Not done: Vita build and console listening; a VF2 fight recording.

## VF2 sound: Yabause's SCSP compared with MAME's (2026-10-18)

extern/yabause (https://github.com/Yabause/yabause, 7e38821, GPL-2: reference
only, nothing copied; in .vscode git.ignoredRepositories). Three SCSPs in it:
scsp.c's default one (2004-2012, table-driven), scsp2.c (its threaded
variant, a build option) and scsp.c's "new SCSP" (use_new_scsp, off by
default: hardware-derived envelope steps and level maths, but no
interpolation, op6 empty). Against ours (MAME's), for what VF2 uses (8-bit
samples, no LFO, no modulation; slot probe and key-on trace of 2026-10-17):
- Level (TL, EG): the same scale (0.375 dB per TL step, 3/32 dB per EG
  unit). The new SCSP turns attenuation into gain by a piecewise-linear
  mantissa and shift (as hardware would); against MAME's exact powf:
  -0.29..+0.51 dB. Not a balance change.
- Envelope decay/release: the new SCSP's step tables decay ~7 % slower
  than MAME's time table (1.06-1.12 x over 0x3bf units, every rate 2-60;
  MAME spreads its ms times over 1023 units, which is 1023/0x3bf = 1.067).
  Attack: MAME ramps the amplitude linearly from 0x17F, the new SCSP steps
  the attenuation exponentially from 0x280 (the same start point).
  Key-on only from RELEASE in both. The default Yabause SCSP decays
  linearly in amplitude (a much louder tail): not a hardware reference.
- Pan: MAME 3 dB per DIPAN/EFPAN unit (bit weights 3/6/12/24, the
  manual's law). New SCSP 6 dB per unit (a shift), default SCSP 6 dB per
  two units: both coarser; VF2's music pans 3 and 8 would be 9-24 dB lower
  on one side there, not a correction.
- Send levels (DISDL, IMXL, EFSDL): 6 dB per step in all three; the
  direct/DSP/effect-return ratios are the same (Yabause /4 overall).
- MVOL: MAME linear MVOL/15, new SCSP 6 dB per step: overall level only.
- DSP (scspdsp.c vs MAME): COEF >> 3 in both; MIXS/EXTS scaling as MAME
  (EXTS unused on Model 2). Differences: EFREG overwritten (MAME adds;
  VF2 writes each of EF0-3 once: no effect), ACC wrapped to 24 bits (MAME
  26), MRD/MWT pipelined one step later and allowed on even steps (MAME
  odd only; VF2's 82 steps access memory on odd steps only, IWT two steps
  after MRD: the same values), SHIFT 3 gives 0 (a Yabause bug: shift == 2
  tested twice). VF2's program has no SHIFT 3.
Conclusion: Yabause shows no MAME error that would change VF2's balance
(speech vs music); its hardware-derived parts differ by <= 0.5 dB and ~7 %
decay time. Candidates if wanted (off by default, to be measured on the
WAV first): the new SCSP's decay steps (notes ~7 % longer) and its
log-to-linear level curve. Neither explains a quiet end-of-fight speech.
Temporary key-on trace (voice table's 16 bytes at key-on, SCSP_KEYLOG) left
uncommitted in the working tree; the idle-fight run was stopped by the
user before the end of a round, no result.

## VF2 sound: MAME vs sm2-emu vs ours; the end-of-fight speech (2026-10-17)

extern/sm2-emu (https://github.com/dmanlfc/sm2-emu, df14ed4, BSD-3; added
to .vscode git.ignoredRepositories): its SCSP is MAME's too. Differences
from MAME: pitch-LFO depth table and a rewritten modulation (VF2 uses
neither: 0 % in the slot probe), an 8 kHz output low-pass computed but never
applied (dead code), a flat per-game gain (VF2 840/256, +10 dB, "a
mastering choice, not a hardware-accuracy claim"); DSP identical to MAME.
So for VF2 the three compute the same mix: no MAME error found behind the
balance; no hardware recording to go further.
Key-on trace (temporary, not committed; desktop game, the user playing to
the end of a fight): VF2's driver streams every sound into 32 per-slot 8 KB
RAM buffers (SA 0x10000-0x4e000): addresses do not identify sounds. Effects
key with TL 0-4, music notes TL 13-25, all DISDL 3, direct out. The
end-of-fight speech (t 55.3 s): one slot, TL 7 (-2.7 dB), direct out, no
DSP, and the driver's voice table marks it as music (owner 0): the MUSIC
setting lowers it, not EFFECTS. Next if wanted: a speech class (a third
owner or another table field) to give it its own gain (off by default).
Vita-only suspect for "strange" sound: when core 2 saturates the reference
audio slows to 97-98 % (pitch down) with gaps; the SCSP speed-up (21c7984)
should reduce it. Diagnostics VPK with it built, not yet run.

## SCSP slots: registers decoded per write, a plain-slot path (2026-10-17)

From the user's perf.log (extern/result_perf_vita/vf2/perf_sound.log, 30
windows, attract then fights; first run of the slot probe 7af308c): the
sound board is 5.5 ms a frame on the title screen, 13-15 ms in fights and
16.4-17.2 ms in the busiest ones (windows 24, 25, 29: core 2 at 100-105 %,
audio gaps, playback speed 97-98 %). Of it: 68000 3-4.3 ms (always 14,357
instructions; ~700-1,000 SCSP register accesses), DSP 2.6 ms (flat), output
mix 0.8 ms, and the slots, linear in the active voices: ~0.46 ms a frame
per voice (0.6 us per voice and sample, ~265 cycles at 444 MHz), up to
9.5 ms with 20.7 voices. VF2's voices are all 8-bit, none with a pitch or
amplitude LFO or modulation.
Change (scsp.h/.cpp, MAME's code otherwise; THIRD_PARTY.md updated):
w16 marks the slot's registers changed; render() decodes them once
(DecodeSlot: SA, LSA, LEA, LPCTL, SBCTL, PCM8B, SDIR, STWINH, LPSLNK, ISEL,
IMXL, and the four pan-table gains: DSP send, left, right, ring buffer)
instead of through the macros on every sample; a "plain" slot (SSCTL 0,
no PLFO/ALFO, no modulation: all of VF2's) runs UpdateSlotPlain, MAME's
UpdateSlot operations in MAME's order without the cases that do not apply;
EG_Update and UpdateSlotPlain inlined into render().
Checks: m2run vf2_fight 6,000 frames, vf2_attract 8,000, srallyc_race
4,000, srallyc_attract 4,000: WAV md5 and every count identical to before;
ctest 23/23 (VF2, Sega Rally). PC perf (perf record, vf2_fight 4,000
frames, samples in render + UpdateSlot + EG_Update): 1,451 before, 1,206
decoded, 916 inlined (-37 %). On the Vita, not measured yet: the macros
cost more there than on x86; the next perf.log tells.

## src/runtime/rom/ renamed romset/ (2026-10-17)

The "rom/" line of .gitignore (any folder named rom: a safety net for ROM
dumps, rule 6) also matched src/runtime/rom/, made by the runtime's
reorganisation: its seven files were committed (git add of renames), but a
new file there would have been ignored silently. A first fix added an
exception to .gitignore (d1d0cce); the user preferred the folder renamed:
src/runtime/romset/ (git mv), every path and source list updated, the
exception removed (.gitignore as before). Checked: git check-ignore
--no-index, a new file in romset/ is not ignored, a rom/ folder still is.

## src/runtime cleanup: dead video options and unused functions (2026-10-17)

Removed, none of it ran: Video's gpu_background option (set_gpu_background
had one caller left, the Vita's set_gpu_background(false), removed in the
Vita cleanup: gpu_background() and gpu_foreground() were always false;
their branch in screen_update and the conditions on them went), the
background/foreground_generation counters (incremented, never read) and
their accessors, Video::hud_at_edges_active, wide_margin (= margin()) and
the stretch_backdrop getter, Raster::hud_polygon_offset, rom_set_name().
Comments that still described the removed GXM compositor corrected.
Checked and kept: every #ifdef macro is still defined by some build (the
M2_DC_* ones by the Dreamcast port only: kept so it keeps compiling); the
transplanted MAME code keeps its unused helpers (rgb_t, spaces,
sample_rate) to stay comparable with upstream.
A wrong lead, checked before any change: screen_update does not compose
the 2D layers on the CPU on the Vita (an early return when the host draws
them), so there is nothing to gain there.
Checks: m2run 4,000 frames, daytona93 race_basic, srallyc srallyc_race,
vf2 vf2_fight: counts, last screen hashes and WAV as before the change;
ctest 23/23 x3; Vita host tests 15/15; test_vita_renderer.py; VF2 VPK
built. No performance change expected (none of it executed).

## Sound probe: the SCSP render split into slots, DSP and output (2026-10-17)

For VF2's sound on the Vita (the user's next target: find what costs).
The last measurement (2026-10-16, before RateConv) had the sound board at
12.6-17.5 ms a frame in a fight: 68000 3-4, SCSP render 9-12.6, of which
DSP ~2.6 and "slots/mix" 6.5-10 as one block. Now (M2_SOUND_PROFILE only,
Vita diagnostics builds): Scsp::render times the 32-slot loop of one
sample in 16 (kSlotEvery, as the DSP), and counts on every sample the
active slots and their pitch LFO, amplitude LFO, ring-buffer modulation
and 8-bit samples. perf.log gets a line "SCSP render per sound frame:
slots ~X ms (us a sample, active slots and their features), DSP ~Y ms,
output mix and the rest ~Z ms"; the clock reads of the new timing are
removed from advance and render as the DSP's are. Without the define
nothing changes: PC vf2_fight 4,000 frames, last screen hash
2e75355888f54820 as before.
Built: build_virtuafighter2/vita_diag/virtuafighter2_diag.vpk (VFTR90095).
Next: the user's perf.log of an attract and a fight, then optimise what
it points at.

## Vita: the menu in vita::Launcher; main() 360 lines (2026-10-17)

Step 4c, the last of the frontend refactor (platform/vita/HANDOFF.md).
frontend/main.cpp: 1,537 lines this morning, 360 now. Not run on the console.

## Vita: the game being played in vita::Session (2026-10-17)

Step 4b (platform/vita/HANDOFF.md). VF2 VPK built, Daytona compile check.
Not run on the console.

## tests/ sorted by subject (2026-10-17)

tests/ was 62 entries in one folder. Now: i960/, m68k/, runtime/{board,
video,sound,io}/ (as src/runtime), trace/ (+ the Lua plugin tests), app/
(desktop frontend), mobile/ (unmaintained port), oracles/ (MAME, Ghidra),
scripts/ (the Python tests of scripts/: unittest discover -s tests/scripts),
vita/ (+ vita/shims/{audio,gl,gpu,link,log}). Moved with git mv; relative
includes recomputed, Python tests' repository root one level up, every
reference (CMake, scripts, CI, docs, comments) rewritten. Removed on the way,
all dead: tests/vita_sdl_shim (its only user, test_vita_text, went on
2026-10-16), scripts/test_vita_gpu_lifetime.py (its test file was already
gone), PERFORMANCE.md's commands for those tests. .github/workflows/vita.yml's
host job compiled the two removed tests (broken since 2026-10-16): it now
configures the root build with -DM2_VITA_TESTS=ON and runs ctest -R vita and
the Vita Python tests (not run on GitHub here).
Checks: ctest 23/23 (vf2 build), Vita host tests 15/15, Python tests 44 OK,
test_vita_renderer.py. tests/mobile/test_mobile_rom_file.sh fails, as it did
before the move (a compile error in the test, mobile is not maintained).

## Vita: the frontend's measurements in vita::Diagnostics (2026-10-17)

Step 4a of the frontend refactor (platform/vita/HANDOFF.md). The user ran
the cleanup batch 1 VF2 VPK on the console: it works (the test menu fix of
4f89e87 included).

## Vita cleanup, batch 1: dead widescreen and renderer leftovers (2026-10-17)

Code the vitaGL-only build no longer reaches, removed (platform/vita/
HANDOFF.md has the list): the widescreen options and settings (kWidescreen
was false), the always-zero renderer counters and their perf fields,
render/text.h (the pre-ImGui font, unused), the gfx_* wrappers,
docs/history/. README.md rewritten for the current port.
Correction of the link play entry below: the communication board is
Revision A's (src/games/daytona.cpp: set "daytona" has .link = true), not
daytona93's; that entry and the Vita README said the opposite.
Checks: VF2 VPK built, Daytona compile check, Vita host tests 15/15,
Python tests. Not run on the console.

## src/runtime sorted by subsystem (2026-10-17)

64 files moved with git mv into board/, cpu/, tgp/, video/, sound/
(+ native/), io/, rom/ (the design doc's "Runtime layout" paragraph has the
list). Renamed on the way: gen_support.h -> cpu/i960_gen_support.h,
m2_replay_bus -> cpu/replay_bus, m2_tgp_board -> tgp/tgp_board, snd_board.h
(the interface) -> sound/sound_board.h, sound_board (Model 1) ->
sound/model1_board. Class and namespace names unchanged. Every include,
CMake list, recompiler's emitted include (m2recomp, m2tgprecomp,
m2sndrecomp, m2dsprecomp; fast_gen.py's), doc and comment path rewritten by
script; older HANDOFF entries keep the old paths. Also moved: the Vita
override (platform/vita/overrides/runtime/video/geo.*, its relative
includes one level deeper) and the test shim
tests/vita_audio_shim/runtime/sound/model1_board.h;
scripts/test_vita_renderer.py's reference copy follows the new paths.
Dreamcast and mobile ports: only their source lists rewritten so the paths
stay valid (not built, as before).
Checks, before and after on the same tree: m2run 4,000 frames, daytona93
race_basic, srallyc srallyc_race, vf2 vf2_fight: same i960/TGP/68000
counts, interrupts, last screen hashes (4ee8eddc7ce92bc2, 520443b3648918e3,
2e75355888f54820) and WAV md5. ctest 23/23 for each game; Vita host tests
15/15; test_vita_renderer.py; Python tests; VF2 VPK built.

## Vita: settings, storage and input out of frontend/main.cpp (2026-10-17)

frontend/settings.h/.cpp (VitaSettings: vita.cfg load/save/sanitize, now
taking the file's path), storage.h/.cpp (data folder paths, app_file,
load/save_bytes, load_nv/save_nv), input.h/.cpp (read_pad, map_input,
game_layout). main.cpp 1,537 -> 1,360 lines. New host test vita_settings
(round trip, ranges, unknown keys, temporary renamed), linked against the
desktop runtime library. VF2 VPK built; Daytona compile check; Vita host
tests 15/15. Not run on the console (vita.cfg format unchanged).

## Vita: render/renderer.cpp split by its sections (2026-10-17)

No code change (platform/vita/HANDOFF.md): 1,789 lines into renderer.cpp
(set-up, frame entry), model2_textures, model2_polygons, layers_2d,
gl_context, shaders, draw_list, ui_draw and gl_internal.h. VF2 VPK built,
Vita host tests 15/15.

## Vita: link play a build option, only Daytona's packages by default (2026-10-17)

Link play was tested on Vitas with Daytona only. scripts/games.py vita_link
(Daytona True, Sega Rally and VF2 False) sets DAYTONA_VITA_LINK;
build_vita.py --link / --no-link override it; the build summary prints
"Link play: in / left out". Off: frontend/main.cpp compiles no net/link.h
(TcpLink, SceNet), the SceNet/SceNetCtl stubs are not linked, the link rows
stay hidden; the link_* keys of vita.cfg are still read and written. The
menu rows still also need Game::link (daytona93 only), as before.
Checks: tests/test_build_vita.py (+2: the default per game, the flags);
Daytona compile check (link in); VF2 VPK built, link left out, no SceNet in
its link line. Not run on the console.

## Vita: platform/vita sorted by role (2026-10-17)

Pure moves (platform/vita/HANDOFF.md): frontend/, render/, audio/, system/,
net/, overrides/runtime/geo.*, docs/. Paths updated in the Vita CMake, the
root CMake's Vita tests (include root platform/vita), tests, scripts'
comments, docs. Vita host tests 14/14, test_vita_renderer.py, Python tests,
VF2 VPK built.

## VF2: the test menu's inline-argument helpers (m2recomp --inline) (2026-10-17)

On the Vita, VF2 stopped with "no recompiled code at 0005ef60" (the test
menu: 0x5ef60 is an entry of its handler table at 0x5fec0, pairs of handler
and name). seed_scan.py saw the pointer but dropped it: its code check wants
a ret or branch before any data, and the function calls balx 0x9444 with
its text right after the balx. The same pattern hid the code after every
such call from m2recomp too: the helpers return past their arguments, so
neither the analysis (it decoded the text as code, or stopped there) nor
the MAME harvest (the test menu never ran) knew the return addresses.
- i960::reach takes InlineHelpers (helper -> its arguments: a byte count,
  "words": packed text up to a word below 0x1000000, "cstring": bytes to a
  NUL, then a word boundary) and resumes after them. m2recomp --inline FILE;
  recompile.py, build_vita.py and seed_scan.py pass seeds/<set>_inline.txt.
- seeds/vf2_inline.txt: the printers 0x93c4-0x9460 (words), 0x4ec00 (8
  bytes, bx 0x8(r14)), 0x4f50c and 0x502a4 (cstring). From a survey of every
  balx with an absolute target in the program ROM (319 to 0x9444, 42 to
  0x4ec00, ...). 0x4f734 is not a helper.
- seed_scan.py also counts a balx as the end of a candidate's code check;
  then 70 seeds (the menu's handlers, the states its windows store, their
  callees) appended to seeds/vf2.txt; 0xa440 is still proposed and still
  wrong (left out). Reachable: 54,504 instructions -> 55,878 with --inline
  alone -> 57,669 with the seeds; seed_scan then finds nothing more.
Checks: PC exact vf2_fight 6,000 frames: last screen hash a0f1d889e5001976
as before; ctest 23/23; Python tests 43. 0x5ef60 and the returns after its
text (0x5ef10, 0x5f02c) have code. Vita VPK built (--fast-inaccuracy, not
--release). Not run on the console: the test menu there is untested.
Daytona and Sega Rally have no inline file: their code is unchanged.

## Vita: vitaGL only; the CPU frontend and the vita2d renderer removed (2026-10-16)

The Vita builds one renderer, vitaGL (main_gpu.cpp + gpu_gl.cpp). Removed:
the CPU-exact frontend (main.cpp, text.h, performance.h), the vita2d/GXM
renderer (gpu_fast.cpp/.h, gpu_memory.h, perspective_vertices.h,
pinned_vita2d.cmake), main_gpu.cpp's, imgui_vita.h's and gpu_text.h's
DAYTONA_VITA_GPU_GL conditions (the vitaGL side kept as it was), the CMake
options DAYTONA_VITA_GPU_FAST/GPU_GL, and build_vita.py's --gpu-gl and
--gpu-fast (an error now: no flag needed). Host tests of the removed code
gone: vita_imgui (vita2d menu), vita_gpu_memory, vita_gpu_lifetime (+ its
vita_gpu_*.inc), vita_tessellation, and the unbuilt test_vita_text /
test_vita_performance; tests/vita_gpu_shim/vita2d.h. system24_upload.h's
colour form (the vita2d upload) stays: scripts/test_vita_renderer.py uses
it as the index form's reference. Docs: command lines without --gpu-gl
(HOW_TO_COMPILE_RELEASE.md, platform/vita/README.md, docs), GPU04-06.md
marked historical, libvita2d's licence record marked removed (root and Vita
THIRD_PARTY.md), the design doc's Vita buffering paragraph updated. Widescreen
was drawn by the vita2d renderer only: the Vita has none now (it had none in
the vitaGL builds already).
Checks: tests/test_build_vita.py (26: the flags rejected, nothing GPU_* passed
to CMake); Vita host tests 14/14 (-DM2_VITA_TESTS=ON); VF2 diagnostics VPK
built with no renderer flag (build summary: vitaGL). Not run on the console.
The three release VPKs built just before (same vitaGL code, with --gpu-gl):
build_daytona/vita/daytona.vpk, build_segarally/vita/segarally.vpk,
build_virtuafighter2/vita/virtuafighter2.vpk.

## Dreamcast and mobile ports marked unmaintained (2026-10-16)

platform/dreamcast -> platform/dreamcast_unmaintained, platform/mobile ->
platform/mobile_unmaintained (same depth: their ../.. paths and the
Android gradle paths still hold). Their READMEs open with an
"Unmaintained" notice; README_fork.md's port table says so. They are not
built or tested; shared changes only keep them compiling where
unavoidable. platform/dreamcast/scripts/fast_gen.py, which the Vita build
runs (--fast-inaccuracy's direct work-RAM pass, --fast-gen), moved to
scripts/fast_gen.py; build_vita.py, tests/test_build_vita.py and
build_dreamcast.py point there. Paths updated in the root CMakeLists (iOS
sources), .gitignore, docs, comments; the older HANDOFF entries below keep
the old paths. Checks: Python tests (44), PC configure, VF2 Vita
diagnostics VPK built through the moved script.

## Wait-loop skip made general (M2_SPIN_SKIP, on by default), VF2's added (2026-10-16)

The Dreamcast's frame-wait skip (M2_DC_SPIN_SKIP, Daytona Rev A 0x1394) is
now the runtime's M2_SPIN_SKIP, ON by default (CMake option M2_SPIN_SKIP,
Vita DAYTONA_VITA_SPIN_SKIP; the Dreamcast Makefile and tools CMake renamed
to keep them building, not built here). Each game declares its loops in its
record: rt::Game::spin[2] (game.h SpinWait: ip of the pass's ldob, the RAM
byte, instructions a pass, the register; kinds ByteIsReg, the old Daytona
loop, and ByteZeroTimer, VF2's). M2Board::spin_skip, at the pass's read:
when the pass will loop, skips whole passes up to just before the next
lockstep event and, for ByteZeroTimer, before the first pass whose timer
read (the dev_read expression at its instruction count; 20 bits, checked
not to wrap over the range; binary search) would leave. The fast byte
read path excludes the declared addresses (it excluded 0x500000 always).
m2run prints the instructions skipped; perf.log's work line too.
VF2: 0x4cb7c, 7 instructions a pass, threshold r13, timer 2 read 2
instructions in. A wrong turn first: timer 3 (ldl's second word) gave
different frames from frame 1000 and 30 more interrupts; the timer region
is not a burst region, so both words of the ldl read 0xf00008 (timer 2).
A wrong entry changes the game silently: check each new one against a
build with M2_SPIN_SKIP=OFF.
Checks (PC, VF2 with M2_SPIN_SKIP OFF vs ON): vf2_fight 4,000 frames
40/40 dumps, WAV, i960/TGP instructions and interrupts identical, 14.9 M
instructions skipped (7.5 %); vf2_attract 8,000 frames 80/80, identical,
22.5 M skipped (5.2 %). Daytona race_basic 4,000 frames identical (no loop
declared for daytona93: 0x1394 is not that loop there). ctest 23/23 for the
three games. Daytona Rev A's entry not re-checked here (no ROM set; its
Dreamcast check: dcmemcheck matched the desktop at 100 checkpoints).
PC wall time no better (16.7 vs 18.7 s, within the run-to-run spread seen
today). Measured on the Vita (VF2 diagnostics, perf_loop.log against
perf_tick_opti.log and perf_last.log, same play, aggregates): 3D windows
mean 56.79 -> 57.13 fps (53.80 before ticks()), worst 54.59 -> 55.40,
windows under 55 fps 2 of 17 -> 0 of 17, game logic in the heavy windows
2.07 -> 1.76 ms a frame for more work (76,531 i960 instructions a frame,
5,000-27,000 of them skipped where the loop runs), audio gaps 9 -> 4.
Steady fight unchanged (core 0 14.6 ms: the vblank handler's i960 and TGP
are the limit now).

## M2Board::ticks() without the 64-bit division (2026-10-16)

Why: the VF2 profile (extern/result_perf_vita/vf2 perf_supra_probe.log,
--tgp-profile --i960-profile) shows the fps drops (43-55 fps windows) are
the game's timed wait at 0x4cb7c-0x4cba0 (ldob 0x500000 until non-zero,
ldl 0xf00008 = timers 2 and 3, out at expiry or below a threshold): 0
passes a frame in the 57 fps windows, 400-1,900 in the slow ones, ~1.9 us
a pass on the Vita (3 bus calls: 0x500000 is kept off the fast read path,
and each running-timer read calls ticks(), a 64-bit division). In a fight
without it the i960 is ~7.2 ms (40,000 instructions, 24,600 bus calls, 333
direct) and the TGP ~4.2 ms (63,000 instructions; its FIFO wait loop
035-03e is 16 % of them; FP ~0.2 ms); ~1,600 device reads a frame even
then (how many are timers: not counted).
Change: ticks() goes through rt::RateConv<kTicksPerFrame, kInstrPerFrame>
(mutable member; io_now() mostly grows; a step back takes the 64-bit
expression). Checks: m2run 4,000 frames, VF2 vf2_fight and Daytona
race_basic: 40/40 frame dumps, WAV and i960/TGP/interrupt counts identical
to before; ctest 23/23 for VF2, Daytona and Sega Rally. PC wall time
16.1 -> 17.4 s (VF2; x86 divides fast, not the target). Vita VPK built
(VF2 diagnostics, with ea210b2's render_to change).
Measured on the Vita (VF2 diagnostics, perf_tick_opti.log against
perf_last.log, both with the sampled sound probe; same play, scenes not
window-aligned after window 3, so aggregates): 3D windows mean 53.80 ->
56.79 fps, worst 46.59 -> 54.59, windows under 55 fps 10 of 19 -> 2 of
17; windows with game logic over 1 ms: 3.78 -> 2.07 ms a frame for the
same work (68,963 / 67,795 i960 instructions a frame); boot window 41.8
-> 55.8 fps; reference audio gaps 70 -> 9. Steady fight windows (no wait
loop): core 0 unchanged (14.66 / 14.63 ms); 68000 4.24 -> 3.58 ms a frame
(render_to, ea210b2). Far above the 0.2-0.8 ms expected: the timer reads
cost more than the division estimate (or are more numerous).
Next: skip the passes of that wait loop exactly (the Dreamcast frame-wait
skip, M2_DC_SPIN_SKIP, made general and on by default).

## SCSP board: render_to without the 64-bit division (2026-10-16)

ScspBoard::render_to ran count * Scsp::kRate / kIps (a 64-bit division,
__aeabi_uldivmod on the Cortex-A9, ~150-250 cycles) on each of its ~1,200
calls a frame in a VF2 fight (every SCSP register access). New
runtime/rate_conv.h rt::RateConv<N0, D0>: the fraction reduced at compile
time (1225/22941 here), the last count, quotient and remainder kept; a
step forward is a multiply and a 32-bit division by the constant (umull);
a step back or past 32 bits takes the 64-bit expression (still in the
object, on that path only). Checks: test_rate_conv (new: 15,000,000
counts over three fractions, growing, going back, jumping: all equal to
count * N / D); VF2 PC: m2run 4,000 frames of vf2_fight.txt, WAV
byte-identical (sha256 995a9904...); ctest 23/23. Vita VPK built; not yet
measured (expected ~0.4-0.7 ms a frame on core 2). The YM3438/MultiPCM
board (Daytona) has the same divisions in render_fm_to/render_pcm_to and
ym_clock_at: not changed, not measured.

## Vita perf.log: slow loops, sound board probe; PACK and VF2 measured (2026-10-16)

PACK with CLZ (cbd7cbb), measured on the Vita (VF2 diagnostics, attract,
extern/result_perf_vita/vf2 perf_pack.log against perf_ref and perf_modif,
same scenes in windows 0-7): sound board 13.1-14.1 -> 12.2-13.2 ms a frame
in 3D windows (-0.89 ms, -6.5 %), -0.44 ms without 3D; the two baselines
(same sound code) differ by 0.03 ms. fps, gaps, core 0 unchanged. Larger
than the 0.1-0.25 ms expected: ~130 cycles a call saved.

New in perf.log (diagnostics builds; no cost elsewhere):
- core_profile.h: "SLOW loops", the 8 longest loops of each window, each
  with its time into the window, its sections of 0.3 ms or more (largest
  first) and a LoopNote (board frames, i960/TGP instructions, textures
  built, texels, changed, deferred; set in main_gpu.cpp).
- runtime/sound_profile.h (M2_SOUND_PROFILE, defined by the Vita CMake with
  DAYTONA_VITA_DIAGNOSTICS): per sound frame, ScspBoard::advance time, one
  render_to in 16 timed (scaled by samples), one DSP step in 16 timed
  (scsp.cpp), 68000 instructions, dispatches (m2sndrecomp emits
  M2_SND_DISPATCH() after dispatch:, nothing without the define: the host
  code must be regenerated), SCSP register reads/writes, render_to calls.
  The clock read (0.70 us on the Vita) is measured at game start and
  removed. main_gpu.cpp folds and clears it after each sound worker join.

VF2 on the Vita (perf_probe.log, 30 windows, a probe that timed every
render: ~0.9 ms a frame of clock reads on core 2; the sampled one now
costs ~0.1 ms, not yet run on the console), per sound frame in a fight:
board 12.6-17.5 ms = 68000 3-4 ms (14,357 instructions: ~250 ns each) +
SCSP render 9-12.6 ms, of which DSP ~2.6 ms (3.4 us a sample) and
slots/mix 6.5-10 ms (1.3 ms in attract, few voices). 68000: 180
dispatches a frame (DIV-OPT point 10, the switch table: drop), ~950 SCSP
register accesses, ~1,200 render_to calls (640 rendering 1.2 samples
each), each a 64-bit division (point 5: worth doing).
Windows 22-28: core 2 at 95 % (16.8-17.5 ms against 17.4), core 0 waiting
for it ("sound sync") 2-9 ms on single frames, 54 fps.
The end-of-fight lag (the character talking, windows 23-24): textures
are not it (56 built 64x64 in 49 frames, 0.21 ms a frame, worst 1.4 ms;
483 of 539 rewritten kept); the slow frames (31-33 ms) are i960 game
logic 17-20 ms (120,000-150,000 instructions) plus the sound board at
16-18 ms, the main core waiting 4.4 ms a frame on it.
Transitions (2D screens): single frames of 45-75 ms with the 2D worker
at ~30 ms on core 2 (main waiting on it, "upload"), the tile cache 10-18
ms and the sound board up to 64 ms behind it.
Diagnostics-only artefact: one "log" frame every 5 s, 18-35 ms, growing
during the run (14-18 ms already in perf_ref); not in normal builds.
Next: render_to's 64-bit division (point 5), then the SCSP slots
(Scsp::render, UpdateSlot: 6.5-10 ms), the heavy i960 logic frames
(--i960-profile).

## Vita texture probes: diagnostics builds only (2026-10-16)

gpu_gl.cpp: the clock reads around texel decode, upload and the texture RAM
check of a rewritten source, and the texture_probe_ counters (builds,
texels, kept, changed, rewritten), now run under kGlLog
(DAYTONA_VITA_DIAGNOSTICS) only; the normal and release builds no longer
pay them. build_begin/build_end (rec_textures, last_texture_us_) are as
before. Both VF2 VPKs (normal, diagnostics) built.

## Vita builds: vita-elf-create's "segment 1 overlaps", padded (2026-10-16)

vita-elf-create appends its SCE data (4,308 bytes for VF2) after the code
segment, in the gap before the data segment (64 KB aligned). A VF2 build
whose code ended 2,120 bytes before it failed ("Cannot allocate 4308 bytes
for SCE data at end of segment 0; segment 1 overlaps", then a segfault);
other builds had ~22 KB by chance. build_vita.py now configures with
DAYTONA_VITA_TEXT_PAD=0 and, when the build fails with less than 8 KB in
that gap (readelf of the linked daytona_vita), reconfigures with
gap + 8 KB of read-only padding (platform/vita/CMakeLists.txt writes
text_pad.cpp, kept by --undefined) and builds again: the data segment
moves one 64 KB step. Checked: VF2 diagnostics builds relinked with 10,312
and 9,792 bytes and ran on the console; the normal VF2 build needed none.

## SCSP DSP: PACK's exponent with one CLZ (2026-10-16)

src/runtime/scsp.h dsp::PACK counted temp's leading zeros (24 bits, at
most 12) bit by bit, as MAME; now std::min(12, countl_zero(temp << 8)).
GCC for the Cortex-A9 kept the loop; now one clz (checked in the Vita
dsp_gen.cpp object: 4 clz, no loop). Shared by the DSP interpreter and
the recompiled DSP. THIRD_PARTY.md notes the change.
VF2's DSP program has 4 PACK (memory writes) a sample, ~3,070 a frame,
not the 6-12 extern/DIV-OPT-HANDOFF.md assumed: expected gain ~0.1-0.25 ms
a frame on core 2 (sound board 13-14 ms a frame in a fight), likely within
the window-to-window noise. Checks: test_scsp_pack (new: every 24-bit val
and 8 outside, as MAME's loop), scsp_dsp_random(-return), VF2 PC ctest
22/22. Vita VPK built (--gpu-gl --diagnostics --fast-inaccuracy); not yet
measured on the console.
Review of extern/DIV-OPT-HANDOFF.md for VF2 (not committed, ideas only):
in a fight core 0 is the TGP (vblank handler ~8 ms, ~68,000 TGP
instructions, no division in tgp_gen.cpp) and core 2 the 68000 driver;
its point 3 (RBL/RBP as locals) gives ~0.02 ms (the Vita code reloads them
only after the 4 writes a sample); point 9 has no DIVS in VF2's sound
program; points 4 and 10: GCC does not compress the switch tables (i960
chunk_000: 8,237 tbh entries for 1,500 cases; 68000: 9,586 4-byte entries,
~38 KB, for 2,312 cases), memory only unless dispatches per frame are in
the thousands (not measured).

## Vita: the z-clip division in check_culling, tried and dropped (2026-10-16)

Tried: under M2_FAST_GEO, check_culling's float divide for the master z
clip replaced by two integer compares against the bounds of the rejected
min_z bit patterns (found by bisection with the original expression,
recomputed when master_z_clip changes). Host check: 106,182,000 patterns
over the 255 clip values gave what the division gives.
Vita (VF2, --gpu-gl --diagnostics --fast-inaccuracy, extern/result_perf_vita
/vf2 perf_ref.log vs perf_modif.log), fight windows at >= 1,400 polygons a
frame: core 1 display list parse 6.385 +- 0.180 us a polygon (11 windows)
-> 6.394 +- 0.189 (10 windows); fps and core 0 unchanged; core 0 blocked
on 0 joins in both. Expected gain was ~25-30 cycles a polygon (~0.06 us,
~1 % of the 6.4 us), below the window-to-window noise, and core 1 is not
the bottleneck. Reverted (geo.cpp, geo.h, test_geo_zclip not kept).
What not to re-propose: removing the z-clip divide (or other single
per-polygon divides) in check_culling; find where the 6.4 us a polygon
goes first (--tgp-profile build, or probes in display list parse).

## Vita: texture builds and the System 24 tile cache made cheaper (2026-10-15)

From the probes (perf_prob_text.log, VF2, Vita): 78 % of texture builds
remade a texture whose texture RAM had been rewritten with the same words;
decode was 89 % of a build (114 ns a texel), glTexImage2D 11 %; the tile
cache cost 1.25 ms a frame in a fight for ~10 tiles actually rebuilt (the
compare of all 16,384 tiles and the 64 KB copy, every frame: the game writes
its scroll registers every frame).
- A (gpu_gl.cpp): mark_rewritten_sources marks a source instead of
  dropping it; at its next use index_texture_hash of its texture RAM words
  is compared with the one stored at the build: same, kept (the generation
  moves on); different, rebuilt. source_cache_ holds mutable pointers.
- B (texel_index.h): decode_index_texture reads a 32-bit word once for its
  8 texels (4 x, 2 rows) through a 16-entry L/A table, and returns the hash;
  textures above 512 texels (subsampled) keep the texel loop.
- Transparent fill: % w / % h replaced by & (w - 1) / & (h - 1) (sides are
  powers of two; the Cortex-A9 has no divide instruction).
- C (video.h/.cpp, m2_board.cpp): the board reports the offset of each
  changed tile RAM / character RAM byte; Video keeps 64-byte block and
  256-byte page bitmaps and update_tracked_tiles compares, rebuilds and
  copies only those (all layers when a character changed). Without write
  tracking (replays) the full comparison is kept. build_layer's body is
  build_tile. A bug the new test caught: character_dirty_ marks left by
  the first (full) update were never cleared by the tracked path
  (chars_marked_).
Checks: test_vita_texel_index (decode_index_texture = texel_index on 304
textures, the fold included; hash), test_vita_tile_cache (new: tracked =
full comparison over 600 frames of random writes: pixels, tile
generations, counts), Vita host tests 18/18, PC ctest 21/21 x 3 games with
last screen hashes unchanged, the AND fill = the modulo fill on 100
textures (scratch). PC bench: decode 1.35 -> 0.32 ns a texel, hash check
0.11; tile cache 59.8 -> 6.5 us a frame.
Vita (VF2 diagnostics, same 797 builds in both sessions): decode 114 ->
77 (A+B+C, perf_after_modif_A_B_C.log) -> 37 ns a texel (+ the AND,
perf_modif_modulo.log); 73 % of rewritten textures kept; worst texture
frame ~30 -> 10.8 ms; tile cache 1.25 -> 0.10-0.12 ms a frame. Probe fix:
"deferred" now this frame's count (it read a per-frame counter as a total).
Left: the 46-50 fps windows are frames where the game logic runs ~100,000
i960 instructions (up to 19-21 ms); next an --i960-profile look at them.
The texture budget (32 builds / 128 K texels a frame) could rise now.

## Vita probes: texture builds and the System 24 tile cache (2026-10-15)

Diagnostics only, to choose how to make texture builds cheaper (VF2: up
to 14 ms in one frame at transitions, under the existing budget of 32
builds / 128 K texels a frame). perf.log, under CORE 0:
- 2D video update: "of which tile cache (palette incl.)" and "of which
  palette" (VideoProfile::palette, new; timed only with a profile clock);
  a line "tile cache: N tiles and M characters rebuilt per frame".
- polygon recording / texture builds: "texture decode" (texel_index loop
  + transparent fill), "texture upload (glTexImage2D)", "identical
  rebuilds" (whole builds that remade a texture dropped for a texture RAM
  write, with the same texels: a 64-bit hash of the decoded texels, kept
  per dropped key; computed after the timings, diagnostics builds only);
  a line "textures: built, texels each, us per 1000 texels decode and
  upload, frames with builds, most in a frame, identical, dropped,
  deferred, cache resets".
New csv columns: tex_decode, tex_upload, tex_identical, palette_2d,
tile_cache_2d (before geo_parse). Release builds: two clock reads more
per texture build, nothing else. Checked: Vita host tests 17/17, PC
ctest 21/21 (vf2_fight output unchanged), VF2 diagnostics VPK built, GXM
compile check. Next: the user's perf.log in a fight, then pick among
word-wise decoding (8 texels a 32-bit word), decoding straight into the
texture's memory (texture_memory, no scratch copy, no glTexImage2D copy),
keeping a dropped texture whose texels did not change, the transparent
fill merged into the decode.

## VF2: the fight crash (missing script stubs) and a seed clean-up (2026-10-15)

On the Vita a fight went back to the menu: "no recompiled code at 00034e28"
(--fast-inaccuracy VPK; 0x355e0 the time before). Both are two-instruction
stubs (lda/shlo imm,g0; b 0x1d194) between the script records at
0x34d78-0x39a90 and 0x6c870-0x6c960, entered through computed pointers: no
word holds their address, so neither the MAME harvest (it saw 1) nor
seed_scan.py finds them. A scan of the program ROM for every b 0x1d194
gives 88; 87 were missing: all seeded. The other shared-branch stubs
(0x277fc, 0x3b310) already had code.

The seed file itself (2,378 seeds) held 1,093 harvested interrupted
addresses inside straight-line code (Daytona 17, Sega Rally 9: VF2 spends
its time in short polling loops). Not wrong as code, but each was a block
start for m2recomp --fast_inaccuracy: VF2 ran 1.3 instructions a block
(37,300 blocks a frame over vf2_fight; Daytona 5.0, Sega Rally 6.4). Three
regions (0x866c, 0x115f0, 0x119d0) were reached only through them: their
function starts were missing (0x8630, 0x11590, 0x115a8, 0x11970, 0x119c0;
each after a ret or data), now seeded. One seed was wrong: 0xa440, the
second word of lda 0x36006c6c at 0xa43c (decoded a constant as code).
Removed with the 1,093: the recompiler reaches the same 54,504
instructions (54,444 + the 5 functions' 61 - 0xa440's 1). Now 1,289 seeds.
Daytona's and Sega Rally's mid-flow seeds hide nothing (0 instructions
lost without them), no wrong seed.

Checks: PC exact vf2_fight 6,000 frames: counts and last screen hash
a0f1d889e5001976 as before; ctest 21/21. Fast mode: 14,858 blocks a frame
(3.3 instructions), the fight runs through. Vita (user, diagnostics VPK,
extern/result_perf_vita/vf2/perf_after_modif.log, 30 windows): no return
to the menu. Fights mostly at 57.3 fps (the morning's perf_base.log:
35-40; the DSP recompilation and --fast-inaccuracy). Core 0 at 85-97 % in
a fight at full speed: vblank handler 8.7 ms (40,000 i960 + 70,000 TGP
instructions), polygon recording 3.4 (vertex writing 2.15), 2D update
1.35, sort 0.67. Drops to 44-52 fps come from single heavy frames at
transitions: game logic up to 19 ms (~100,000 i960 instructions), texture
builds up to 14 ms, 2D update 25 ms.

Wrong turn, reverted: m2recomp --fast_inaccuracy no longer making seeds
block starts. Measured on the Vita (perf_A.log: old seeds, perf_B.log:
the change, perf_after_modif.log: clean seeds): the same scene costs the
same in all three (vblank handler 8.5-8.9 ms, game logic 0.2 ms): in a
fight the i960 is 0.2 ms of logic, the TGP fills the vblank handler. With
the clean seeds the change no longer alters VF2's blocks either.

## Vita reference audio: one rate conversion, in the callback (2026-10-15)

platform/vita/audio.h no longer uses SDL_AudioStream. push() (sound core)
copies the board's samples at their own rate into two FrameRings (PCM, FM;
audio_rate.h, half a second each, cleared past 250 ms as before); the SDL
callback converts them straight to 48 kHz with StereoResampler, ratio =
board rate / 48 kHz x AudioRate's speed correction. Before, SDL resampled
to 48 kHz in SDL_AudioStreamPut on the sound core and the callback
interpolated a second time for the speed. Silent FM is still not queued
(padded with silence when it starts again); the FM resampler restarts from
silence when its queue is empty. StereoResampler is now cubic (Catmull-
Rom; at ratio 1 a copy three frames late instead of two).

Measured on the PC (scratch tools, not committed): VF2 fight audio at the
board's 44.1 kHz (m2run, 6,000 frames), the old path (host SDL 2.30
AudioStream) against the new. Band power against SDL: linear interpolation
-0.5 dB at 4-8 kHz, -1.3 at 8-12, -2.5 at 12-16, -3.1 at 16-20; cubic
-0.1, -0.4, -1.1, -1.4 (those bands are 9-34 dB below the total). Above
22 kHz SDL2 leaks images too (-48.5 dB, cubic -46.2). Cost per emulated
frame: SDL put 15 us, linear 6.6 us, cubic 13.9 us: on the PC (SSE in
SDL) no gain. The Vita's 1.6 ms "sample conversion/queue" (VF2's
perf_base.log) is 100x the PC figure: not explained yet (lock wait?).
NOT measured on the Vita: next, a --diagnostics VPK before/after, same
session, the "sample conversion/queue" line. Revert if no gain there.
Tests: vita_audio_rate (FrameRing; a 1 kHz tone 44.1 -> 48 kHz within
5e-5), vita_audio_worker (the queueing gate moved to the device lock).

## Vita controls per game (2026-10-15)

The Vita had two fixed schemes (driving, fighting: rt::ControlScheme). Now
each game's record lists its own controls (Game::control_list:
rt::ControlDef {control, name, default button}, src/runtime/cabinet_controls.h,
no dependencies so the Vita host test includes it alone), from the cabinet's
inputs in MAME's ports:
- Daytona: accelerate, brake, steer, shift up/down, views 1-4 (Cross,
  Circle, Square, Triangle), start, coin: as before.
- Sega Rally: the same less views 2-4, plus HAND BRAKE (Cross: rt::Inputs
  handbrake 0xff, the analog port srallyc reads at IN2 case 3) and one VIEW
  (Triangle, in0 0x20: srallyc has a single VR button).
- VF2: joystick (D-pad or left stick: steer/accel/brake ends, as the fight
  wiring reads them), PUNCH / KICK / GUARD (Cross / Circle / Square, in0
  0x20/0x40/0x80), start, coin.
platform/vita/controls.h: vita::Layout over the list; Bindings hold a button
for every rt::Control, 0 for the ones the game lacks; sanitize(layout) clears
those and resets to the game's defaults on an invalid, missing or duplicated
binding (so an older vita.cfg of Sega Rally or VF2 comes back as the new
defaults once; Daytona's keys are unchanged). Controls::set_layout picks the
wheel, pedals or joystick handling. main_gpu.cpp: the Controls page lists the
game's controls in its order with their names; steering curve and invert
hidden for a game without a wheel; vita.cfg keys bind_handbrake, bind_up..
bind_guard added. The desktop app (src/app) keeps its own bindings: not
changed.

Checks: test_vita_controls rewritten (Daytona, Sega Rally and VF2 layouts:
hand brake, unused buttons doing nothing, joystick, cross-layout sanitize;
g++ -fsanitize=address,undefined: passes). PC builds of the three games
rebuilt, ctest 21/21 each. Vita VPKs of the three (vitaGL, --fast-inaccuracy)
built, and the GXM path (--gpu-fast --compile-check) compiles; not yet tried
on the console.

## SCSP DSP program recompiled (m2dsprecomp), for Virtua Fighter 2 (2026-10-15)

Why: on the Vita, VF2 ran at 35-40 fps with core 2 (sound + 2D worker) at
~100 %: sound board 88-93 % of it, 17-26 ms a frame against the 17.4 ms
budget, the main core waiting 7-18 ms a frame on it ("sound sync /
dispatch"); the 2D worker 1-6 % (extern/result_perf_vita/vf2/perf_base.log,
not committed). PC probes (rdtsc in ScspBoard::run_to/render_to and
Scsp::render, temporary, not committed), 6,000 frames: VF2 fight 68000
3,059 ms, SCSP render 2,526 ms of which DSP 1,876 (408 ns a sample, an
82-step program on 99.7 % of samples), slots 508 (11.5 active); Sega Rally
race 68000 1,341, render 750, DSP 39 (its program is empty: 0 steps), slots
601 (14.2 active). The DSP is VF2's difference.

The program: the sound driver copies it from its program ROM into MPRO at
boot (sound_program.bin 0x4a48, 82 steps, CRC-32 0x9CDF098E; logged at
ScspDsp::Start in m2run: loaded twice, the same words, in attract and in a
fight; Sega Rally loads an empty one). Game::dsp {offset, steps, crc};
m2import writes dsp_program.bin (rt::dsp_program, CRC-checked; empty for a
game without one). tools/m2dsprecomp writes <set>_dsp/dsp_gen.cpp: one block
per step, the fields resolved, the interpreter's operations in its order
(memory only on odd steps, the early return on an input index past 0x31);
COEF, MADRS, TEMP, MEMS and the RAM stay run-time state. The generated file
registers the program (snd::scsp_dsp_native, a static initializer): no
symbol the runtime needs, so builds without the file (other games, the
Dreamcast and mobile builds, the tools) keep the interpreter.
ScspDsp::Step runs it while MPRO's first steps x 4 words and LastStep are
the program's, checked again after every MPRO write and Start (a reload in
progress, or another program, runs on the interpreter: never a wrong sound).
The interpreter's helpers (sext, PACK, UNPACK) and read_word/write_word
moved to scsp.h, unchanged, shared with the generated code. recompile.py
runs it (a cache without dsp_program.bin is imported again); build_vita.py
copies <set>_dsp; CMake and the Vita CMake add dsp_gen.cpp when present.

Checks: tests scsp_dsp_random and scsp_dsp_random-return (ctest; no ROM):
m2dsprecomp --random / --random-return 128-step programs against the
interpreter, 20,000 samples with new inputs and coefficient changes, the
whole DSP state and the 512 KB sound RAM identical after every sample.
VF2, 8,000 frames of attract: WAV identical byte for byte to the
interpreter's, 16/16 frames and every count the same. DSP time (rdtsc,
temporary probe, 4,000-frame fight, twice): interpreter 980-1,025 cycles a
sample, recompiled 236-238 (x4.2). Daytona and Sega Rally parity unchanged.
On the Vita (user): the sound is right with the recompiled DSP. Left on
the sound core: the 68000 (VF2's driver costs twice Sega Rally's on the PC)
and the slots. Sega Rally rebuilt through the new chain (dsp_program.bin
empty, dsp_gen.cpp registers nothing): race_basic parity 13/13, VPK built.

VF2 --fast-inaccuracy (games.py vita_fast): recompile.py --fast_inaccuracy
on the PC, vf2_fight 6,000 frames: the frames differ from the exact code's
from frame 1000 (the interrupts a few instructions late move the game's
timing: the same scenes, other animation phases; Sega Rally's stayed
identical) and the run stopped at 0x355e0, an entry point the exact code
does not reach there: seeded (seeds/vf2.txt 2,291). Then vf2_fight and
8,000 frames of attract run through; the fight plays (Akira against Lau,
round 1 at 4000), sound RMS -21.8 against -21.7 dBFS exact. Vita VPK built
with --fast-inaccuracy; not yet run there.

## Vita vitaGL: textures kept from rewritten texture RAM (2026-10-15)

Reported on the Vita (vitaGL), Virtua Fighter 2: once a game starts, every
fighter wore the attract's old man's textures (Shun Di), in fights too; the
PC was right. Cause: GpuGlRenderer caches each index texture built from
texture RAM, keyed by the polygon's texture header (size, position) only;
VF2 loads each fight's characters into texture RAM where the attract's were,
so the Vita kept the old pictures (the desktop GPU renderer re-uploads
texture RAM whenever VideoMem::tex_generation moves). Daytona and Sega Rally
hardly write texture RAM, hence never seen. Measured (m2run, temporary
probe, not committed): vf2_fight writes texture RAM on 567 of 6,000 frames,
in bursts (attract scenes, character select 1414-1562: 940,000 writes,
round ends ~1,365 a frame), none while fighting: clearing every texture on
a write frame would rebuild them all at each round change.
Fix: M2Board keeps tex_generation at each 4 KB page's last write
(VideoMem::tex_page_generation, 2 x 512 pages, set in tex_write); each
gpu_gl Source records the generation it was built at and the pages it reads
(texel_index's addressing, per row pair, both fold halves);
drop_rewritten_sources, once per frame and only when tex_generation moved,
deletes the sources one of whose pages was written since (source_cache_
cleared, refilled on use). Checked on the Vita by the user: right
textures. Daytona parity unchanged (race_basic 3,000 frames, 13/13).
Not done: gpu_fast (GXM) caches its sources the same way and has the same
fault (not reported: not tested on vf2).

## Virtua Fighter 2 (vf2, Version 2.1; Model 2A-CRX), one player (2026-10-15)

New game `virtuafighter2` (scripts/games.py; build_virtuafighter2/, Vita
virtuafighter2.vpk, title ID VFTR00095, images platform/vita/logo_livearea/
virtuafighter2/): src/games/vf2.cpp, seeds/vf2.txt (2,290), seeds/vf2_snd.txt
(144), scripts/inputs/vf2_attract.txt and vf2_fight.txt. MAME runs it on its
`model2a` machine, which is Sega Rally's board without the drive board and
analog inputs: no runtime change was needed for the board. Differences are
the game's: program 512 KB in ROM, run from ROM (no --alias, only
--no-mirror); no copro data ROM; textures with a gap (0x400000-0x7fffff,
0xff in MAME, 0 here; not read); the TGP program in the program ROM at
0x3e90, 3,802 words, CRC 0xFD8D0383 (equal to MAME's copro_tgp_program after
boot, dumped with a Lua -autoboot_script); port E drives a billboard (its
bytes go to the drive board queue, unused).

State, all against MAME (scripts/m2_check.sh, SET=vf2):
- attract 600 frames: m2native and m2replay MATCH (71.3 M instructions).
- vf2_attract 1,800 frames: m2native MATCH, 213.1 M i960 instructions, 3,680
  interrupts; TGP 55.7 M, 750,192 output words; 696,630 polygons; 3D layer
  809/809 and screen 1,796/1,796 frames identical; sound 22.5 M 68000
  instructions, 33,055 register checkpoints MATCH.
- vf2_fight 6,000 frames (2 coins, start, punch/kick: Akira against Lau):
  m2native MATCH, 626.0 M i960 instructions, 12,222 interrupts; TGP 332.8 M,
  4.79 M output words; 7.07 M polygons; 3D 4,951/4,951, screen 5,996/5,996;
  m2sndcheck MATCH, 72.9 M instructions, 116,860 checkpoints, 99,100
  interrupts. Only the measured buffer RAM reads differ (the TGP's mailbox
  timing, as Daytona's).
- m2run, 8,000 frames: 434.7 M i960 instructions with the idle loop (1,212.4
  M without: every frame to the cap), 206 frames/s on this PC.

What it took:
- i960 templates from MAME's executor (tools/m2recomp): spanbit/scanbit
  (0x640/0x641), subc (0x5b2, 64-bit math routines at 0x6cbe8-0x6da18),
  dmovt (0x644; MAME's AC &= 0xfff8 also clears AC's upper half).
- TGP: one word MAME marks unimplemented (0x1b4: 3c0f0000, opcode 0f
  subop 7): MAME runs only its ALU part, as clr1/set; m2tgprecomp now does
  too for subops 4-7 instead of failing.
- Seeds: MAME's harvest (branches.txt targets) of attract runs of 1,800,
  3,600 and 8,000 frames and an 8,000-frame fight, plus seed_scan.py: 54,252
  instructions. Sound: two "jmp (d16,pc)" tables (0x6013de, 128 entries;
  0x601fbc, 16), found from M2TRACE_SNDPCS (needs M2TRACE_SNDLOG set too,
  /dev/null will do): 2,312 instructions, every address MAME's 68000 ran.
- Idle loop 0x10f98-0x10fa3 (ldob 0x500000 / cmpibe, as Daytona's 0x1394),
  found as the hottest block of recompile.py --i960-profile (62 %).
- Lockstep: MAME holds immediate interrupts while a multi-word load waits
  on the TGP's empty output FIFO (burst stall) and takes them when it
  completes, IP past it, before counting it; ours never stalls. Seen at
  0x7dc4 (ldq from 0x00884000), 127 M instructions in. Lockstep::apply now
  lets such a line wait one instruction when MAME's take is at another IP,
  and on_take accepts the count one higher (deferred_). Test harness only;
  the game's behaviour is unchanged (the clockless TGP, design doc).
- m2_check.sh ran m2sndcheck without --scsp (the Model 1 board's map) on
  SCSP games, so it always failed there ("no recompiled code at 012000");
  it now passes --scsp and the samples when scripts/games.py says --scsp.
  Sega Rally's sound had never been checked that way through m2_check.sh.
- Sound: the driver's voice table is laid out as Sega Rally's (A1 = 0x2800 +
  slot x 0x10 at every key-on routine, owner byte +5): Game::scsp_voices.
  Effects alone are silent on the music-only intro (9-17 s) and 13 dB below
  the music in a fight (-35.1 against -22.5 dBFS). Loudness: RMS -22.8
  dBFS in a fight, peak -4.7: output_gain 1.5 (+3.5 dB). Default mix 100/100.
- Controls: rt::ControlScheme (Game::controls), Fighting for vf2. The
  frontends keep rt::Inputs: left/right as the wheel at its ends, up/down as
  the accelerator/brake fully pressed, Punch/Kick/Guard as view buttons 1-3;
  vf2.cpp's port() turns them into MAME's IN1 (player 2 released). Vita:
  vita::Controls::set_fighting (the steering and shift bindings and the left
  stick as directions; Pad::ly added), Controls page lists only the used
  actions with their fighting names; tests/test_vita_controls.cpp covers it.
  The desktop app has no fighting layout yet (its driving keys still reach
  the same Inputs: arrows, pedals and view keys).

Regression after the recompiler, lockstep and game.h changes: daytona93
race_basic and srallyc srallyc_race, 3,000 frames, dumps and WAV identical
(13/13 each); ctest 19/19.

Next: the Vita VPK on hardware (speed with the exact code; --fast-inaccuracy
not checked on vf2, games.py vita_fast empty); a desktop fighting layout;
two players if wanted (IN2).

## Per-game default settings; the coin at start: the backup RAM, now opt-in (2026-10-15)

`rt::Game::defaults` (FrontendDefaults: music_volume, effects_volume in %):
the Vita's VitaSettings take their defaults from it (RESET DEFAULTS too).
Daytona 100/20 (the old global default), Sega Rally 100/85. A vita.cfg saved
before keeps its own values until RESET DEFAULTS.

Reported on the Vita: a game sometimes (Sega Rally almost always) starts a
race by itself at launch; a coin inserted survives Reset and quitting.
Cause, reproduced on the PC with m2run --save-nvram/--nvram: both games keep
the coins and credits paid for but unused in their battery-backed RAM, as a
cabinet does through a power cut, and the Vita saved backup_ram.bin at every
pause and quit and loaded it at start. Once the unused coins make a credit,
the game starts a race on its own about 2 s later, without Start (Daytona: 3
coins at frames 1200-1289, Circuit Select by 1400; the same as MAME's, the
parity runs show it). Not an uninitialized array: the inputs, the I/O board
and the Vita's runtime were checked and start released.
Decision (the user's): the Vita's new option SAVE RECORDS (BACKUP RAM), off by
default: backup_ram.bin neither loaded nor saved, each start is a cabinet
switched on afresh; ioboard_eeprom.bin (test mode settings) always kept. On:
records and bookkeeping kept, and the unused coins too (as on a cabinet).
Wrong turn, written and removed the same day: clearing the unused coins in
the loaded image (rt::clear_pending_credits, m2run --clear-credits); it
worked, the user preferred the option. What it had found, for later:
- daytona93: a block at 0x100 and its copy at 0x180, CRC16 (XMODEM: poly
  0x1021, init 0, MSB first) of +6..+0x7f stored little-endian at +4; +0x28
  the unused coins (full credits included), +0x10/+0x18 coin totals.
  Revision A not checked.
- srallyc: a block at 0 and its copy at 0xc68, CRC16 (poly 0x1021, its own
  initial value: XMODEM of +2..+0x147 XOR 0x7de8 in every save) at +0,
  little-endian; +0x20 coins toward the next credit, +0x22 whole credits,
  +0x04 credits bought, +0x0c/+0x3c coin totals, +0x34 play statistics.
- The CRC is linear, so a byte change only needs the checksum XOR the CRC
  (init 0) of the change, from the byte to the CRC's end; tested: the game
  kept its totals (the block accepted) and started in attract at CREDIT 0.

## Sega Rally sound: as loud as Daytona, music and effects volumes (2026-10-15)

Reported on the Vita: Sega Rally (SCSP board) far quieter than Daytona, and
the Music/Effects options without effect on it.

Loudness, m2run WAVs, 6,000 frames, after the first 10 s: srallyc RMS -38.5
dBFS race / -47.3 attract, peak -20.9; daytona93 -14.4 / -21.8, peak -0.9.
Not an emulation fault: our SCSP is MAME's and the game's own levels (MVOL,
TL) are low; the cabinet's amplifier makes it up. So a per-game frontend
gain, `Game::output_gain` (default 1; srallyc 10, +20 dB: its race peaks at
Daytona's, no clipping), applied by the Vita (vita::Audio::gain, reference
audio) and the desktop app (stream gain); m2run's WAV and the parity checks
stay at the board's output.

Music/effects: the Model 1 board splits them through its driver's voice
pools (SoundBoard::voice_channel); the SCSP board did nothing. Sega Rally's
driver keeps one 16-byte record per SCSP slot at 0x2800 in the 68000's RAM:
the key-on routines (0x600ec6, 0x601046, 0x601092, 0x6010de: ori.b #$18 to
the slot's register) take the slot's register offset from its first word
(A1 = 0x2800 + slot x 0x10); its byte 5 is the owner, 0 for the music's
sequence (2,886 of 2,887 attract key-ons, the other at the coin's effect),
1-15 for effects (22 % of race key-ons). Found by logging the 68000's PC/A1
and that record at every key-on (attract vs race), after RAM-snapshot
statistics alone had been inconclusive (the driver reuses records, so the
slot number is not rewritten at most key-ons). Described as data,
`Game::scsp_voices` = {0x2800, 0x10, 5} (src/games/srallyc.cpp);
ScspBoard::scsp_slot_effect reads it at each Scsp::StartSlot; Scsp scales a
slot's sample (direct and DSP send) by its owner's volume, and leaves the
samples untouched while both are 1 (MAME's output). Check (m2run --volumes,
new): attract music alone -47.3 dBFS = all, effects alone -74.8; race music
-44.5, effects -39.8, together -38.5. Parity with both at 1: daytona93 and
srallyc 3,000-frame race dumps + WAV identical; ctest 19/19. Not done: the
DSP's own output (EFREG, reverb) is not split, it follows the slots' sends.
Not yet heard on the console.

## Vita Options page: only the options that can change something (2026-10-15)

platform/vita/main_gpu.cpp: the Options rows are an enum (`Option`), and
`option_available(option, settings)` decides from the build and the game's
record, never a set name: Aspect/HUD/Stretch only with the GXM renderer
(kWidescreen; vitaGL draws 4:3 only), Audio engine only with
Game::native_sound, Music/Effects when the board can split them
(rt::has_music_effects) or the native engine is selected (corrected the same
day: the Model 1 board's reference audio splits them too), Draw distance only with the new
`Game::draw_distance` (src/runtime/game.h, default false; true on both
Daytona sets, whose hook_draw_list serves it), Link play only with
Game::link and its 7 detail rows only once it is on, ROM only when the
package holds the companion's executable (as start_game looks for it). The
Graphics API, Fullscreen and Binds rows are gone (fixed or informative;
the footer shows the chords). Input handling goes by the listed row's
Option; after a change that lists or unlists rows the cursor stays on the
option changed. vitaGL lists 14 rows for Daytona '93 (was 31; +2 with the
native engine selected, +1 in a two-ROM package), 12 for Sega Rally.
Built: daytona and segarally --gpu-gl --fast-inaccuracy VPKs, GXM
--compile-check, desktop builds and ctest 19/19 (game.h shared). Not yet
run on the console.

## Builds per game: build_<game>/, --game, <game>.vpk (2026-10-15)

Everything is built by game now, each in its own folder (scripts/games.py
paths: game_dir, recomp_dir, rom_cache, gen_root, pc_dir, vita_dir, vpk_name):

  build_<game>/recomp/<set>/rom_cache/  m2import's images
  build_<game>/recomp/<set>/gen/        the recompiled code (CMake M2_GEN_ROOT;
                                        inside: <set>/, <set>_tgp/, <set>_snd/ as before)
  build_<game>/pc/<set>/                the desktop build (tools, m2run, the game)
  build_<game>/vita/, vita_diag/        <game>.vpk, <game>_diag.vpk

games.py: `GAMES` is now per game (daytona: daytona93, daytona; segarally:
srallyc), with title, sets (main first), vita_title_id, vita_name; the per-set
table is `SETS` (gains "game", loses title/vita_*). `resolve(game, set)`: the
set alone gives its game, the game alone its main set, a set of another game
is an error. recompile.py, build_vita.py, seed_scan.py take `--game` (default
daytona) and `--set`; `--build-dir` is gone from recompile.py/seed_scan.py and
`--host-build-dir` from build_vita.py (build_vita.py keeps `--build-dir` for
side-by-side variants). recompile.py `--recomp-only` stops after the
recompilation. setup.py builds the tools and tests in build_daytona/pc/daytona93
(CI's ctest dir changed with it), then every set with a ROM into its game's
folder. Vita CMake: DAYTONA_VITA_VPK (the package's file name) and
DAYTONA_VITA_LIVEAREA_DIR; the icon/LiveArea images are per game in
platform/vita/logo_livearea/<game>/ (committed empty, .gitkeep; images
ignored). m2_check.sh and the three sound test scripts take their default
paths from the same layout; the sound scripts find the generated objects with
find (outside the build tree, CMake names them after their path).

Checked: recompile.py --game segarally and recompile.py (daytona) from
scratch; m2run race_basic / srallyc_race 3,000 frames, dumps every 250 + WAV,
identical to the references (5d4b1b641acf6f5b, 4b73fca0c3d6b13e); ctest 19/19
in both pc/ builds; Python tests (test_build_vita 27, test_setup, make_input,
setup_vitagl) OK; build_vita.py --gpu-gl --fast-inaccuracy for both games:
build_daytona/vita/daytona.vpk (DAYT00093), build_segarally/vita/segarally.vpk
(SRAL00094), not yet run on the console. Old build/, build-*/ folders deleted locally.

The desktop executable is named after the game (CMake `M2_GAME`, passed by
recompile.py and setup.py; OUTPUT_NAME of the `daytona` target):
build_daytona/pc/<set>/daytona, build_segarally/pc/srallyc/segarally.

Dreamcast and mobile follow the layout too (asked for, though the rules
leave them alone otherwise): build_dreamcast.py reads the daytona set's
recompilation from build_daytona/recomp/daytona/ and writes to
build_daytona/dreamcast*, its --host-build-dir is gone; its tools CMake,
Makefile comment, pc_profile.py and flycast_shot.ps1 defaults likewise. The
iOS script and the Android gradle read build_daytona/recomp/daytona93/gen (iOS
output build_daytona/ios). Not built or run here (Dreamcast and mobile are not
tested in this fork): only the Python parses and --help works.

## The TGP's FIFO inlined too; the inlined bus now opt-in (2026-10-15)

Verdict first: the inlined bus (RAM, ROM, and now the TGP FIFO) gives no
measurable gain on the Vita without profilers, so it is OFF by default
everywhere: `build_vita.py --inline-bus` (was `--no-inline-bus`), Vita CMake
`DAYTONA_VITA_INLINE_BUS` OFF, desktop `M2_INLINE_BUS` OFF as before
(`recompile.py --inline-bus`). The code stays, results identical, for
comparison.

What changed: stage 2 of `gen::BoardMem` (src/runtime/gen_support.h,
m2recomp --board code only) starts with the TGP: aligned dword accesses to
the output FIFO (0x00884000 read), the input FIFO and function port
(0x00880000-0x00887fff write), the FIFO status (0x00980004 read) and buffer
RAM (0x00900000-0x0097ffff, read after tgp.sync(), write all lanes) call
`rt::TgpBoard` directly, as `M2Board::dev_read`/`dev_write` do; one range
test (0x00880000-0x00980fff) after stage 1 and before the data ROM.
`flags()` answers BURST for buffer RAM and 0 for the ports without the page
table (ldq from the FIFO pops 4 times, as before). TgpBoard's fifo_w,
fifo_r, fifo_out_empty and function_port_w are inline in m2_tgp_board.h
(every build); the program upload (`upload_w`) and the TGP run on an empty
output FIFO (`refill`) stay out of line. Byte and word TGP accesses,
coproctl, geo ports: still the bus.

PC parity (Release, 3,000 frames, dumps every 250 + WAV, against the
binaries of 0d290cc): daytona93 race_basic and srallyc srallyc_race inline
builds identical (5d4b1b641acf6f5b, 4b73fca0c3d6b13e, same i960/TGP/
interrupt counts); default daytona93 build identical too. ctest 19/19.

PC i960 profile builds (3 runs each): TGP bus calls a frame daytona93
682 r + 2,057 w -> 0 + 2, srallyc 187 + 899 -> 0; i960 alone daytona93
0.148-0.160 -> 0.149-0.156 ms, srallyc 0.158-0.179 -> 0.139-0.148 ms
(noise level).

Vita, the measurement that counts (extern/result_perf_vita, not committed):
`--gpu-gl --fast-inaccuracy --diagnostics`, no profilers, the same commit
with and without the inlined bus (RAM+ROM+FIFO; eboot hashes checked
against the installed DAYT90093/DAYT90094), attract left running from boot,
so every 5 s window does the same work in both (i960/TGP instructions and
polygons within 0.1 %): perf_base.log against perf_all_modif.log, 10
windows, i960+TGP (game logic + vblank handler) 3.90 -> 3.90 ms a frame on
average, every window within +-1 % except window 1 (boot, 2.98 -> 2.80);
fps the same (57.3-57.6), core 0 9.43 -> 9.52 ms a frame. A release build
(LTO) with the inlined bus was handed over for a check with the Vita's own
profiler (DAYT90095); nothing significant found there either.

Why not: on the Vita M2_FAST_BUS already short-cuts the TGP FIFO and the
last RAM page in read_dword/write_dword, and --fast-inaccuracy already
makes fixed work-RAM accesses direct; what was left was the virtual call,
~7,000 a frame, tens of microseconds. The time follows the TGP's
instruction count and the i960's cost per instruction, not memory access.

How the earlier "gains" were wrong (kept here so they are not trusted
again):
- the --i960-profile builds count every bus call in prof_bus() (a page
  lookup and a counter): removing bus calls removed probe cost, which
  showed as -0.8 to -1 ms in the race;
- without profilers, the race windows of two separate sessions differed by
  ~10 % for reasons unrelated to the code (perf_modif_ram_rom.log was slow
  throughout: slower than the build without any of it), which read as
  "-0.73 ms" for the FIFO;
- the race is not reproducible window by window; the attract from boot is.

Unrelated, found on the way: build-srallyc-rel's generated code predates
0d290cc (no gen::inline_board), so its m2native no longer links; rerun
recompile.py there.

## The bus inlined in the i960 code, per board (2026-10-14)

`m2recomp --board model2o|model2a` (scripts/games.py "board"): loads and
stores are `Mem::ld32(B, a)`... with `using Mem = gen::BoardMem<board>`
(src/runtime/gen_support.h) and `B` the `rt::M2Board` behind `c.bus`
(`final` under M2_INLINE_BUS: direct calls, not virtual). Three stages:
1. inline: work RAM 0x500000-0x5fffff, program ROM 0-0x1fffff, main RAM
   0x200000 (2: 128 KB, then the ROM mirror at 0x220000; 2A: 256 KB),
   aligned or not as long as the access stays inside one 128 KB block (every
   range boundary is one); writes skip ram_written (nothing there); flags()
   BURST below 0x240000 and in work RAM; the frame-wait byte (ldob
   0x500000) still goes through read_byte;
2. `M2Board::read_dword_mid` / `read_word_mid` / `read_byte_mid`: the data
   ROM 0x02000000-0x03ffffff (the TGP FIFO can go here next);
3. the bus as before (read_dword etc., devices, video RAM, TGP).
GameLoop throws at start if the code's board (`gen::inline_board`, written
by m2recomp in gen_table.cpp, -1 without --board) is not the game's, or the
program image is not 2 MB / the data ROM 32 MB. m2native refuses the code
(replay bus). Without --board the chunks are byte-identical to before
(gen_table.cpp gains the `inline_board = -1` line).

Switches: CMake `M2_INLINE_BUS` (desktop OFF; `recompile.py --inline-bus`
sets it and passes --board); Vita `DAYTONA_VITA_INLINE_BUS` ON, build_vita.py
passes --board in --fast-inaccuracy and --fast-gen, `--no-inline-bus` to
compare. (15 Oct: OFF by default on the Vita too, `--inline-bus`; the
measurements below were mostly probe cost, see the entry above.) Not on the Dreamcast (M2_DC_MEMORY: #error). fast_gen.py (shared
with the Vita) now turns `Mem::` fixed work-RAM accesses into gen::wram_*
too, and treats any other `Mem::` access as not register-only.

PC parity (Release, 3,000 frames, every 250 dumped + WAV): daytona93
race_basic and srallyc srallyc_race identical with and without, same i960/
TGP/interrupt counts (5d4b1b641acf6f5b, 4b73fca0c3d6b13e); default build
regenerated: identical too. ctest 19/19 both builds, Python tests OK.

PC i960 alone (M2_I960_PROFILE builds, ms/frame; PC noise ~10 %):

| | before | after |
|---|---|---|
| daytona93 | 0.227 | 0.147-0.165 |
| srallyc | 0.192 | 0.167-0.186 |
| daytona93 bus calls/frame | 12,623 | 7,060 (4,197 direct) |
| srallyc bus calls/frame | 15,506 | 10,137 (1,448 direct) |

Sega Rally gains less: it uses its work RAM little (1,392 accesses a frame
against Daytona's 6,339); what is left on its bus is video RAM copies (a
ldl/stl loop at 0x5fcb1c, ~3,000 reads + 2,800 writes a frame: they need
ram_written) and the 315-5649 I/O chip (2,039 reads). Daytona's remaining
work RAM reads are the frame-wait byte (1,199 loop runs a frame) and
do_ret/do_call's frame spills (the runtime's, not the generated code; on
the Vita M2_FAST_CPU already copies them directly). Wrong turn on the way:
the srallyc main-RAM calls left after the first version were not unaligned
accesses (making those direct changed nothing); the data ROM's 16-bit reads
were (ldos/ldis, now stage 2).

Vita hardware, Daytona, `--gpu-gl --fast-inaccuracy --diagnostics
--i960-profile --tgp-profile` (extern/result_perf_vita/
perf_modif_ram_rom_i930_TGP.log against perf_base_i930_TGP.log, same flags
without the inline bus; not committed):
- bus calls in the race ~19,300 -> ~9,800 a frame (~11,000 direct);
- light windows (little TGP work, comparable): window 1 (~56,500 i960
  instructions both) i960 alone 3.97 -> 3.45 ms, 69.8 -> 61.1 ns/instr;
  window 4 70.0 -> 63.7 ns/instr (-9 to -12 %);
- a fit over every window (alone = a x i960 instr + b x TGP instr, rms
  0.15 ms): a 60.0 -> 48.8 ns, about -0.45 ms a frame at the race's ~41,000
  instructions: a third of the plan's 1-1.5 ms;
- race windows: i960 alone 7.4-7.8 ms both, fps the same (49-57), core 0
  93-100 %. Not a measure of the change: "i960 alone" there holds ~5 ms that
  follow the TGP instruction count (b = 82-91 ns per TGP instruction, on top
  of TGP time's 47 ns), most likely the profilers' own probes (a clock read
  is 0.69 us on the Vita) saturating core 0. Race timings need builds
  without the profilers.
A/B without the profilers: `build_vita.py --gpu-gl --fast-inaccuracy
--diagnostics` (build/vita-diag, DAYT90093) and the same with
`--no-inline-bus --build-dir build/vita-diag-noinline --title-id DAYT90094`
(installs next to it). Next: run both in the race, compare "i960+TGP game
logic" and "vblank handler" in perf.log.

## i960 and TGP profiling for the Vita's optimisation choices (2026-10-13)

Measurement only: what the i960 and the TGP cost on the Vita, to choose
what to optimise. With both flags off the runtime and the generated code
are unchanged.

`--tgp-profile` / `--i960-profile` on recompile.py (CMake M2_TGP_PROFILE /
M2_I960_PROFILE, m2run prints the reports) and on build_vita.py
(DAYTONA_VITA_TGP_PROFILE / DAYTONA_VITA_I960_PROFILE, sections in each
perf.log report; imply --diagnostics, need --fast-inaccuracy, the TGP one
also takes --fast-gen, and a GPU build).
- TGP (src/runtime/tgp_profile.h): m2tgprecomp --profile counts each basic
  block at its end and writes its contents (FIFO, I/O, data RAM, FP by kind,
  branches); the runtime times run_tgp, counts table ports and banks, and
  micro-benchmarks unit costs at game start.
- i960 (src/runtime/i960_profile.h): m2recomp --profile, the same per block
  (bus loads/stores in words, ALU, mul, div, compares, branches, calls/rets,
  indirect, FP, libm, sys), dispatches, chunk entries, lockstep slow paths.
  The runtime times gen::run (GameLoop::run_generated) and, inside it, the
  Lockstep callbacks (board events: vblank, rendering in m2run, timers),
  and counts M2Board bus calls by region (work RAM, other RAM, ROM,
  texture, TGP, devices). "i960 alone" = gen::run - callbacks - TGP.

Diagnostics Vita builds (build_vita.py --diagnostics, or a profile flag) are
their own app: title ID with a 9 as first digit (DAYT90093, SRAL90094), name
"... (diagnostics)", build dir build/vita-diag (or vita-SET-diag); the
release install and build are left alone. Same ux0:data/SET folder.

PC (this machine, Release, race 3,000 frames, both profilers): counts, last
screen hash, the 12 frame dumps and the WAV identical to the build without
(daytona93 race_basic: 79,926,273 i960, 61,962,319 TGP, 5d4b1b641acf6f5b;
srallyc race, TGP only: 4b73fca0c3d6b13e). ctest 19/19, Python 37/37.
Daytona: i960 alone 0.163 ms a frame, 26,642 instructions, 6.1 ns each;
board callbacks inside gen::run 3.53 ms; TGP 0.036 ms, 2 ns per instruction.
i960 per frame: 5,346 blocks of 5.0 instructions, 1,020 dispatches, 298
calls, 300 rets, 55 divides; 12,623 bus calls (work RAM 6,339, TGP 2,739,
ROM 1,783). Hottest i960 block 0x12b0 (2 instructions, 1,199 runs a frame,
9 %: a wait loop).

Vita hardware, Daytona, TGP profile only (extern/result_perf_vita, not
committed): full speed (57.3-57.6 fps), core 0 busy 50-88 % in the race.
TGP 0.9-3.0 ms a frame = 20-36 % of the i960+TGP lines; ~48,000 TGP
instructions a frame at 46-54 ns = 20-24 cycles each (PC: 1.8-2 ns).
Known costs (FP, tables, banks, FIFO) explain 6 %: the rest is the code
around each instruction. Likely (from the generated code, not yet checked in
ARM assembly): the TGP's registers are struct fields that alias data[]
(uint32_t both), so every data RAM store forces reloads; the ALU flags
(stset_fp, st) are computed for every op though rarely read; alu_r1/r2 go
through memory; one-instruction rep loops (blocks 262, 269: 1,872 runs a
frame) pay a whole block end each turn. Ideas for m2tgprecomp (bit-exact):
block-local copies of a,b,d,p,st,x0/x1..., dead flag elimination, rep as a
C loop. Expected 1-2 ms a frame in the race, to be measured.
The Vita micro-benchmark is not usable: fp add+mul and fifo read 0.0 ns
(the loop is optimised away or below the 14 ns loop cost); the clock read
costs 0.69 us there (~0.15-0.2 ms a frame around the TGP's runs).
fpscr on the Vita is 0x83000010 (bit 24, flush to zero, looks set): not
checked against MAME's denormals.

Next: run the Daytona probe VPK (both profiles) on the Vita, read the i960
section: cycles per i960 instruction, the share of bus calls, the hottest
blocks; then pick the first optimisation (TGP block-local registers or the
i960's).

`scripts/build_jobs.py`: recompile.py and setup.py passed a bare
`--parallel` (make -j unbounded): a fresh srallyc build started 433 compilers
at once (~26 GB) and the OOM killer took the desktop down. Now min(CPUs,
available MB / 600), M2_BUILD_JOBS overrides. recompile.py now gives a new
build directory its build type (--config, Release), as setup.py does: with
none (-O0) test_fp and fp_vs_mame do not link (SoftFloat's INLINE_LEVEL=5
helpers are not emitted), which is how build-srallyc was broken.

## README for the fork (2026-10-13)

README.md now introduces this project: a Model 2 static recompiler for the
PS Vita, based on daytona-arcade-recomp, with Daytona USA and Sega Rally.
The previous README is kept unchanged as README_fork.md.

## Sega Rally on the Vita with --fast-inaccuracy (2026-10-12)

`scripts/games.py` `vita_fast` is now the list of build_vita.py code modes
checked on a set (was a bool): daytona93/daytona both, srallyc
`("fast_inaccuracy",)`. build_vita.py refuses the others by name. The
direct work-RAM pass applies as is: srallyc's work RAM is the same
0x00500000-0x005fffff (only 108 fixed accesses become direct: its code,
copied to 0x5a0000, mostly addresses through registers).

Checked on the PC: build-srallyc-fast (recompile.py --fast_inaccuracy, then
build_vita.direct_ram on gen/srallyc, CMAKE_CXX_FLAGS=-DM2_FAST_GEN=1 like
the Vita runtime): srallyc race 6,000 frames, the 24 frame dumps (every
250) and the WAV identical to the exact build, 10,996 sound bytes both. (That
build had no CMAKE_BUILD_TYPE, so its 127 s is unoptimised: not a timing.)
Python test_build_vita 19/19. Vita: `build_vita.py --set srallyc --gpu-gl
--fast-inaccuracy` built; not run on hardware. --fast-gen on srallyc: not
tried (fast_gen.py fails on what it does not recognise).

## Board timers: stale expiries piled up in the Lockstep log (2026-10-12)

Review of the uncommitted work (same day): kept as general Model 2 work the
2A-CRX map and 315-5649, the four timers, UART receive, the i960 FP ops /
flushreg / addc / --alias, m2sndrecomp --scsp, the SCSP board, the games
split, colour_generation (moves in 5 of 3,000 daytona93 race frames, 85 of
6,000 srallyc: the Vita's palette rebuilds are rare). Timer `gen` removed:
with cancel_callback the scheduled expiry is always current, and [this, n]
fits std::function's inline storage on ARM32 (no allocation each frame).
Seen, not changed (in HEAD): dev_read calls getenv("M2RUN_VERBOSE") on every
TGP FIFO status read outside M2_DC_SPEED, the Vita included.

Hardware perf.log (srallyc, VITAGL, before this fix): core 0 saturated by
"i960+TGP game logic", not by sound (SoundSync 0.07 ms; the sound board 3.5
ms on core 2). In the attract mode, before any polygon, the i960 cost grew
by ~2.3 ms every 2 s at a constant 18.4 k instructions a frame
(core_ms 2.7 -> 16.4 ms in 14 s); the race ran at 20 fps. A cost that grows
with time, not with work: a growing structure.

Cause: `M2Board::timer_schedule` (added with the timers for the SCSP sound
commands) added a Lockstep callback on every reload and for every running
timer at every `idle_until_vblank`, and left the previous one to lapse
(gen check). Most of a frame's board time is skipped, not run, so a timer's
count can be far ahead: at count 103 M, 34 k events in the log, 30 k of them
more than 10 M instructions ahead (last at 1.09 G). Each add_callback scans
and shifts past them (memmove 29 % of an m2run profile); calls_ grew without
bound outside the fast builds (srallyc's Vita build is not fast).

Fix: `Lockstep::add_callback` returns a handle; `cancel_callback` takes the
event out and frees its slot; `Timer::expiry` holds the handle, cleared when
the callback runs. Slot reuse (`free_calls_`) is now in every build, not
only M2_DC_MEMORY/M2_FAST_GEN. The log no longer reaches 4,096 events in a
12,000-frame srallyc race.

Measured (m2run, this machine): srallyc race 12,000 frames 93.6 -> 57.6 s
(frames 6,000-12,000: 9.7 -> 5.1 ms a frame), 309 -> 280 MB; 6,000 frames
44.7 -> 29.6 s, frame dumps every 250 frames and WAV identical; daytona93
race_basic 3,000 frames 16.0 -> 15.7 s, dumps and WAV identical. ctest 19/19
both builds. Vita: built (`build_vita.py --set srallyc --gpu-gl`), not run
on hardware yet; expected: Logic flat over time in perf.log.

Still open: m2run's RSS still grows (195 MB at 1,500 frames, 280 MB at
12,000) with the log bounded; not looked at.

## Wrong turn, reverted: the Vita board frame not waiting on sound (2026-10-12)

An earlier change made main_gpu.cpp dispatch a sound packet only when the
worker was idle and merge later frames into a held one
(`SoundPacket::absorb`, `kMaxCatchUp` = 4 frames, `SoundWorker::ready()`),
on the guess that srallyc's slowdown was core 0 joining a slow sound frame.
The hardware perf.log disproved it: SoundSync 0.07 ms a frame, the sound
board 3.5 ms on core 2; the cost was the timers' stale Lockstep callbacks
(section above). Reverted: it changed Daytona's audio too (gaps instead of
waits, UART bytes of merged frames at one time) for no measured gain.
Do not re-propose without a perf.log showing SoundSync on core 0.

## Sega Rally sound: the SCSP board, and the board timers (2026-10-12)

Branch vita-model2-recomp. Design doc, Audio ("SCSP sound board") and the
Model 2 board timers paragraph next to it.

Why there was no game sound traffic: srallyc's i960 sends its sound
commands from a routine (0x5c4c60: TxRDY polled on the UART status,
0x1c80002 bit 0, one byte per call, 50,000-tick reload) run by board timer
interrupts (intena 0x215: timers 0 and 2). The runtime had no timers
(Daytona never enables them), so 1 interrupt a frame instead of MAME's
10.5, one byte sent in 3,000 frames, and "SOUND BUFFER FULL !!". The
earlier lockstep matched MAME only because m2native replays MAME's
interrupts.

What changed:
- M2Board: the four timers (MAME timers_r/w, model2_timer_cb) on board
  time in 25 MHz ticks = i960 count x 434,783 / 110,000 + the vblank wait
  skipped; GameLoop asks `idle_until_vblank()` before starting vblank, and
  a timer interrupt due before vblank runs first. i8251 reads: status (DSR,
  TxEMPTY, RxRDY, TxRDY = data buffer empty) and data; `receive_sound_bytes`
  for the SCSP's MIDI output (request bit 10 when enabled).
- `src/runtime/scsp.{h,cpp}`: MAME's SCSP + DSP transplanted (BSD-3,
  THIRD_PARTY.md); `ScspHost` for IRQ lines, timers, MIDI out.
  `src/runtime/scsp_board.{h,cpp}`: 68000 + SCSP, kIps 825,876 (MAME:
  25,842,220 instructions in srallyc's 1,800-frame attract), sample ROMs at
  0x800000 (8 MiB set: no banking), MIDI in at the line rate.
- `snd::Board` (snd_board.h): the interface GameLoop, the app, m2run and the
  Vita use; `snd::SoundBoard` (Model 1) implements it unchanged.
  `GameLoop::SoundReplies`: the MIDI bytes back to the i960 (locked: the
  Vita overlaps sound and board frames). `snd::board_fm_rate/pcm_rate`.
- `Cpu68k` map is a `SndMap` (kModel1Map = the old constants, kScspMap);
  Devices gains read16/write16 for the SCSP board. m2sndrecomp `--scsp`
  (scripts/games.py `sndrecomp`); `seeds/<set>_snd.txt` passed by
  recompile.py; m2sndrecomp emits the 68000's read before `clr` to memory
  (MAME's core does it; the SCSP log showed it). Game record:
  `Game::sound` (SoundBoardType None/Model1/Scsp) replaces `model1_sound`;
  srallyc loads epr-17890a.30 and mpr-17756/17757/17886/17887 (`Samples`).
  m2import writes samples.bin.
- seeds/srallyc_snd.txt: four `jmp (d16,pc)` tables (16, 128, 16, 16
  entries) from the 2,207 addresses MAME's 68000 executed in srallyc_race.
- MAME patch 0004 (log for `:audiocpu`, SCSP/bank accesses with mask,
  `M2TRACE_SNDPCS`). m2sndcheck `--scsp SAMPLES.bin`; the interrupt level
  is taken from the handler's snapshot SR (the log's m_int_level was 2 for
  a level-1 entry at race 54,773,855), except when two interrupts nest at
  one count.
- App: audio pacing on the PCM stream (the SCSP board's FM stream is empty).

Measured:
- m2sndcheck --scsp MATCH: attract 25,841,665 instructions, 28,903
  interrupts, 2,229,366 device accesses; race 69,408,422 instructions,
  110,711 interrupts, 5,209,129 accesses (traces/scsp_*.snd, git-ignored).
- Free run: attract 1,800 frames, 25,842,651 68000 instructions (MAME
  25,842,220); WAV RMS per second matches MAME's -wavwrite of the same
  attract (silent to 16 s, then 254 237 212 227 282 255 225 0 137 ... vs
  239 209 232 232 268 277 241 0 138 ...; peak 1,306 vs 1,302).
- srallyc race 3,000 frames: 30,469 interrupts (was 2,918), 2,335 bytes to
  the sound board (was 1), no "SOUND BUFFER FULL" at frame 2700; 10.3-10.8 s
  on this machine with the sound board (10.3 s before, without).
- Daytona: daytona93 race_basic 3,000 frames (and +2 draw distance) frame
  dumps and WAV identical, after regenerating its sound code; ctest 19/19
  both builds; Python tests 35/35; the PC app runs srallyc with reference
  audio (PipeWire), 1,500 frames. Vita, no --release: `build_vita.py --set srallyc --gpu-gl`
  (SRAL00094) and `--gpu-gl --fast-inaccuracy` (DAYT00093) built, no
  warnings in the touched files; not run on hardware.

Findings:
- The SCSP 68000's driver writes the slot monitor (MSLC, 0x100408) 2 M
  times in 1,800 frames: each costs a render up to the current count.
- MAME's i960 sends 39 MIDI bytes in that attract, ours 31 (our board time
  model is not MAME's; not a lockstep path).

Next:
1. Vita: run the srallyc VPK on hardware (speed with the sound worker).
2. Hand brake in the app's controls; drive board commands for srallyc.
3. Music/effects volumes for the SCSP board (set_volumes is a no-op).

## Games apart from the board (2026-10-12)

Branch vita-model2-recomp (renamed from sega_rally_convert). Goal: a
generic Model 2 recompiler for the Vita, where adding a game touches only
its own files. Design doc, Architecture, "Games"; checklist in
docs/adding-a-game.md.

What changed:
- `src/runtime/machine.h` became `src/runtime/game.h`: `rt::Game` per MAME
  set (board, ROM table, main_data mirror, TGP program location, idle loops,
  Model 1 sound and the native engine's `SoundLayout`, 2A-CRX input wiring
  `CrxInputs`, link play, Vita companion set), `rt::game()`,
  `rt::find_game()`, `rt::games()`. `rt::Inputs` moved there from m2_board.h.
- `src/games/`: `daytona.cpp` (daytona93, daytona, their ROM tables, sound
  layouts and `hook_draw_list`, moved from enhance.cpp), `srallyc.cpp` (ROM
  table, the 315-5649 wiring moved from IoBoard::port_in/crx_read),
  `games.cpp` (the list), `hooks.h` (every game's hooks; gen_support.h
  includes it). The records are `extern const` and constant-initialized, so
  they are safe to use from static initializers. enhance.cpp is gone;
  enhance.h only holds the options.
- rom_import.cpp is generic over `game().roms`. The native sound sequencer
  takes the `SoundLayout` (it no longer detects the layout among Daytona's
  two); `NativeSoundEngine` passes it; app, Vita and m2run gate native audio
  on `game().native_sound`.
- `scripts/games.py`: the scripts' half (archives, m2recomp options, Vita
  title ID/name, `vita_fast`, companion, make_input ports, analog range).
  recompile.py (`SETS = GAMES`), seed_scan.py, setup.py, make_input.py,
  m2_check.sh and build_vita.py read it. build_vita.py always passes the
  title ID and package name (`DAYTONA_VITA_GAME_NAME`, new) and the
  companion; vita_code passes the set's m2recomp options; the hook check
  takes every name in seeds/<set>_hooks.txt.
- CMake (desktop and Vita): a set is accepted when seeds/<set>.txt exists,
  not by a hard-coded list. The Vita CMake no longer maps set to title ID.
- Vita launcher: the ROM option switches to `Game::companion` (was
  "daytona" hard-coded); the target executable is app0:/<companion>.self when
  present, else app0:/eboot.bin when we are the .self; link play is refused
  when `!game().link` (daytona93: false, as before).

Rebased on push_DC_opt_on_Vita (2026-10-13): the Vita home-screen name is
the set's `vita_name` (scripts/games.py: "Daytona USA", "Sega Rally
Championship"), the same for every renderer, as that branch made it for
Daytona; the ROM option is index 11 after its music and effects options; the
LiveArea images (platform/vita/logo_livearea) are shared by every game.

Measured, unchanged: m2run frame dumps every 250 frames and WAV hashes
identical before and after (daytona93 race_basic 3,000 frames, the same at
draw distance +2 for 2,000 frames, srallyc srallyc_race 3,000 frames, also
after regenerating srallyc's code through the new recompile.py); same
instruction, TGP, interrupt counts; daytona93 3,000 frames in 11.40 s
(11.37 s before; a run at 18.8 s was host load from other compiles).
--native-audio-check on daytona93 3,000 frames with no fault; on srallyc
it stops with "no native audio". ctest 19/19 in build/ and build-srallyc/,
Python tests 35/35. Vita, no --release:
`build_vita.py --set srallyc --gpu-gl` (SRAL00094, build/vita-srallyc) and
`build_vita.py --gpu-gl --fast-inaccuracy` (DAYT00093, build/vita) built
their VPKs, with no warnings in the touched files. Not run on hardware: the
ROM switch to the companion set and the link refusal are untested on the
console.

Not done, still game-specific: the drive board's command model
(drive_board.h, Daytona's), the app's log prefixes and pref folder name
("daytona"), the legacy CPU Vita frontend (main.cpp, daytona93 only),
m2_check.sh's daytona93 file names (traces/hdr.m2in, check_NAME).

## Sega Rally Championship (srallyc, Model 2A-CRX) bring-up (2026-10-11)

Branch sega_rally_convert. Goal: one runtime for Daytona and Sega Rally
Championship Revision C (MAME `srallyc`). Sound (SCSP) deferred: no sound
board for srallyc yet. State: `recompile.py --set srallyc --build-dir
build-srallyc` builds the game; m2run boots it, passes the EEPROM setup and
shows the title screen (frame 1200) and the attract race (frame 1500), the
same scene as MAME 0.264 at frame 1500 (checked by eye on snapshots, not by
trace). 1,800 frames: 27.1 M i960 and 7.4 M TGP instructions, 1,718
interrupts, all native. Daytona unchanged: daytona93 attract_long and
race_basic, 4,000 frames each, give the same instruction, TGP, interrupt
and sound-byte counts and the same screen hashes before and after; ctest
19/19 in build/ and build-srallyc/, Python tests 32/32.

Lockstep against MAME (patched MAME 0.264, `SET=srallyc BUILD=build-srallyc
scripts/m2_check.sh SCENARIO`), all MATCH:
- attract (600 frames, no input): 116.5 M i960 instructions, 5,474
  interrupts, m2native and m2replay; TGP 515 words; screen 596/596 frames
  (no polygons yet in those frames).
- srallyc_attract (1,800 frames): 349.7 M i960, 18,272 interrupts; TGP
  7.47 M instructions, 17,367 output words; 693,255 polygons, 3D layer
  440/440 frames, screen 1,796/1,796. MAME 2.5 min, trace 1.1 GB.
- srallyc_race (6,000 frames, the race script): 1,032.5 M i960, 62,992
  interrupts; TGP 160.2 M instructions, 1,830,962 output words; 4,568,552
  polygons, 3D layer 4,624/4,624, screen 5,996/5,996. 10 min in all, trace
  3.3 GB, m2native 57 s.

What changed:
- `src/runtime/machine.h`: per-set `Machine` (board type, Model 1 sound or
  not, idle loop ranges). `M2Board` picks model2o or Model 2A-CRX by it:
  RAM 0x200000-0x23ffff (no ROM mirror), and the 315-5649 I/O chip at
  0x01c00000 (ports, analog mux, 93C46 EEPROM on port A/B; MAME
  `sega_315_5649_device`, `model2a_state::eeprom_w/in0_r`).
- `rom_import.cpp`: the srallyc files (rev C program, no sound ROMs), and
  its TGP program, which is in the **program ROM** at 0x5ae94, 1,665 words,
  CRC 0x072484B1: equal to MAME's `copro_tgp_program` after boot (dumped
  with a Lua script). The game uploads it from its RAM copy, 0x5f9e94, with
  the count at 0x5fb898 (ROM 0x5c898).
- `m2recomp --no-mirror --alias 5a0000:1000:60000`: the boot (ROM 0x514)
  copies ROM 0x1000-0x60fff to work RAM 0x5a0000 and runs from there.
  `recompile.py` and `seed_scan.py` share a per-set table (`SETS`) for the
  archive names (a merged `srallycb.zip` holds rev C too) and these flags.
- m2recomp templates: `flushreg` (MAME's, `rt::Cpu::flushreg`), every FP
  opcode MAME implements but `movre`, with fp0-fp3, long reals and FP
  literals (MAME's double expressions; design doc, Floating point), and
  `addc` with the silicon's carry (Open decisions).
- `m2sndrecomp` writes a module with no code for an empty sound program.
- seeds/srallyc.txt: 464 addresses from seed_scan (45,307 instructions).

Findings:
- The first EEPROM hang: busy-wait compared the write-cycle end with the
  time of the last port-A *write*; the game polls DO by reading only. Reads
  now pass the current count (`io_now`).
- srallyc's wait for the next frame is 0x5a379c-0x5a37b3 (polls
  videoctl_r's frame bit); without it in `Machine::idle` frames ran to the
  110,000-instruction cap.
- seed_scan takes some TGP program words (0x5ae94-0x5c898) for i960 code
  (an `addc` at 0x5af28 is one): dead native code, harmless.
- 5,683 of the 45,307 instructions are FP (movr/movrl 3,227). No extF80
  model or proven fast path for them: they follow MAME's host double.
- Unverified: whether the game ever rewrites its RAM code (no runtime hash
  check yet; design doc, Code outside ROM).
- Inputs (315-5649 ports, MAME `srallyc` config): IN0 coin/test/service
  plus VR (any of Daytona's VR buttons) and start; IN1 bits 0x70 the
  gearbox, as Daytona's; IN2 the hand brake (`Inputs::handbrake`, script
  name `handbrake`); analog 0-2 wheel, accelerator, brake, rescaled from
  Daytona's 0x20-0xe0 to 0x00-0xff; port E bytes go to the drive board
  queue. `scripts/inputs/srallyc_race.txt` (2 coins, start x5, accelerator
  held) reaches a Championship race: countdown at frame 2100, driving in
  gears 1-4 at up to 207 km/h by 2400, still racing at 6000 (m2run, 6,000
  frames: 143.8 M i960, 160.4 M TGP instructions, 5,918 interrupts, 100 s
  on this machine, by eye on dumps every 300 frames).
- "SOUND BUFFER FULL !!" stays on screen in the race: the game programs the
  8251 at frame 82 (00 00 00 40 4e 37), sends one byte (0xff) with intena
  0, and never enables IRQ 10 nor reads the UART; with no SCSP board it
  gets no answer. Harmless (the game runs); expected to go with step 5.
- The MAME build stops on `SDL2/SDL_ttf.h` when the apt packages of
  `setup.sh --build-mame` are not installed (build_mame.sh's own exit code
  is the make error; check `extern/mame/m2` exists).
- Interrupted MAME builds leave empty `.o` files (23 here: emumem_*,
  luaengine, m68000 cores) and archives without an index; make takes them
  as up to date and the link fails (undefined lua_engine, "archive has no
  index"). Fix: delete every object `nm` rejects and the archives holding
  them. With 15 GB RAM, `build_mame.sh -j4` (extra args go to make).
- Lockstep harness for srallyc: `M2ReplayBus` maps work RAM by
  `machine().board` (0x200000-0x23ffff, no ROM mirror, on 2A-CRX) and takes
  the hashed regions from the trace itself instead of a fixed table; the
  m2trace plugin hashes 0x200000-0x23ffff and records IN2 when the machine
  has the 315-5649 (`:io`), so Daytona traces are unchanged;
  `make_input.py` picks srallyc's ports from the header's game (start IN0
  0x40, one VR, handbrake IN2) and rescales the analog values like the
  runtime; `m2_check.sh` takes `SET=srallyc BUILD=build-srallyc`
  (header `traces/hdr_srallyc.m2in`, no m2sndcheck without a sound program).

- App: window title, launcher heading and launcher.ini header from
  `Machine::title`; the Android import path per set (`M2_ROMSET`); the
  startup dialog is "Startup error" (test_gpu_startup checks it). With no
  Model 1 sound board the app opens no audio at all: `--audio native` hung
  on srallyc's empty sound program before. `build-srallyc/daytona --rom
  roms/srallycb.zip`, 600 frames headless with either audio setting.
- setup.py builds build-srallyc when roms/srallyc or srallycb (.zip/.7z) is
  there (archive names from recompile.py's SETS).
- MAME trace runs (run_trace.sh) took the screen full screen (black) and
  grabbed the mouse and keyboard even with `-video none`; the user had to
  kill them, which explains earlier short traces and a "GEARS saw 2" replay
  divergence (host keys reached the game). Now `-window`, every input
  provider `none` and `SDL_VIDEODRIVER=dummy` (overridable).
- Vita: srallyc is its own app, title ID SRAL00094 ("Sega Rally Recomp
  GL/ImGui"), beside Daytona's DAYT00093, so both install side by side.
  `build_vita.py --set srallyc --gpu-gl --release` (host gen from
  build-srallyc, output build/vita-srallyc; the title ID is always passed so
  a stale CMake cache cannot keep DAYT00093; `--title-id` overrides) built
  a 3.7 MB VPK whose param.sfo holds SRAL00094; not run on hardware.
  Vita main: title from `Machine::title`, no native audio without the
  Model 1 board, the ROM option refuses to switch to Daytona, diagnostic log
  in ux0:data/<set>/. `--fast-inaccuracy`/`--fast-gen` refused for srallyc:
  vita_code runs m2recomp without recompile.py's per-set flags and
  direct_ram/fast_gen assume Daytona's work RAM. CMake regexes have no
  `{n}` quantifier (the first title-ID check rejected every ID).
- Vita colours, srallyc end-of-race fade to white: on the Vita only the
  flat shadow faded, the textured scenery kept its colours. Cause: both Vita
  renderers cache polygon colours across frames (gpu_gl palette rows keyed
  by luma base, colour index and polygon luma; gpu_fast material palettes)
  and never noticed the colour tables changing. Measured with a temporary
  m2run probe (hash per frame, not committed): srallyc rewrites only the
  colour translation RAM (0x01810000), every third frame, at frames
  1764-1956 (before the start) and from 6931 (end-of-race fade, whole 3D to
  white on PC at 7000); palette 3D colours and luma RAM never change after
  boot. Fix: `Video::colour_generation()`, bumped by M2Board::ram_written
  on every write that changes palette RAM's 3D colours (0x01802000-
  0x018027ff), colour translation RAM or luma RAM (luma RAM now excluded
  from M2_DC_SPEED's plain-RAM fast write page so its writes are seen); the
  Vita renderers compare it once per frame: gpu_gl drops its palette rows
  (rebuilt on use), gpu_fast reshades a material's 16 colours on its next
  use. Costs one increment per changed table write. Checked on PC: the
  counter moves at exactly the frames above (probe, not committed); srallyc
  attract 1,800 frames still MATCH in m2native; ctest 19/19 both builds.
  A first version compared the 178 KB of tables with a copy every frame
  (memcmp): dropped, the bus already sees every write. vitaGL VPK built and
  gpu_fast compile check; not yet seen on hardware.
- m2_check.sh no longer doubles the set in output names
  (traces/check_srallyc_race, not check_srallyc_srallyc_race).

Next, in order:
1. Hand brake in the app's controls (runtime has `Inputs::handbrake`, no
   key or button sends it yet).
2. The srallyc drive board's command set is not checked against Daytona's
   model (drive_board.h).
3. Runtime hash check of the recompiled RAM code words.
3b. Vita: run the srallyc VPK on hardware (speed with exact code), then
   per-set flags and RAM layout in build_vita.py's vita_code for the fast modes.
4. SCSP sound board (68000 + 2x SCSP).

## Vita icon/LiveArea images, app name always "Daytona USA" (2026-10-12)

Images in `platform/vita/logo_livearea/` (icon0, bg, startup, template.xml; not
committed) go into the VPK when present, checked at configure time; otherwise the
Vita defaults. The home-screen name no longer depends on the renderer. Not yet
installed on a console (`platform/vita/HANDOFF.md`).

## Vita rebindable controls (2026-10-12)

Launcher Controls page: each game action (pedals, steering, shifts, start, coin,
views) rebinds to one pad button, swaps on conflict, saved in vita.cfg; sticks and
Select chords stay fixed. Effects volume default now 20. Host tests 35/35, Vita
compile checks pass, not tried on a console (`platform/vita/HANDOFF.md`).

## Vita music and effects volumes (2026-10-11)

The desktop launcher's music/effects balance on the Vita (Options, defaults 100/75
because the effects cover the music on the Vita's speakers); details in
`platform/vita/HANDOFF.md`. Not yet heard on a console.

## Shared runtime fast paths split from M2_DC_SPEED (2026-10-11)

`src/runtime/fast_paths.h`: M2_FAST_BUS (TGP ports before the page table,
remembered RAM pages), M2_FAST_TGP (FIFOs as a ring read directly by the TGP),
M2_FAST_CPU (register frames through work RAM; needs M2_FAST_GEN), M2_FAST_GEO
(block reads, culled polygons skip texture reads and lighting). M2_DC_SPEED turns
them all on (Dreamcast unchanged, not rebuilt here); video (16-bit PVR layers),
frame skip and M2_AL stay Dreamcast-only. The Vita turns the four on by default
(`build_vita.py --no-fast-paths` to compare). New for the Vita: the TGP FIFO
pointers are also set in the non-M2_DC_MEMORY constructor, and TgpBoard is not
copyable with M2_FAST_TGP. Parity: desktop m2run with and without all five flags,
race_basic and time_attack 6000 frames: same i960/TGP counts, interrupts, 120/120
frame dumps and (race_basic) WAV identical; ctest 18/18. Console (probe 1, LTO,
saturated race windows at the same 1,465 polygons/frame): 47.6 -> 50.4 emulated
fps; i960 code 7.02 -> 6.10 ms, TGP 3.62 -> 3.30 ms, core 1 np_s 7.5 -> 6.5 us per
polygon (`extern/perf/perf_probe_base.log` vs `perf_prob_modif_1.log`; the probe
clock read calibrated 0.705 vs 0.939 us, unexplained). Not yet measured without
probes. What not to re-propose for the Vita: `--fast-gen` (slower than
`--fast-inaccuracy`: per-instruction checks), fast_gen.py's rewrite_tgp (rejects
the --fast_inaccuracy TGP output; about 0.1 ms/frame to gain), M2_DC_SPIN_SKIP
(Vita race frames end in the idle loop at about 44k instructions, never at the
110k cap), the Dreamcast video paths (the Vita uses M2_VITA_RENDER_OPT's tile cache).

## Mobile upstream rebase and GPU startup diagnostics (2026-10-07)

Rebased mobile onto original alphanu1 upstream/main 0435a46; recovery branch
backup/mobile-before-gpu-20261007. Main/Vita/PSP refs are not modified.
The Xiaomi Mali-G715 report exits normally from SDL_main after GPU creation
fails, not an established native crash. API version alone cannot establish
support; no device feature dump or physical phone is available here.

Shared SDL_GPU creation now uses properties to opt out of unused shader clip
distances, indirect-draw first-instance and anisotropy. Audited game shaders,
samplers and pinned ImGui backend for usage. Depth clamping remains required:
the current pipelines disable depth clipping, so merely disabling that device
feature would be invalid. No shader fork or external dependency edits.
fail() now snapshots the SDL error, logs it and opens an SDL native message
box; dialog failure is logged too. Startup lists compiled GPU backends.
These frontend changes follow the design's Platform layer section.

Mock regression tests cover requested features, property/device failures,
cleanup and error-message preservation even when logging changes SDL's error.
Host app objects and mobile main syntax checks pass; GPU startup, pacing and
TestHold tests pass. Xiaomi runtime compatibility and dialog appearance are
not hardware-verified. Android build initially required matching ANDROID_HOME
and ANDROID_SDK_ROOT; mismatched paths are a local configuration issue.
Android assembleDebug succeeds with both SDK variables pointing to
/home/boucy/Android/Sdk. The arm64 APK passes ZIP integrity and apksigner
verification, contains libmain.so/libSDL3.so, and contains no ROM archives.
Touch-control and mobile ROM-path regression tests also pass.

## Original upstream and platform test-menu integration (2026-10-06)

Merged alphanu1/daytona-arcade-recomp main 1877da9 into the fork, not merely
the fork's origin/main. Recovery branch: backup/main-before-alphanu1-20261006.
Upstream adds the shared TestHold helper and launcher action. Mobile keeps
that shared action with wrapped help text on small displays; Vita exposes
the same one-shot action in its ImGui options, retains physical Test/Service
bindings and does not persist the request. Both ROM versions wait until
frame 240, hold Test for 173 game frames and release, per upstream tests.
Resetting an armed Vita game restarts the full hold; Reset Defaults cancels it.
Inputs and Platform layer sections govern this frontend-only integration.

All 15 targeted tests (13 Vita, pacing and TestHold) pass, as do standalone
touch controls and 13 Android-path plus two desktop ROM-file cases. Desktop
app objects build and mobile launcher/main syntax checks pass with M2_MOBILE.
Vita main_gpu.cpp cross-compiles for both the 1993 and Revision A targets.
Full ROM cross-rebuilding was stopped deliberately in favor of targeted
frontend compilation; no new VPK/APK/IPA or physical device validation is
claimed for this update. Vita and mobile branches are advanced to the
integrated main; PSP untouched.

## Vita and mobile integration into main (2026-10-06)

Rebased psvita-native-frontend and mobile onto fetched origin/main 91560cf.
Mobile's touch controls and iOS file picker were already ancestors of main;
its rebase therefore fast-forwarded without replaying commits. Vita replayed
13 commits, retaining the ImGui launcher, both ROM revisions and platform
settings. The only conflict was this handoff: both upstream licence notes
and platform history were retained. No implementation conflicts occurred.
Recovery refs: backup/vita-before-main-merge-20261006 and
backup/mobile-before-main-merge-20261006. PSP is untouched.

This merge integrates the rebased Vita work into main; mobile is already
included. Publishing uses an explicit lease on the old GitHub Vita tip,
with main and mobile updated by fast-forward in the same atomic push.
Device builds and hardware runtime testing are not repeated by this merge.

Validation: desktop app objects and the selected test targets build; all
13 Vita tests plus app_pacing pass. Standalone mobile touch controls and
Android/desktop ROM-file tests pass. The broad all-target build stopped at
test_fp with unresolved SoftFloat helper symbols in this empty-build-type
configuration; no floating-point implementation changes were made. Mobile
tests are standalone programs/scripts, not named CMake build targets.

## Vita main rebase and ImGui launcher (2026-10-06)

Rebased psvita-native-frontend onto fetched origin/main 50d6638. Recovery
branch backup/vita-before-imgui-20261006 retains the pre-rebase history.
Main and PSP branches are not modified by this work; no Vita push performed.

Replaced the GXM frontend's bitmap menu with pinned upstream Dear ImGui,
rendered through the existing vita2d context. All 28 Vita options, dual-ROM
launching, link settings, physical controls and saved settings remain.
Front touch operates rows, adjustment buttons and the scrolling settings
panel; D-pad selection scrolls into view. UI work runs only in the launcher,
pause menu and loading screen. No additional gameplay display buffering.
The adapter supports the uniform-tint primitives used here, not arbitrary
per-vertex colour gradients. Geometry fringe AA is disabled; font AA remains.

Rebase integration preserves the Vita native-sized backdrop stretch path,
widescreen CPU tile fallback and foreground caches, while adapting the HUD
polygon helper to upstream panel detection. Non-Vita external-renderer
margin handling is retained. Host desktop app compiles; all 13 Vita host
tests pass, including real ImGui draw generation against a mocked backend,
pool exhaustion and GPU shutdown synchronization. These checks do not prove
physical Vita rendering, touch behaviour or gameplay performance.
The Vita ImGui target disables its unused desktop shell-opening handler,
which otherwise links unavailable execvp/waitpid functions from VitaSDK.

Both VitaSDK GPU builds pass. Final dual-ROM package:
build/vita-enhancements/daytona_vita.vpk (01.24, Daytona Recomp ImGui).
ZIP integrity and ARM ELF checks pass; the bundled daytona.self SHA256
matches build/vita-revision-a/eboot.bin. Archive contains executables and
licenses only, no ROM archives. Physical Vita installation remains untested.


## Mobile touch controls and latest main (2026-10-06)

## Vita Revision A link and dual-ROM launcher (2026-10-03)

Rebased psvita-native-frontend with --rebase-merges onto fetched origin/main
9ad266b (three-computer link validation). Recovery branch:
backup/vita-before-link-20261003. Merge conflicts combined enhance and
comm_board in Vita runtime; preserved Vita wide margins and foreground
cache invalidation while accepting upstream scene detection. Main and PSP
branches are unchanged; nothing pushed.

User supplied roms/daytona.zip alongside daytona93.zip. Revision A import
passes CRC validation; its i960/TGP/sound code is separately generated under
build/revision-a-host, never committed. GXM build selects DAYTONA_VITA_ROMSET;
the daytona93 VPK can bundle Revision A's SELF as app0:daytona.self.
Options ROM selector + Start/Reset replaces the process using LoadExec.
Each executable uses its own ux0:data/<set>/<set>.zip, vita.cfg, EEPROM and
backup RAM; existing 1993 saves are not repurposed as Revision A saves.

Vita SceNet IPv4 transport implements the shared TCP ring protocol with
nonblocking connect/accept/read/write, partial-send retention, a 64 KiB
bounded queue, reconnect delay and two-second no-progress timeout. Network
modules/heap are owned only where initialized here. Options expose enable,
four next-IP octets, local/next ports and optional frame sync (off by default).
Reset applies changes; pause status shows local IP, RX/TX and cabinet ID.
1993 link requests are rejected explicitly; Revision A is required on peers.
Master/slave, unique car numbers and matching cabinet/region settings remain
the game's test-menu choices. Native audio now tolerates unsupported effects
as upstream does (linked vibrato omitted), while invalid data still faults.

The host integration uses the real Vita transport through a narrow POSIX
SceNet shim against desktop TcpLink: numbering and data both directions,
37-byte partial sends, stalled-peer loss, reconnect and queue overflow pass.
This is not evidence of Vita Wi-Fi, executable switching or a hardware race.
Those remain the next device checks. CPU500/core options, road renderer,
wide CPU tiles and removal of the buffer selector remain preserved.
Address/undefined sanitizer execution of the socket integration also passes.
Host suite after rebase: 24 tests pass, two optional Lua tests skip.
Both ARM release executables and SELF/VPK builds pass. Final dual package:
build/daytona-vita-dual-rom-link.vpk. Archive checks pass; bundled eboot.bin
and daytona.self match their respective build outputs. The archive contains
only executables, SFO and license documents, no ZIPs or extracted ROM assets.
Build directories: build/vita-enhancements (1993, existing generated input),
build/vita-revision-a (fresh Revision A generation). Both were incrementally
rebuilt after final frontend edits, then bundled in that order.

## Remove configurable Vita display buffering (2026-10-02)

User reports 30 FPS and requests removal of single/double/triple settings.
Removed the menu/config field, physical ring adapter and its obsolete tests.
Old gpu_buffers keys are ignored and disappear on the next settings save.
The same pinned libvita2d now builds without source injection, retaining
upstream's normal three-surface queue and display synchronization. This
removes custom buffering, not the framebuffer storage needed for scanout.

Inspection found no explicit 30 FPS cap: FrameClock retains fractional time
at native 57.524 Hz; the display callback waits one vblank, not two.
Do not claim the reported slowdown is proven caused by buffer count, or
force game logic to 60 Hz. Audio pacing, CPU/core options, renderer and
wide tile fallback are unchanged. Hardware speed remains unverified.
Package: build/daytona-vita-standard-presentation.vpk.
Validation: release cross-build and archive check pass; ELF has upstream
vita2d_swap_buffers and no custom daytona_vita2d symbols. Host CTest:
21 passes and two optional Lua skips. New one-step GXM clock regression
produces 5752 game steps over 6000 simulated 60 Hz display ticks (100 s).
Deleted adapter/test sources remain recoverable in Git history.

## Optional Vita CPU clock and fourth core (2026-10-02)

GXM options now include CPU 500 MHz and a persisted fourth_core switch.
Defaults remain 333 MHz and fourth core off. CPU requests are read back;
rejected/ineffective 500 MHz requests fall back to a 444 MHz request, with
actual frequency shown in options. An overclock plugin/profile may override
application requests; selecting 500 alone is not proof it is active.

Main probes USER_ALL | SYSTEM affinity and falls back to USER_ALL on failure.
Reference sound jobs and both audio callback implementations adopt the shared
mask once per change, with callback caches reset when reopening devices.
No kernel patch/dependency is installed. A working core-unlock plugin is
required. Existing threads can use the additional core; no game-loop
parallelization or audio clock/pacing change is introduced.

Host fallback tests and audio worker/lifecycle tests pass; complete CTest
suite: 22 passes, two optional Lua skips. Vita release cross-build and VPK
archive validation pass. Package: build/daytona-vita-500mhz-fourth-core.vpk.
Actual plugin acceptance, stability and performance require hardware testing.
Wide CPU tile fallback, GPU roads and double-buffer default are unchanged.
Main and PSP remain untouched; no push.

## Wide tile recovery after hardware slowdown (2026-10-02)

User reports moving tile composition onto GXM made the game slower. Disable
the explicit wide GPU tile capability in the Vita frontend: widescreen now
uses the existing CPU backdrop/foreground fallback. Original-aspect GPU tiles,
GPU 3D, the road subdivision change, audio, clocks, physical double-buffer
default and main3044f3b remain unchanged. No additional performance hypothesis
is presented as established; the earlier CPU-only timing excluded GPU cost.

This isolates wide tile composition from the road change, which also can add
vertex work. If this build remains slow, measure/subtract that separately;
do not advertise moving work to GXM as automatically faster. Hardware result
for this comparison build remains pending. Main and PSP untouched; no push.
Package:build/daytona-vita-wide-tile-recovery.vpk.
Validation: VitaSDK build and21 CTest passes (2 optional Lua skips). The
fallback composition and mode transitions are covered by renderer tests.

## Rebase onto GitHub main and GPU tile/road follow-up (2026-10-02)

Fetched origin/main3044f3b and rebased psvita-native-frontend with merge history
retained. Backup:backup/vita-before-main-20261002 at20becae. A preliminary flat
rebase was aborted before continuing with rebase-merges to preserve Vita merge
content. Resolved Video API conflicts by keeping desktop snapshots/instance
tracking and preserving Vita widescreen, plus the renamed HUD copy destination
parameter. Main and PSP branches are untouched; no remote push. The obsolete
Vita frame-skip helper/docs from an old merge were not reinstated; replacement
docs describe the physical GPU buffers (double default).

New main renders tiles on GPU except for per-item HUD relocation, which still
uses CPU foreground composition. Vita now matches that split: GXM composes
centred foreground as well as backdrop, skips CPU pixel composition/uploads,
and retains dirty state for transitions back to the relocated HUD fallback.
CPU tile decode/cache work remains. Main's SDL_GPU shaders are not Vita GXM
binaries; they are retained for desktop, not falsely advertised as a direct
Vita shader port. Audio timing, game cadence and double buffering unchanged.

Main's per-fragment reciprocal-depth UV correction confirms the difference
from Vita's affine tessellation. Add an edge-midpoint texture-error criterion
alongside the existing depth/span minimum subdivision. A synthetic shallow
road triangle goes from46.545 to0.925texels midpoint error, at subdivision8
instead of1. Initial test wrongly expected0.5 at that cap; corrected to check
the measured sub-texel result. The0.5target is not a guaranteed bound, especially
at the eight-way cap or when pool pressure lowers subdivision. This can add
vertex work. No homogeneous matrix or untested shader replacement is used.

Tests exercise scroll-dirty GPU foreground skipping, fallback transitions,
HUD placement, source UV/stretch separation and25440 tessellation cases.
Hardware road appearance, GPU timing and overall speed remain unverified.

Validation: full host build,21 CTest passes with2 optional Lua skips, and
ASan/UBSan renderer contracts pass. The first dirty-transition test omitted
tile_memory_w under write tracking; it now uses the real board notification.
Desktop SDL_GPU Vulkan smoke tests complete120 and1200frames at16:9; the latter
with HUD-edge option enabled. This is not Vita rendering or full-race parity.
VitaSDK build and archive checks pass. Package:
build/daytona-vita-main-gpu-tiles.vpk
SHA256:50dde138308c20680cf6b5a4f9a8ba92abc5c84cb4aa9fb8a4104c90e5a3728b.
Frontend options, controls, audio and physical buffer adapter compare unchanged
against the pre-rebase backup. Source tree is ready for device testing; no push.

## Widescreen GPU backdrop optimization (2026-10-01)

Hardware feedback: all physical buffer choices still slower in widescreen.
Buffer count was not a cure for the extra rendering work. The wide path still
CPU-composed scrolling backgrounds, unlike original aspect's GXM tiles.

Add an explicit GPU-background capability enabled only by Vita's GPU frontend.
Wide screen_update skips CPU background composition/copies; GXM uses the
existing System24 tile upload and composition around unchanged 3D polygons.
Only background destination x changes for stretch, never UVs or 3D projection.
CPU foreground and per-item HUD relocation remain intact. Non-stretched side
margins clear to palette pen0 as in the original GPU path. The old CPU backdrop
path remains when capability is off; normal desktop/PSP behavior is unchanged.
Double GPU buffers remain default; no frame skipping, audio or clock changes.

A synthetic host test (1000 scrolling updates,margin93,empty geometry,constant
character pixels,changing hscroll) measured526.78ms before and238.81ms after
for CPU screen_update, about55percent less. Commands/source are under ignored
build/vita-wide-bench-new.cpp and /tmp/vita-wide-bench.cpp. This excludes GPU
cost and does not establish hardware FPS, visual fidelity or a full-speed game.
Prior homogeneous-WVP failure is not reintroduced: no draw_polygons changes,
shader changes, custom matrix, or HUD algorithm changes.

Host renderer contracts cover margins59/93/200, centred/stretch coordinates,
unchanged UVs and HUD pixels, no CPU backdrop generation, and one CPU layer
draw instead of two. Full host build and21 CTest passes (2 optional Lua skips),
ASan/UBSan renderer contracts, VitaSDK build and VPK archive checks pass.
Package:build/daytona-vita-wide-gpu-background.vpk
SHA256:400df3856a523876c0686cd47a8aac79c7c555b3b0e3d247760fb8e5ed36cb1e.
Hardware rendering and audio checks remain necessary. Main/PSP untouched;
no remote push.


## Vita physical GPU display buffers (2026-10-01)

User requested actual Vita GPU buffering separate from main's software draw
mode, with double as default. Options now persists gpu_buffers1..3; default
and Reset Defaults select2. Old draw_mode remains ignored. No frame skipping,
clock catch-up, audio backend, or polygon-renderer changes are reintroduced.

The pinned public MIT libvita2d source is built privately in the build tree.
display_buffers.inc changes the active GXM surface ring, not board cadence.
Double/triple retain normal display-queue sync; single finishes GPU work and
directly presents the sole surface without queuing identical old/new sync
objects. It may tear during rendering and is not promised faster. Switching
drains GPU and display work, copies the latest frame to surface0 when needed,
waits for scanout, then resets front/back indices. All three allocations stay
alive to allow switching; this does not reclaim framebuffer RAM.

Source archive hash verified. Initial build caught upstream JPEG ceil missing
math.h with the current compiler; target-only forced include resolves it.
All seven pinned shaders' extracted binaries match the installed package
exactly (object metadata hashes differ). No shader compiler used.
The design and dependency records now describe physical buffers rather than
the withdrawn frame-skip options; upstream MIT license is packaged.

Validation: adapter tests cover all mode transitions, VSync on/off, invalid
counts, uninitialized/in-scene/system-app rejection and GPU-before-display
ordering. ASan/UBSan pass;21 CTest passes,2 optional Lua skips. VitaSDK cross-build,
linked buffer-control symbols and VPK archive checks pass. Package:
build/daytona-vita-gpu-buffers.vpk. Hardware switching,
tearing, frame pacing and audio remain unverified. Main/PSP branches untouched.


## Vita pre-draw-mode pacing recovery (2026-10-01)

Hardware feedback after9790e57: game and audio still lag since draw modes.
The synthetic debt test did not establish the cause of native audio stutter.
Back out the Vita draw-mode UI, config loading and presentation gate, and the
follow-up retained-debt clock. main_gpu.cpp is identical to6a6fdc8 apart from
the PACING RECOVERY menu title; controls and input tests match that baseline.
This restores the previous VSync-paced presentation on every frontend loop,
not just when a board frame is due. Saved draw_mode values are ignored and
removed on the next settings save; other preferences are preserved.
Shared main frame-skip support remains merged but defaults to zero and is
not enabled by Vita. Native audio remains device-clocked per the design
document Audio section; neither audio backend nor GPU geometry was changed.
Do not re-propose frame skipping as physical single/double buffering.

Validation: source comparison to6a6fdc8,20 CTest passes,2 optional Lua skips,
VitaSDK cross-build and VPK archive validation. Installable recovery package:
build/daytona-vita-pacing-recovery.vpk. Hardware playback remains unverified;
this is a controlled rollback, not a claim of a measured hardware speedup.
Main and PSP branches untouched. No remote push.


## Vita audio pacing follow-up (2026-10-01)

Hardware report: audio stutters with draw-mode selection. A deterministic
37-second alternating 35 ms / 2 ms host-timing test exposes the one-step
clock discarding whole-frame debt:1114 board/audio steps instead of2128.
Vita GPU frontend now retains at most four frames of debt, still executing
only one board frame per loop. Cheap skipped frames can recover that debt;
no multi-step live-geometry submission or renderer changes are introduced.
Pause/reset clears debt; sustained overload remains bounded. Other frontends
keep the existing FrameClock policy. Audio dispatch stays outside draw gating.

The design document Audio section requires device-clocked native playback.
That backend already has it and is unchanged. This finding explains reference
sample starvation and late main-board commands, not proven native callback
underruns. Do not claim this reproduces or resolves every hardware crackle;
if native music still stutters, capture callback timing and active engine on
the device next rather than increasing latency or changing audio fidelity.

Validation: regression recovers2128 steps, bounded overload/reset tests pass;
20 CTest passes,2 optional Lua skips; public VitaSDK cross-build passes.
Package:build/daytona-vita-audio-pacing.vpk. Hardware listening remains pending.
Main and PSP branches unchanged; no remote push.


## Vita draw modes from updated main (2026-10-01)

Merged origin/main931504e05f4cd471a207c78b275f48e6ce77cc4c into the Vita branch.
Upstream calls frame-skip presets Double Buffered / Single Buffered / Every
Third Frame. They mean draw every1/2/3board frames, NOT physical buffer counts.
The earlier uncommitted buffer-library drafts were moved to ignored
build/paused-buffer-drafts; they are not compiled or shipped. The installed
libvita2d, framebuffer ring and VSync remain unchanged.

Vita Options exposes those three modes, persists draw_mode0..2 (default0),
clamps loaded values and applies on resume. GPU submission/clear/swap is gated
by the same pre-step board frame counter used by M2Board::vblank_end.
No stale live geometry is redrawn on skipped frames: retain the last displayed
frame instead. The clock is capped to one board step per frontend iteration.
Board execution, input and reference/native audio dispatch stay outside the
presentation gate. Menu drawing remains available; zero-step iterations avoid
duplicate scene submission and retain the existing idle delay. SIM FPS label
makes clear the counter measures simulation, not presentation cadence.
The polygon renderer and perspective vertices are unchanged from Wide2.

Validation:20CTest passes,2optional Lua skips. Draw cadence tests sweep invalid
and valid modes over600board frames (600/300/200draws). Public VitaSDK build
and VPK archive checks pass. Three6000frame race_basic headless replays show
identical196665345i960,223429779TGP,12050interrupts,3636sound-command bytes,
and7843660868000instructions. Host times35.18/19.45/14.24seconds are not Vita
FPS. Last-picture hashes differ as expected because skipped modes retain an
earlier frame:ad67233983ea8808 /04b9cd7b8fe2cdda /0504a052c927e9ff.

Artifact: build/daytona-vita-draw-modes.vpk
SHA256c02ffa1702a682062c991cbf569dabafcbe8bd22d055540254fd6281fc8833b1.
Menu title DRAW MODES. LoggingOFF. No hardware result yet.
Main and PSP branches untouched; only the local Vita branch is updated.
Next: compare simulation pace, audio and displayed motion across presets on
real Vita. Lower draw frequency can reduce render cost but also makes motion
less smooth; do not advertise these presets as actual buffer-count changes.


## Wide2: merge GitHub main and Vita options (2026-10-01)

Rebased mobile again onto origin/main 10c85cb before completing touch input.
Kept upstream pacing.cpp and Pacer alongside mobile controller startup, iOS
Files import and screen fixes. Backup: backup/mobile-before-main-touch-20261006;
the touch-work stash remains as an additional recovery copy. No branch pushed.

Shared mobile-only touch overlay: analogue horizontal steering, gas/brake,
sequential gears, four views, coin/start and launcher menu. Raw SDL fingers
capture controls independently (device and finger IDs), not ImGui's single
mouse pointer. Short taps survive until a game frame; focus loss, cancellation,
menu and safe-area changes release input. Existing physical controls merge
with touch through Controls; desktop receives zero touch input by default.
This follows the design's Platform layer & build boundary; runtime/shaders
and upstream pacing remain unchanged. See mobile handoff for validation.

## Mobile rebase onto remote main (2026-10-06)

Rebased mobile onto origin/main 6817bff, preserving Android document imports,
iOS Files import, screen sizing and unsigned packaging. CMake conflict
resolution keeps upstream link/force-feedback sources and Windows socket
libraries alongside mobile SDL targets. Generated-source paths combine
M2_GEN_ROOT with upstream M2_ROMSET. Mobile startup gamepad enumeration now
uses the upstream Devices owner. Initialize the link address before Android's
stale-URI early return. The previous mobile tip is retained at
backup/mobile-before-main-20261006. Main and the other platform branches are
unchanged. Existing IPA files predate this rebase and must not be presented as
rebuilt from it.
Validation: desktop daytona_app compilation passes; 13 Android-path import
tests and 2 desktop-path tests pass with the host SDL shim. Remote main is an
ancestor of the rebased tip. iOS/Android device builds are not rerun here.

## iOS launcher and Files picker (2026-10-06)

Replaced the unsupported SDL iOS dialog with a UIKit document import delegate,
keeping a separate private copy for each selection before the existing ROM
validation. Enabled modern full-screen launch sizing, safe-area placement,
logical-point ImGui styling, text wrapping, a visible scrollbar and blank-space
drag scrolling. Picker errors are shown beside the ROM field. Changes follow
the design's thin platform layer; runtime and shaders are unchanged.

Device Release IPA and simulator build succeed. The simulator cannot provide
visual verification: SDL_CreateGPUDevice reports that its device does not meet
SDL_GPU Metal hardware requirements; the resulting black screen is not UI
validation. Native Files selection and layout need another physical-device
test. See platform/mobile/HANDOFF.md. Do not claim successful picker import or
screen fit based on compilation alone.

## Mobile iOS packaging (2026-10-06)

The iOS build script supports `UNSIGNED=1` for AltStore: disable Xcode
signing, stage the device app under `Payload/daytona.app`, and produce
`build/ios/Daytona-unsigned.ipa`. The bundle template now explicitly supplies
CFBundleExecutable and the APPL package type. Custom build and generated-source
paths are normalized before packaging; argument handling remains compatible
with macOS Bash 3.2. No ROM archives or generated sources are committed.
This follows the design document's Platform layer & build and ROM handling
sections: the shared runtime/renderer is unchanged. See the mobile handoff
for build validation and device-test status.

## Recovery 1 after hardware regression (2026-10-01)

IMG_2856 shows the c3ec891 package losing most textured scene geometry: road
and car surfaces missing, while tiles and some solid/checker geometry remain.
This overrides the host-test success recorded below. The exact GXM failure
mechanism has not been isolated; the host uniform/projection model did not
execute the installed shader and was insufficient to validate the change.

Restored gpu_fast.cpp, perspective_vertices.h, video.cpp/h and corresponding
renderer tests byte-for-byte to1ca8cb3 (git diff --exit-code verified).
Both homogeneous-WVP and wide GPU-tile changes are withdrawn, rather than
shipping another unverified matrix guess. Steering curves, their controls
tests, saved settings and existing Test/Service bindings remain.
Menu identifies DAYTONA RECOMP - RECOVERY 1. Main and PSP untouched.

Public VitaSDK cross-build and VPK archive checks pass. Input tests pass;
restored renderer ASan/UBSan contracts pass, including painter order, checker
parity, wrap/mirror addressing, allocation ownership and shutdown.
Artifact: build/daytona-vita-recovery-1.vpk
SHA256825368c373f9c52dd00329ff16fc9d9e08aed68c0dfa614efe3eed584693e0a5.
Logging remains off. No hardware recovery result has been observed yet.

Road wobble and wide-mode cost remain as in the earlier working renderer.
Next: verify Recovery 1 on hardware before attempting isolated optimisations.
Do not redistribute daytona-vita-perspective-curves.vpk as a working fix;
its host math checks and replay hash did not demonstrate real GXM correctness.


## Vita perspective, widescreen cost and steering curves (2026-10-01)

Work remains on psvita-native-frontend; main and PSP branches untouched.
The Rendering/Enhancements design governs: presentation changes only, native
board timing unchanged, original aspect and linear steering remain defaults.

Road texture warp: previous affine subdivision used1/4/16/64 times as many
triangles depending on depth/span, then reduced precision under pool pressure.
Replaced it with homogeneous positions and the existing libvita2d WVP shader:
clip.w=depth, clip.z=0.5*depth, ordinary UVs. This gives perspective interpolation
with three vertices per source triangle. Painter order, stencil clips, tint,
solid/checker paths and fence ownership are preserved. Matrix changes are scoped
to textured batches and restored on all exits. No custom shader/compiler.
The exported libvita2d matrix is an internal ABI dependency documented with
upstream source revision and installed archive hash in platform/vita/THIRD_PARTY.md.
Recheck it when updating that library. Physical GPU output is not yet verified.

Widescreen had forced every tile layer through CPU composition and full uploads.
GXM now composes background tiles at the selected aspect, and foreground tiles
when centred. Sky margin colour is sampled through the same tile rectangles,
preserving split/window/scroll semantics. Active edge HUD alone uses shared CPU
grouping; unchanged source pixels reuse the grouped layer/upload even when
background scroll dirties drawing state. Pixel comparison, not a hash; extra
HUD source snapshot is about0.74MiB. Further distance still adds geometry.

Options adds persistent Linear/Soft/Extra Soft steering (linear, signed square,
cubic) after deadzone and before inversion. D-pad and full lock are unchanged.
Existing Test/Service mappings remain. Diagnostic logging staysOFF.

Validation:
- Host build and20CTest passes;2optional Lua tests skipped.
-5151 perspective sample comparisons and matrix projection/depth checks.
- Actual renderer submits3vertices for a depth100:1, wide triangle; no subdivision.
- All256stick positions across curves, monotonicity, endpoints/invert/D-pad tests.
- Final standalone renderer contracts pass, including eight sky comparisons
  (four split modes x normal/line scroll) against CPU top-left composition,
  unchanged-HUD upload reuse and matrix restoration.
- ASan/UBSan renderer contracts pass including sky comparison; final later test
  fixture enlarges the long-depth triangle, with the ordinary contracts rerun.
-6000frame16:9/edge-HUD/default-distance host replay:196665345i960,
  223429779TGP,3636sound-command bytes, hash9047513777edfaae, unchanged.
  42.27seconds on this host is not a Vita performance measurement.
- Public VitaSDK cross-build and VPK archive validation pass. Package:
  build/daytona-vita-perspective-curves.vpk
  SHA25666f209118967cb3c0beb55453d54a0f4dfb5b88c1bfda1b09bd683e2656aee87.
  Previous package retained at
  build/vita-enhancements/daytona_vita-before-perspective.vpk.

What not to re-propose: more affine subdivision to hide road warp costs both CPU
and vertex pool and remains approximate. Do not restore blanket CPU wide mode.
The former extreme-UV test rejected a finite polygon only because CPU u*q
overflowed; homogeneous submission keeps it finite, and its expectation was
updated. Host Ninja reported a truncated log and rebuilt fully on a subsequent
invocation; builds were sequential in each directory and completed successfully.
Next: confirm road lines, HUD/sky/clip transitions and frame pacing on real Vita.
No device FPS, crash-free-runtime or visually-fixed claim from these host tests.


## Vita GXM presentation options and cabinet binds (2026-10-01)

Switched to psvita-native-frontend and fast-forwarded to origin/bc02bb0 first.
No PSP files or commits were brought across; main and PSP branches untouched.
Upstream already contained desktop widescreen, per-item race-HUD gating and
draw-distance hooks, but the GXM frontend had none of their options connected.

Added persistent Aspect0..3, HUD edges and Draw Distance-2..2 settings, live
application on resume, scrolling16-row options and unchanged defaults.
Original GXM System24 fast path remains. Wide modes use shared CPU tile/HUD
composition, widened geometry clip planes and matching GXM projection/clip/HUD
offsets. Layer textures reserve896x384 for up to21:9;12MiB layer arena raises
totalGPU reservation30->32MiB. Wider/further options may cost FPS; no physical
performance claim.21:9 fits with vertical letterboxing.

Select+Triangle maps Test and Select+Square Service, consuming coin/view inputs.
Plain Select coin now triggers on release so staggered chords do not insert
coins. Menu latch suppresses a coin on resume; Start+Select remains frontend menu.
Controls regression covers both chords, staggered press and release.

Found existing generated C++ lacked hook_draw_list. Regenerated privately into
build/vita-enhancements-input/gen with seeds/daytona93_hooks.txt; onlychunk012
differs. Vita build links enhance.cpp and uses that generated tree. No generated
game code/assets are committed.

Validation complete: full host build,20CTest passes (2optional Lua skipped),
11build-helper tests and final GPU lifetime/layout/HUD ASan/UBSan pass.
Default and Furthest6000frame host16:9/HUD-edge replays complete:
default196665345i960 instructions, hash9047513777edfaae; furthest202533889,
hashc45f82273dcf08c6. Both223429779TGP instructions and3636sound-command bytes.
These are host replay results, not Vita speed or physical visual validation.

The first cross-build used old hookless generated code and was discarded.
An interrupted retry initially overlapped; both owned build trees' processes
were stopped, then a single build was resumed and completed. A host replay
link first missed the ymfm include path; corrected before the recorded runs.
The build helper now rejects GPU packages with a missing generated hook.

VPK: build/vita-enhancements/daytona_vita.vpk, archive integrity verified.
SHA256:95523616c6226f77293c8f71122f483aa1bbb9bc9973cc2e017483d9ba14da42.
ELF contains hook_draw_list and shared hud_polygon_offset; frontend objects
are newer than the final settings/control/source edits. Logging remains off.
Evidence: build/vita-enhancements-{package,final-tests,asan-final,
race-default,race-furthest}.log. Physical Vita testing remains required,
especially FPS at higher scenery levels and HUD appearance.
Changes are local on psvita-native-frontend; no push to main or PSP.

## Current state

**Force feedback: centring driver override, Linux (issue #16).** A PS2
Driving Force on Linux feels crashes but no centring on bends: the kernel's
Logitech driver offers, as far as known (hid-lg4ff from memory, not checked
here), constant force and autocentre but no spring, so the board's centring
spring is never played (autocentre is off: the game centres the wheel).
rt::DriveBoard::motor(x): the force the board's own closed loop drives for a
wheel at x (springs towards the centre over 8% beyond a 2%/6% dead zone,
uncentring away, the constant force as is, resistance nothing). Controls tab,
Linux only: "Centring driver override" (ffb_centring_override, off); on, the
constant force carries motor(wheel) from the game's own steer value
(io().inputs.steer) and the spring effect carries nothing. main passes it on
Linux only. Also: test_app_controls counted every joystick, so a real wheel
plugged in failed it (#16); it counts on top of what is there. Tests pass;
not tried on a wheel without a spring.

**Hide mouse cursor in game (issue #18).** Launcher, Game tab, next to
Fullscreen: "Hide mouse cursor in game" (hide_cursor, saved, off by default).
One rule in the main loop, every pass: hidden when the option is on, a game
exists and the launcher is closed; shown otherwise, so Esc (or a fault that
opens the launcher) shows it and Resume or Start hides it again, without
touching each place in_launcher changes. While hidden, ImGui's
NoMouseCursorChange is set: its SDL backend shows the cursor on every frame it
draws, and mobile draws its touch controls during play. Tried in the desktop
app (2026-10-07): hidden in play, shown on Esc, hidden again on Resume.

**Hold Test button (the test menu without F2).** rt::TestHold
(src/runtime/test_hold.h): holds the cabinet's Test switch (in0 0x04, active
low) for 173 frames (3 s), from game frame 240 at the earliest, then reports
it is done. Launcher, Game tab, next to Start: "Hold Test button"
(Config::hold_test, not saved; it clears itself when the hold ends); F2 and
any Test binding still work. For mobile, which has no F2 key. Measured first
(m2run, screen hash at frame 2,400): the game wants a fresh press from about
frame 200 after power-on; a press already down then is ignored even if held
to frame 287; a 3 s hold from frame 200, 240, 600 or 1,200 gives the same
screen as a 10-frame press at 1,200 (the test menu; the menu's
Test-selects-an-item does not fire). Revision A the same. test_test_hold. Tried in the desktop app
(2026-10-06): the test menu opens and the box clears.
Left for the Vita frontends' developer: GPU25 has no Test item; the plain
frontend's TEST SWITCH presses for one frame only, which can be missed; both
can use rt::TestHold. Dreamcast has no way into the test menu yet.

**Licence: BSD-3-Clause (LICENSE).** "Copyright (c) 2026, Ben Templeman and
contributors". The same licence as the MAME code transplanted into the
runtime; every linked component is compatible (BSD, zlib, MIT, public
domain). rules.md rule 9, THIRD_PARTY.md and the README say so; the open
decision is gone. GPL code (the MiSTer core, Supermodel) stays read-only.
Docs also brought up to date: the mobile notes folded into Current state,
PR #8's CI, branches now on main, #6 (Android and iOS, with touch, merged),
and the README's platform table: Android, iOS and PS Vita tested on devices
and working (2026-10-06), Dreamcast in Flycast.

**Force feedback: confirmed, enhancements documented (issue #9).** The
reporter confirmed on a Simagic Alpha Mini: no shaking, centring works, no
cut-outs. Documented in docs/issues.md, not started: a safeguard against
force feedback cutting out (SDL's DirectInput update reacquires a lost device
and retries, but its effects stay stopped: restart any effect
SDL_GetHapticEffectStatus reports stopped, about once a second; a theory from
SDL's code, seen in another recomp, not in Daytona), and settings asked for
after FFBPlugin: strength up to 200%, centring spring, resistance, a damper,
minimum force, with defaults that keep today's force feedback.

**Android and iOS (PR #14, platform/mobile).** The desktop runtime and its
SDL3/SDL_GPU renderer packaged for Android (arm64, SDLActivity, the game as
libmain.so) and iOS, from game code generated on a desktop (M2_GEN_ROOT). A
touch overlay on mobile only: analogue horizontal steering, gas and brake,
sequential gears, the four views, coin and start, and the launcher menu.
Fingers are tracked by device and finger ID (not ImGui's one pointer), short
taps last until a game frame, and focus loss, cancellation, the menu and
safe-area changes release them; touch merges with physical controls through
Controls, and the desktop gets no touch input. Android reads the ROM document
through SDL; iOS imports a private copy of each Files selection (a UIKit
document picker), the launcher fits the safe area and scrolls. UNSIGNED=1
platform/mobile/ios/build.sh makes build/ios/Daytona-unsigned.ipa for AltStore.
Runtime and shaders unchanged (the design's Platform layer & build). Checked:
Android assembleDebug and iOS device Release builds, plist lint, the unsigned
IPA; the ROM import and touch unit tests. Tested on devices (2026-10-06):
Android, iOS and PS Vita all play with no issues reported. Not recorded yet:
performance figures. (The iOS simulator cannot be used: it stops at
SDL_CreateGPUDevice, no SDL_GPU Metal there.) Found on an Android device: a
build that saved the picker's content:// URI in the config, reopened at the
next start, crashed on opening; a saved content:// is now cleared and the
archive asked for again (a Browse result is staged into app-private storage
first). If it still crashes at start, take a logcat before changing the import
again. Details: platform/mobile/HANDOFF.md.

**Windows setup: pin Clang discovery to VS 2022.** Both setup scripts now
select the documented VS 2022 toolchain when newer versions are installed
alongside it. CMake receives the matching generator and installation path;
mismatched cached configuration is cleared, preserving generated sources.

Checked: three setup regression tests pass. Revision A built with VS 2022
and Clang 19.1.5; 12 CTest tests passed, two optional Lua tests skipped.
CI now runs the setup tests and builds on branch pushes. Remote CI passed
(Build and Vita compile check; merged as PR #8).

**Frame pacing and fullscreen mode (issue #7).** src/app/pacing.h: Pacing
(Clock, Display, Vrr) chosen from the display's refresh and three settings,
all off (pace_smooth, pace_sync_display, pace_vrr), and Pacer, the per-pass
frame count. Clock is the old loop moved over unchanged (test: identical
frame counts over 20,000 random passes). Display: a frame every N refreshes
(Smooth: a multiple of 57.52 Hz within 1%; Sync: the first division of the
refresh at or under 61 Hz, if 56 or over: 60 on 60/120/180/240 Hz, nothing on
144/165), capped by the wall clock (vsync forced off) and restarted after a
stall. Vrr: one frame per pass, SDL_DelayPrecise to a 1/57.52 s deadline,
vsync kept (not IMMEDIATE as first planned: VRR with vsync and a frame cap
does not tear). Reference audio's stream ratio is scaled by game_hz/57.52 so
Sync to display does not overrun the 240 ms queue; native audio keeps its
device clock. The refresh is read every pass, so pacing follows the window
between displays (seen: 60 and 144 Hz). Fullscreen mode (fullscreen_mode,
"WxH@Hz"): the display's modes in the launcher, applied with
SDL_GetClosestFullscreenDisplayMode/SDL_SetWindowFullscreenMode. Measured
over 15 s in the game: default 57.9, Sync on 60 Hz 59.8, VRR 57.53 frames/s.
Design doc's open question on pacing answered. Not tried on a VRR display;
not listened to with Sync on.

**Legacy Logitech wheel support (issue #4).** The original Driving Force
(046d:c294) is listed but sends nothing through SDL 3.4.16's HIDAPI lg4ff
driver, which reads c294 reports only when they are exactly 27 bytes; the
reporter confirmed it works with SDL_JOYSTICK_HIDAPI_LG4FF=0. A setting, not
a change for everyone (SDL's driver serves the wheels it was written for):
Controls tab, "Legacy Logitech wheel support (Restart Required)",
legacy_logitech_wheels (on by default on Linux, whose kernel driver gives
Logitech wheels force feedback; off on macOS, where SDL's lg4ff haptics are
the only force feedback for them). On, main sets SDL_HINT_JOYSTICK_HIDAPI_LG4FF to
"0" before SDL_Init (SDL reads it when it finds devices; a change while
running is not guaranteed to hand the wheel back to evdev) and logs it.
Hidden on Windows, where SDL leaves lg4ff off already (hid.dll). The wheel's
GUID differs between the drivers, so it is bound again after switching.
Checked: a throwaway --profile with the setting on logs it, runs and saves it;
all CTest tests pass. Not tried with a Logitech wheel here.

**Audio tab: music and effects volumes (issue #10).** Launcher: a new Audio
tab with Volume, Mute, Music, Effects (music_volume, effects_volume: 1 and 1
by default) and the Native audio switch, moved from the Game tab. Reference
audio: at each MultiPCM key-on, SoundBoard::voice_channel reads the owning
channel from the driver's voice pools in 68000 RAM (0xf01500 and 0xf01618, 28
records of 10 bytes; byte 0 nonzero in use, bit 3 the chip; byte 1 the slot
code; byte 3 the channel; byte 6 0xff on the engine layers' reserved records,
skipped: they carry chip 0's bit with chip 1's fixed slot codes, which gave
1,597 false double matches in a race before they were skipped). Music is
channels 0-9 and 15 (NativeSoundSequencer::music_channel, the driver's "stop
music" set); a key-on with no record (the engine's fixed slots 18-27 on chip
1) is an effect. MultiPcm sums music and effect slots apart and scales them
before its 16-bit clamp; at 1 and 1 the sums are added as before. Native
audio: VoiceEvent::music, NativeSampleMixer::set_volumes per voice, set from
the audio callback through atomics.
Checked: class against the native sequencer's channel, note for note
(scratch program on the oracle's pairing): daytona93 race 4,899/4,899, attract
2,274/2,274; daytona race 4,892/4,892, attract 2,095/2,095; one voice record
per key-on. race_basic reference WAV byte-identical before and after (20 MB);
screen hash ad67233983ea8808 and instruction counts unchanged. Music alone +
effects alone = the full mix except 0.097% of samples, where a chip clips in
the full mix. Race RMS: effects 0.195, music 0.059 (10 dB). New tests:
multipcm_volumes, native_sample_mixer's volumes case; all CTest tests pass.
Not listened to on this machine.

**Force feedback: Daytona's drive board commands (issue #9).** The command
meanings taken from Supermodel's later drive boards were wrong for Daytona:
the game's centring spring (0x39-0x3C, on all race) was played as a 60 ms
sine, so wheels shook constantly with no centre pull, and pads buzzed. Checked
against the drive board's own program, EPR-16488A: disassembled in MAME
0.289, then run with Lua taps on its I/O (commands fed on port $27, wheel
positions on its ADC at $80, motor power and direction read from $46).
rt::DriveBoard now follows it: motor on/off (0x0-), one effect at a time with
strength in the low 3 bits (0x2- resistance, 0x30-37 centring spring, 0x38-3F
the same with a dead zone, 0x4- uncentring, 0x5-/0x6- constant force), no
force for 0x1- and the x8-xF halves, queries (0x8- up) changing nothing; no
vibration (the board has none). app::ForceFeedback: spring (negative for
uncentring), friction and constant force; pads rumble only for the constant
force and uncentring; a wheel whose haptics fail to open gets nothing instead
of SDL's DirectInput rumble (a sine through the wheel) and is retried every
3 s; failed effect updates are retried; pausing keeps the board's state
(ffb.reset() on a new game). Launcher: "Log force feedback" (ffb_log, off).
Checked: test_app_controls and all CTest tests pass. Not checked on a real
wheel; which of 0x5-/0x6- is left on a cabinet is not known (Invert force).
Details and the command table: docs/issues.md.

**Open issues reviewed (docs/issues.md).** Each open GitHub issue checked
against the code: #4 (Driving Force on Linux: SDL 3.4.16's lg4ff driver drops
input reports that are not 27 bytes; ask for a run with
SDL_JOYSTICK_HIDAPI_LG4FF=0), #7 (frame pacing; four display settings
planned, all off, so the default stays native speed, 57.52 frames/s), #9
(force feedback; the drive board's commands checked against its program),
#10 (reference audio's mix matches MAME; native audio's master gain 1.95 and
missing sample envelopes) and #6.

**Dreamcast port (in progress; on main since PR #11).** Now about 32 frames/s in
Flycast (reported 2026-10-06; the work behind it is not on main yet, so the
measurements below stop at the older figures). platform/dreamcast: a
KallistiOS build run by platform/dreamcast/build_dreamcast.py after the
desktop build, from the `daytona` (Revision A) set only, like platform/vita.
Runtime changes are behind M2_DC_MEMORY (ROM read through a page cache the
frontend supplies, texture and frame buffer RAM supplied, a two-level page
table, no GPU-layer copies, lazy rasterizer buffers, reused Lockstep
callback slots, geo_test's cursor moved on without its loop), M2_DC_SPIN_SKIP
(the game's frame-wait loop at 0x1394 skipped by whole passes to the next
lockstep event: 79% of the i960's instructions in a race) and M2_DC_SPEED
(the same output, faster: tile rows with nothing to draw skipped, geometrizer
object data parsed only for frames that are shown, frame skip 3 allowed, tile
layers composed in the PVR's 16-bit formats);
the desktop defines none of them. Checked after the changes: build-daytona m2run
race_basic, screen hash 9427a612c5cb7511, 909,312,001 i960 and 195,261,176
TGP instructions, as before. In Flycast a whole recorded race (race_basic) runs,
drawn by the PVR with textures and both tile layers, in lockstep with the
desktop at all 100 checkpoints; 25 frames/s with every 2nd frame drawn, the default, with sound (175.9 s for the race; 29 frames/s, 207 s, with every 4th; 1,414 s at the start); with the controller it runs until Flycast is closed. The geometrizer's transforms by the SH-4's ftrv (M2_DC_NATIVE_GEO, picture only; --no-native-geo turns it off): race 240.8 -> 237.0 s in Flycast, lockstep exact. Tile layers composed again only on the lines that changed (M2_DC_SPEED, Video::compose16; dcmemcheck's layer hashes identical): 237.0 -> 228.5 s. The race's scenery layer (pixmap layer 2) drawn by the PVR from a video RAM texture at its scroll instead of composed every frame (M2_DC_SPEED; dcmemcheck rebuilds and compares it): 228.5 -> 218.2 s. The geometrizer's ROM reads in blocks (GeoPtr::read, M2_DC_SPEED; dcmemcheck identical): 218.2 -> 212.8 s. Two remembered ROM pages per region and an FPU finiteness test in the renderer: 212.8 -> 205.3 s. The i960's and the TGP's instruction counts kept in registers by fast_gen.py (dcfastcheck identical every frame): 205.3 -> 195.1 s. Renderer radix sort and colour cache: 195.1 -> 193.4 s. No library memcpy/memset for kept polygons and register frames (M2_DC_SPEED): 193.4 -> 187.5 s. Tile block compare without memcmp: 187.5 -> 186.5 s. Culled polygons skip their texture reads and lighting (M2_DC_SPEED): 186.5 -> 180.1 s. Two-level entry switch and binary chunk search in the i960 code's dispatch (fast_gen.py): 180.1 -> 176.9 s. The runtime reads count + Lockstep::pending during a call, so the generated code stores its count once instead of adding it (M2_DC_SPEED): 176.6 -> 175.9 s, 0.21 MB of RAM back. The play build prints no TRACE lines and reports every 1,800 frames (the serial port is slow on a console). Found for the desktop too, not changed
there: Lockstep::calls_ never shrinks (a callback every 1,024 instructions;
estimated 270 KB a second on the desktop). KOS 2.2.1 in extern/kos-dc
(git-ignored): DreamSDK R4's installed KOS master stops every C++ program
using libstdc++'s exceptions at startup. Details, measurements and failures:
platform/dreamcast/HANDOFF.md.

**Vita build: the ROM set.** The 1994 set's M2_ROMSET broke the Vita compile
check (its CMake builds the runtime itself, without the define);
platform/vita/CMakeLists.txt defines M2_ROMSET="daytona93", and
rom_import.h falls back to daytona93 when a build does not say. Checked on
the link-play branch's CI: Vita compile check passes (with link play's
comm_board added to the Vita runtime there).

**Link play (on main).** Revision A's communication board
(837-10537), from MAME's m2comm simulation: src/runtime/comm_board.{h,cpp}
(the protocol: shared RAM set-up at cn_w, the master's 0xff/0xfe numbering
tokens round the ring, every frame each cabinet's 0xe00-byte block from
shared 0x2000 to the next, landing at 0x21c0, and the master's 0xfc vsync;
frame sync optional, off by default as in MAME). M2Board::set_link attaches
it to a LinkTransport; with none (the default, and daytona93) the comm
registers stay plain, so nothing changes. src/app/link_socket.cpp: TCP, this
cabinet listens, connects to the next (retried until it answers), non-
blocking reads, whole-frame sends with a 2 s limit, POSIX or Winsock. App:
launcher Game tab "Link play" (on/off, port, next host:port, frame sync,
status line); daytona --profile NAME for a second cabinet on one computer;
m2run --link-listen/--link-next/--link-sync and --save-nvram. Master and
slave are the game's own settings (test mode > GAME SYSTEM: LINK ID, CAR
NUMBER; factory: MASTER, car 1, twin). Checked: tests/test_comm_board.cpp
(two boards in memory: numbering, data both ways, frames in pieces, loss);
two headless m2run cabinets on localhost (master: factory EEPROM; slave: one
set to LINK ID SLAVE, CAR NUMBER 2 through test mode by script): the
linked attract (通信システム 2人まで対戦できます), both through the linked
course and transmission select, a race with POSITION /2 on both, red car
(car 1) and blue car (car 2); "cabinet 1 of 2" / "2 of 2"; the link lost
when the other cabinet quits. Not checked: two computers, Wi-Fi, frame
sync, more than three cabinets.
First two-computer try (Mac 192.168.1.52 slave, port 15112, next
192.168.1.2:15113; the other the master): NETWORK CHECKING, because the
other computer was not listening on 15113 (connection refused: its port was
still the default 15112). The launcher's waiting status now says which half
of the ring is missing (next cabinet reached or not; a cabinet connected to
this port or not), and the help says every computer can use the same port.
Then linked on two computers once the cabinets' settings matched (the game
cancels the link, "CANCELLED", when they differ: one was DELUXE/USA, the
other TWIN/JPN). Native audio then stopped the game: linked play's music
turns on the MultiPCM LFO (MIDI controller 0x01, value 1, channels 0 and 5;
found with the new m2run --native-audio-check on a headless linked pair,
frames ~1,510), which the native mixer does not have, and the app treated
any unsupported effect as a fault. Now it plays without the vibrato and
says so once in the log; invalid data and callback failures still stop it.
Open: the LFO in the native mixer (the driver's controller 0x01 handler to
MultiPCM registers 6/7, MAME's multipcm LFO).
Tested by the user: three computers linked (Windows, macOS and Linux), all
played well.

**The seed_scan seeds checked against MAME (daytona93).** A local MAME
build (scripts/build_mame.sh) and scripts/m2_check.sh on every scenario: the
11 in scripts/inputs and two new ones, attract_long (9,000 frames of
attract) and race_to_end (race_basic played on to 20,000 frames: out of
time, game over and the screens after). All match MAME in every check:
native i960 code (every device event and interrupt), the native
geometrizer, the CPU 3D layer, the composed screen, the recompiled TGP and
the sound 68000. race_to_end alone: 2.16 billion i960 instructions, 19,997
frames, 20.2 million polygons, 597 million TGP and 261 million 68000
instructions identical. Coverage (MAME's indirect-branch log against the
seed list): the 11 old scenarios reach all 334 harvest seeds and none of the
248 seed_scan seeds (they are the harvest's own scenarios); attract_long and
race_to_end reach 81 of them (10 of 19 game modes, 13 of 29 task states, 3 of
85 jump-table entries, 5 of 16 lda tables, 48 of 92 ROM-record handlers, 2 of
7 data-ROM pointers), now checked. The other 167 need states no scenario
reaches yet (other endings, name entry, link play). Found on the way:
- An Apple silicon MAME differed from ours in the geometrizer's last bit
  (attract frame 173): clang fuses a*b+c into FMA on arm64 by default;
  build_mame.sh now builds it with -ffp-contract=off, like our code.
- build_mame.sh picks a Python whose XML parser loads (Homebrew's Python
  3.14 here had a pyexpat built against a newer libexpat: "No parsers
  found"); setup.sh --build-mame on macOS installs sdl3 and pkgconf (MAME's
  macOS front end is SDL 3, found through pkg-config), not sdl2.
- The first m2_check run of race_to_end stopped at 6,897 frames (cause not
  found: MAME's output goes to /dev/null there); run again by hand, MAME
  recorded all 19,997 frames and every check matched.

**Setup builds the 1994 set too.** scripts/setup.py builds every set it
finds in roms/: daytona93 into build/ (as before), daytona (Revision A) into
build-daytona/ (configured as the main build, with -DM2_ROMSET), and prints
how to start each. The rejection and no-ROM messages name both sets.
getting-started.md and the README describe both, including Revision A's
first run (a single cabinet set in test mode). Checked: setup.py from a
configured tree with both sets in roms/: both games built, tests pass.

**Widescreen: scene or 2D screen, by window and coverage.** The margins'
fill (stretched or sky behind a 3D scene, each row's edge colours on a 2D
screen) was chosen by "the 3D covers half the screen". Attract and race
camera shots that look at a lot of sky cover 36-49% (measured: daytona93
attract frames 720-780, 6720-6860, 7020-7140, 8320-8460; Revision A the
same kind), so those frames smeared their sky's edge colours sideways and
then snapped to stretched when the camera moved. Video::scene(): one window
and at least 15% coverage. Measured over attract and race_basic on both
sets: daytona93's car and circuit select draw their 3D in 2-3 windows,
Revision A's select screens have no 3D, titles none; every one-window frame
with 15-49% is a scene. Checked: daytona93 attract frame 760 now stretched,
circuit select still edge colours (16:10, stretch on).

**Wheels, pedals and force feedback.** Controls gained a third binding
column, Wheel / joystick: every SDL joystick is opened (app::Devices), and
an action binds to an axis, button or hat direction of a device by GUID, so a
wheel, pedals and a shifter can be separate devices. An axis is calibrated
when bound: the capture records where it rested and how far it was moved
before being let go (pedals resting at either end or short of full range; a
900-degree wheel's chosen lock). Saved as `<action>.joy=` lines, with
`joy_deadzone`. Force feedback: the game writes the drive board's command to
I/O board dual-port RAM byte 0x11 (found by logging its writes: 44 in a
race, every type), which IoBoard queues; rt::DriveBoard decodes them (the
meanings below were wrong for Daytona and are replaced: see "Force feedback:
Daytona's drive board commands" above. Then: the
command set of Sega's later drive boards, as Supermodel documents it:
0x1- centring, 0x2- friction, 0x3- vibration, 0x5-/0x6- pull right/left, 0xc-
reset; 0x0-/0x4- sequences and 0x7- not modelled) and app::ForceFeedback
plays them on the steering device: SDL haptics (spring, friction, sine,
constant force on the steering axis; only changed levels sent) or rumble.
Launcher: Force feedback strength (70% default, Off) and Invert force; m2run
prints the commands by type. Checked: tests/test_app_controls.cpp with SDL
virtual joysticks (a wheel and pedals resting at +32767; config round trip;
ADC values; a wheel paddle; unplugging; drive command decoding; rumble and
its scaling and stop). Not checked: a real wheel or force feedback (none here; user
testing): the launcher marks the wheel column, its dead zone and force
feedback Experimental; the direction of the pull may need Invert force.

**The 1994 set (daytona, Revision A) builds and runs, for comparison.**
CMake `M2_ROMSET` (daytona93, the default, or daytona), one set per build
directory: `scripts/recompile.py --set daytona --build-dir build-daytona`
with the set at roms/daytona.7z. The importer has both sets' MAME tables
(Revision A: other program, sound program, two main data ROMs mirrored from
0x800000, two polygon and two texture ROMs; the TGP program is the same
2,024 words, at main_data 0x800020). Saves are per set (pref folder
daytona-recomp/<set>). Seeds: seeds/daytona.txt, the daytona93 seeds carried
over by code (new scripts/seed_map.py: 469 of 582, 175 of them by
alignment where the revision inserted code), then seed_scan.py --set
daytona (239 more; 38,001 instructions reachable). Revision A does some
float maths on the i960's FPU instead of the TGP: m2recomp gained addr, subr
and modi (MAME's semantics; register forms only; daytona93's generated code
unchanged). Hooks: seeds/daytona_hooks.txt, draw distance at 0x17508
(daytona93's draw-list routine moved by 0x490: the same masked instructions,
RAM and boot-time budget); checked: -2 and +2 change the frames as on
daytona93. Its default settings are a linked twin cabinet, which waits on the
link at the settings screen: set a single cabinet in test mode (F2) once;
m2run and m2gpushot `--nvram DIR` load the app's saved EEPROM and backup RAM
(tools/common/nvram.h). Checked with that: 12,000 attract frames, and every
input script (races on all three courses, steering, test mode screens)
runs with no missing code; race_basic plays through circuit select,
transmission select, the rolling start and the race, with drive board
commands. Native audio works on Revision A too: its sound program has the
native sequencer's tables from 0x505a on 0x48 bytes later, their pointers
with them (sequence banks and engine sound tables unmoved); the sequencer
detects the layout by those tables' first pointers. scripts/
test_native_sound_oracle.sh takes the set from the ROM folder's name and
M2_NVRAM=DIR: Revision A race, 6,000 frames: 4,892 of 4,893 notes the same
as the reference audio (the extra one at the run's last instant), the same
two pitch differences as daytona93, RMS ratio 1.02, no invalid data.
Why: the arcade's wheels sit inside the wheel arches with a gap; ours (and
MAME's) poke out over the wings. The car code and data are the same in both
revisions (wheel table at 0x234af4 / 0x230d54: ±0.525, 0.32, 1.4125/-1.4;
body and wheel models on shared polygon ROMs), and Revision A here frames
the arcade footage's bridge shot (frames 8680-8780) almost exactly and still
shows the tyres over the wings: not a revision difference. In attract the
car is moved by the course-following routine (0xca40), so body roll is 0
and pitch small; the full physics (0xf31c) does not run. Open: what on the
board differs (the geometrizer port, the TGP).

**Super sampling (hardware renderer).** Launcher > Game > Super sampling:
Off, 2x, 3x, 4x (`supersampling=` in launcher.ini; m2gpushot
`--scale N`). GpuRenderer::render draws into a target `scale` times the
frame: vertices stay in original pixels (the viewport scales them), clip
rectangles and the depth buffer scale, the tile shaders map target pixels
back to original ones (tiledata[2]; tile pixels repeated), and textures
take a finer mip level, log2(scale) levels (texlod + 128 log2(scale); the
rasterizer picks levels from z, not screen size). The checker pattern is
per target pixel. main draws it into its own texture with mip levels and
shows the level nearest the window's size, so a frame bigger than the
window is averaged down (supersampling). 1x: byte-identical to before (104
race frames, 16:9). race_basic 16:9 headless (Metal): 455 / 389 / 295 /
265 frames/s at 1x / 2x / 3x / 4x. Checked: a 3x race frame (no cracks,
HUD in place) and the app at 3x (700 frames).

**Tilemaps, step 2: drawn on the GPU (hardware renderer).** m2.hlsl
ps_tiles_back / ps_tiles_front compose the System 24 layers per pixel with
Video::draw's rules (window masks, per-line scroll, the split modes, the
back pass's opaque 3 and 2), then the widescreen margins as fill_margins
(edge colours, the sky's colour, or stretched). Inputs: the decoded pixmaps
(a u16 per pixel, pen and category; uploaded by the span of tile rows
changed since the last upload, by the tile generations; never with
SDL's cycle flag, which would drop the rows not sent), and a per-frame
snapshot Video takes at screen_update of tile RAM 0x4000-0x6fff and the
4,096 pens. Video's external-3D `cpu_layers` is now `desktop`: the CPU only
estimates the 3D coverage (margin fill) and, with the HUD at the edges in a
race, still draws the front layers and moves the HUD blobs (uploaded as a
texture then). Checked against the previous build (CPU-drawn layers):
byte-identical frames, 0 pixels differ, over race_basic at 4:3, 16:9,
16:9 stretched and 21:9 with the HUD at the edges, the advanced and expert
courses, test mode and test drive (622 frames; the races use split modes
and per-line scroll). race_basic 4:3: 400 -> 526 frames/s; game 1.82 ->
1.03 ms, renderer CPU 0.53 -> 0.33 ms, GPU 0.16 -> 0.54 ms. Tested by the
user on macOS (Metal), Windows and Linux.

**Tilemaps, step 1: decode only changed tiles (both renderers).** Measured
first (m2gpushot --bench now reads Video's own timers): of the hardware
frame's 2.2 ms game time, the CPU tilemaps took 1.27 ms: decoding the four
512x512 layers 0.55 ms (all 16,384 tiles, every frame), drawing them with the
scroll/split/mask rules 0.63 ms, composing 0.09 ms. Video::decode_layers
(desktop; the Vita path keeps its own cache) re-decodes only tiles whose
value or character changed, comparing character RAM (256-byte pages, then
32-byte characters) only on frames the game wrote it. Same pixmaps: race,
time attack, test mode and attract screen hashes unchanged. Decoding 0.55 ->
0.02 ms; hardware 343 -> 409 frames/s, software race 189 -> 204. Next: the
layers drawn on the GPU (0.65 ms drawing + 0.09 composing).

**Vulkan application name (MangoHud showed "SDL").** SDL's Vulkan backend
hard-codes VkApplicationInfo: no application name, engine "SDLGPU", which
overlays such as MangoHud show instead of the API; SDL has no property to
change it. `patches/sdl3/0001-vulkan-application-name.patch` (applied by
setup.py's new apply_patches, shared with MAME's patches; already-applied
patches are skipped; fetch forces the checkout if a patched file would block
a new pin) reports SDL_SetAppMetadata's name ("Daytona USA", set by main)
and no engine name. Checked: setup re-applies it after a revert and skips it
when present; macOS builds and runs. The MangoHud result itself is untested
here (no MangoHud on macOS).

**Windows: the game's messages.** daytona is a WIN32 (GUI) program, so on
Windows its output went nowhere and a command window returned at once; a
tester could not see which renderer ran. It now attaches to the parent
console when there is one, else writes daytona.log in the pref folder, and
prints `daytona: renderer hardware (GPU)` / `software (CPU)` whenever the
active renderer changes. Checked on macOS (both lines); the Windows branch is
compiled by CI only.

**Hardware renderer speed (`m2gpushot --bench`).** race_basic, 6,000 frames,
headless on this Mac (Metal): software 189 frames/s (5.3 ms a frame) at 4:3
and 188 at 16:9; hardware 324 (3.1 ms) and 347. Hardware frame at 4:3: game
2.20 ms (logic, geometrizer, CPU tilemap layers), renderer on the CPU 0.72 ms
(vertices, uploads, a 4 MB texture RAM compare, the colour table), waiting
for the GPU 0.17 ms. From the draw-mode figures (every third frame drawn:
442 frames/s), logic is about 0.75 ms, the CPU tilemap layers about 1.45 ms
and the CPU 3D rasterizer about 3 ms a frame. Next costs, in order: tilemaps
on the GPU; texture RAM tracked by writes instead of compared.
Done: M2Board::tex_write counts writes (VideoMem::tex_generation) and the
renderer uploads texture RAM only when the count moves; the 4 MB compare and
shadow copy are gone. Renderer CPU 0.72 -> 0.54 ms, 324 -> 346 frames/s at
4:3; GPU frames byte-identical to before (8 of 8 sampled).

**Hardware renderer, widescreen and a mip-level fix.** Hardware mode keeps
widescreen: Video's external-3D mode with CPU layers no longer drops the
margin (only the Vita path does); both layers are width() wide, the
backdrop's margins filled as in software mode with the 3D coverage taken
from Raster::coverage_estimate (the polygons on an 8x8-pixel grid; no CPU 3D
layer exists here) and the front layer's HUD moved to the edges; the GPU
projects with the margin, widens full-width windows into it, and moves the
condition panel's quads by Video::gpu_hud_shift (same box and z as the
software path). Fix found while comparing: the rasterizer's max mip level is
30 - countl_zero(min(w, h)) = log2(min) - 1; stage 2 used log2(min), so a
fading circuit-select map (texlod -321, mml 1132) took level 7 not 6 and
came out coloured instead of grey. Measured after (race_basic, Metal,
m2gpushot vs m2run, every 650 frames to 5200): 89.7-100% identical,
94.4-100% within 8 levels; the rest are rounding on high-contrast textures
(road lines, rock), where a one-step texel coordinate difference flips the
blend. 16:9 with HUD at the edges: race frames 94.8-98.3% identical.

**Hardware renderer, stage 2 (textures).** ps_poly is a port of the
rasterizer's draw_tex_span and fetch_bilinear_texel in integer arithmetic:
the 4-bit sheets with their 2048x1024-as-1024x2048 mapping, bilinear 8-bit
blending (LERP), wrap/mirror/edge rules, mip levels by fast_log2 (the same
128-entry table) and texlod, the microtexture, the translucency flag and
test, the luma RAM and the colour translation. Data: three read-only storage
buffers (texture RAM, both sheets, uploaded only when it changes; luma RAM;
colour translation with the rasterizer's gamma applied on the CPU); per
polygon texture state as flat integers decoded as render_one does; 1/z, u/z,
v/z interpolated noperspective. Measured (race_basic, Metal, m2gpushot vs
m2run): frame 1500 99.9% of pixels identical; race frames 3000 and 4500
95.5% and 95.1% identical, 98.4% and 98.8% within 8 levels. The differences
are scattered over textured surfaces (far road, rock face), not edges:
float differences flipping mip-level and texel thresholds. The rasterizer
accumulates 1/z, u/z, v/z per pixel along each span; the GPU evaluates each
pixel's directly, and Metal compiles with fast math. Exactness is stage 4.
build_shaders.py pulls the x86-64 Ubuntu image explicitly (a cached arm64
one failed with "exec format error").

**Hardware renderer, stage 1 (geometry).** Launcher > Game > Renderer:
Software (exact; default) or Hardware (Experimental). `src/app/gpu/`:
`m2.hlsl` (one source) -> `scripts/build_shaders.py` (DXC v1.9.2609 to
SPIR-V and DXIL, SPIRV-Cross to MSL; Docker when the tools are not
installed) -> `shaders_gen.h` (committed; builds need no shader tools).
GpuRenderer draws the 3D in the rasterizer's order (window, then z, newest
first), projected as model2_3d_project, each polygon a fan from vertex 0,
clipped to its window by scissor; a depth buffer with depth = draw order
and LESS reproduces "first polygon to fill a pixel wins" (the rasterizer's
fill buffer). Colour: the solid renderer's palette/luma/gamma; textures are
stage 2 (the Vita GPU path is a reference only: a tester saw small road
geometry/orientation errors there). Video's external-3D mode gained
`cpu_layers` (the CPU still draws the tilemap layers for it; the Vita path
does not). No widescreen in hardware mode yet. `m2gpushot` renders frames
offscreen through it for comparison with m2run's (tools/common/
input_script.h shared). Checked on Metal: the game runs (710 frames in
15 s), and a race frame's geometry, HUD and backdrop line up with the
software renderer's. Not yet run on Vulkan or Direct3D 12.

**Draw mode (frame skip).** Measured first: Daytona runs the board in 60 Hz
mode and the geometrizer starts a new frame every vblank (3,000 of 3,000
race frames drew a new 3D picture), i.e. double buffered. Launcher > Game >
Draw mode: Double buffered (every frame, the default), Single buffered
(every 2nd), Every third frame (every 3rd); `m2run --frame-skip 0|1|2`.
M2Board::vblank_end skips screen_update on the frames between (3D raster,
tilemaps, composition), keeping the last picture; the geometrizer still
parses every frame (the game reads its polygon count). Measured race_basic:
identical i960 (196,665,345), TGP (223,429,779) instruction, interrupt
(12,050) and sound byte (3,636) counts in all three; 190, 332, 442 frames/s
headless on this Mac. Default hash unchanged.

**Skip launcher.** Launcher > Game > "Skip launcher" (saved): start-up goes
straight into the game, as `--autostart` does, when the ROM set checks out;
otherwise the launcher shows with the reason. Esc still opens it. Checked:
with it set, `daytona` started the game at once (377 frames in 8 s).

**"Stretch tile background" stretches, it does not extend.** In a race, with
the option on, the backdrop as drawn for the 496 columns is scaled across
the whole width (linear blend per row); the tester wanted no repeat at all.
Video::draw_ext (drawing the tiles past the screen edge) is removed. Earlier:

**Widescreen sky: plain by default, "Stretch tile background" to extend.**
A tester still saw a seam in the margins with the tile backdrop drawn out.
Measured: the race sky is tilemap layer 2, one layer in normal scroll mode
(not a split pair: the split-mode alternation added to draw_ext changed 0
pixels there), and its hscroll sweeps the whole 0..511 range over a lap
(152 values), so the original 4:3 screen passes the picture's join too;
only ~79% of rows match across it. So the margins default to the sky's
plain colour behind 3D, and launcher > Enhancements > "Stretch tile
background (Experimental)" (`m2run --stretch-backdrop`) draws the tiles out.
2D screens keep each row's edge colours either way. Split pairs in draw_ext
now alternate A/B every 512 columns (a 1024-pixel panorama), as the
hardware's layout implies; not exercised by Daytona's race sky.

**Launcher labels.** Options that need it say so: "Graphics API (Restart
Required)"; "Native audio (Experimental, Reset Required)" (it applies when a
game starts or is reset, not on an app restart); "HUD at the screen edges
(Experimental)". Everything else applies straight away.

**Draw distance (enhancement, off by default).** Launcher slider (Shortest,
Shorter, Default, Further, Furthest = -2..+2), `m2run --draw-distance N`.
Found by tracing, not by guessing: the geometrizer's master z clip is unused
(0xff); object commands in the display list are written by the TGP, which
reads model lists from its own ROM (only 8 of ~3,000 i960 FIFO words per
frame match model addresses). The i960 picks what to draw:
- Scenery by course cell: a 16x16 grid (cell = x + 16 y; offsets table
  0x17136 = dx + 16 dy). 0x16f74..0x17070 takes the 5x5 around the car's cell
  (r8) in nearest-first order (0x17104; 0x1711d on two courses), kept where
  two visibility masks allow (r13 from 0x171f8, r9 from 0x1727c), into the
  draw list (count 0x5016c0, cells 0x5016c1.., 63 bytes before 0x501700) and
  a near list of the 10 nearest (0x501600) plus bitmaps 0x501500/20/40.
  Measured over a race: 8-13 cells listed, always reaching radius 2.
- 0x17828 draws every object of each listed cell (table 0x501420) until a
  per-frame polygon budget runs out: 0x5010e8 accumulates each object's cost,
  0x5010f4 is the limit (5000, set once at 0x1210), checked at 0x17a78 and
  0x1786c. Extra cells appended to the list drew nothing until the budget
  was raised: it, not distance, is what stopped them.
- The road: a 14-section window (0x13f5c: 5 back via +0x8c, 14 forward via
  +0x88, list at +0x5c of its struct; ordered pair checks via table 0x13ec4
  and TGP maths at 0x14180). Also game logic (car/section interaction);
  not changed.
Mechanism: `m2recomp --hooks FILE` ("ADDRESS name": the generated code calls
rt::hook_<name>(c) before that instruction), `seeds/daytona93_hooks.txt`,
`src/runtime/enhance.{h,cpp}`. hook_draw_list at 0x17078: -1 keeps the
game's list within one cell, -2 the car's cell only; +1 lists all 5x5, +2
all 7x7 (inside the grid), and raises the budget to 10000 / 15000 (restored
to 5000 back at default). Only the draw list and the budget change; the near
list and bitmaps the game logic reads do not. Measured (race_basic, m2run on
this Mac): -2 242 frames/s, -1 198, default 193, +1 190, +2 185; default's
screen hash unchanged (ad67233983ea8808). +2 adds visible scenery (frame
2600: 1,560 pixels, a tree line behind the billboard); most frames differ
by tens of pixels, because the road, not the scenery, is the horizon.

**Setup's "ROM set rejected" was shown for any recompile failure.** A
Windows tester got it with a zip that imports on macOS and Linux. The
reader is portable C++ (binary I/O, fixed-width fields, its own inflate
and CRC), so the likelier cause is a later step failing on Windows (the
game's generated code has never been compiled there) under the wrong
message. `recompile.py` now exits 3 only when `m2import` rejects the set;
any other failure gets "ROM accepted, the build failed, see the errors
above". The tester's next run confirmed it: the ROM was accepted and the
generated code compiled (`m2run`, `m2native` built); the final build step
still failed. Likely cause, not yet confirmed on Windows: `daytona` is a
WIN32 (GUI) executable and `main.cpp` did not include `SDL3/SDL_main.h`,
so nothing provided `WinMain`. Now included (no effect on macOS/Linux).
The tester's next output showed the real failure: MSVC building SDL itself,
`yuv_rgb_internal.h` C2099 "initializer is not a constant". Our directory-
wide `/fp:strict` reached SDL, and under it MSVC will not fold C float
constants in static initializers. SDL now gets an empty COMPILE_OPTIONS
(ours keep /fp:strict). CI never built SDL (it was only added with
generated game code); SDL, ImGui and the app objects (`daytona_app`) now
build whenever extern/sdl3 exists, so CI compiles them on every OS; only
linking `daytona` still needs a ROM set. CI then compiled SDL, ImGui and
`daytona_app` under MSVC and clang-cl (including the yuv_rgb file that
failed). The tester's PC had built with MSVC: setup.ps1's Visual Studio
Installer `modify` ran unelevated and, it seems, failed quietly. setup.ps1
now runs it elevated, checks the Clang toolset with vswhere afterwards,
stops with instructions if it is missing, and sets M2_COMPILER=clang
(`--msvc` for MSVC). Untested on a real PC; CI only checks it parses
(GitHub's Windows runners have no winget). README's Setup and
docs/getting-started.md now cover Clang on Windows and `--msvc`, updating,
a clean rebuild, the "build failed" message, and the widescreen options.

**Widescreen (enhancement, off by default).** Launcher > Enhancements >
Widescreen: Original (4:3), 16:10 (614x384), 16:9 (682x384), 21:9
(896x384); `m2run --aspect 16:9` for headless dumps. More of the scene at
the sides, same focal length, nothing stretched: `Geo::set_wide_margin`
opens a full-width viewport's left/right clip planes by the margin,
`Raster` draws into a wider layer (stride >= 496 + 2 x margin) with the
clip widened for full-width viewports, `Video` composes at `width()` with
the tilemaps centred and each back-layer row carried into the margins (the
sky's colour, not the backdrop pen). With it off every path is the old one:
all 11 scenarios give their previous screen hashes. With 16:9 all 11 run to
the end. Sampled 21:9 race frames show no obvious edge pop-in yet; not
checked frame by frame.
"HUD at the screen edges" (with widescreen; `m2run --hud-edges`): two
groups in 496-wide coordinates move out by the margin: lap and lap times
(x < 125, y < 130) left; position, condition panel and course map
(x >= 352, y < 300) right. Decided per item: the front layers' pixels are
grouped into blobs (pixels within 4 of each other join) and a blob moves
only if it lies wholly inside a group, so a banner crossing a group stays
whole and centred. The first version decided per area (a band at the cut
had to be empty): during the rolling start the banner's letters passed the
cut, the decision flipped frame to frame and the HUD jumped back and forth.
Measured after, frames 2585-3200 every 5th: the course map is at the edge
in all but the first (HUD not yet drawn). "40TH/40" reaches x 367. The
condition panel's box and car are polygons in the main 3D window at sort z
0x600 (scenery there is above 18000); overlay polygons (z <= 0x0fff) inside
a group move with it, but only while the race HUD is on screen: a tester
saw the car's door come off in an attract close-up, because the car's own
near polygons (and the ranking screen's 2D markers) sat in the HUD's
right-hand area and were moved. Now nothing moves unless the frame has the
condition panel's box (one checker-shaded overlay polygon, texheader
0x8000, sort z <= 0x0fff, ~77x82 at x 385..462, y 67..149;
Raster::race_hud_visible). Measured after: 240 attract frames at 16:9
identical with the option on and off; the race HUD still moves, and the
map stays at the edge through the rolling start. A tester still saw 3D
moved in play: the rule was any polygon at sort z <= 0x0fff inside the right
group. Now only the condition panel's own quads move: polygons at exactly
the box's z inside the box's outline (Raster::find_race_hud records both).
Measured: race at 16:10, option on vs off, 0 of 120 frames differ outside
the HUD areas.
Side margins: in a 3D scene (the 3D layer covers >= 50% of the screen;
measured races 69-100%, select screens ~23%) the back layers are drawn
margin to margin by Video::draw_ext, draw()'s rules pixel by pixel for any
screen column: scroll, per-line scroll, the split modes that put layers
L and L^1 side by side, priority, window masks. Checked: its visible columns
equal draw()'s on every frame of a race (M2_CHECK_DRAW_EXT=1 prints any
difference; none). Earlier tries, all wrong in play: one plain sky colour;
each row's edge carried out (smeared the clouds); copying columns mod 512
(a tester saw the backdrop duplicated with a seam: a column off the screen
can belong to the other layer of a split pair); a "joins up across the
wrap" test to tell sky from menus (failed on the race sky, median 79% of
rows). On 2D screens (car, circuit select) each row carries its own edge
colours: their art covers only the 496 columns, and drawing further shows
leftover tiles as stripes. Before that the margins were the sky's plain colour (the back
layers' top-left pixel); carrying each row's edge out smeared the sky
picture's clouds and mountains. Off: all scenario hashes unchanged; 21:9
with it on: all scenarios run to the end. rules.md now lets enhancements change game logic.
Measured for draw distance: Daytona leaves the master z clip at 0xff
(0 polygons culled by distance over a 6,000-frame race); backface 4.2M,
behind the camera 1.1M, off-screen 1.4M. The limit is in the game's code.

**Windows: Clang by default; MSVC fixed.** MSVC failed on SoftFloat: the
CMake used SoftFloat's `build/Linux-x86_64-GCC/platform.h` everywhere, whose
`opts-GCC.h` needs `__int128`, `__builtin_clz` and GNU inline (C4235, C4013).
MSVC and clang-cl now get `cmake/softfloat-portable/platform.h` (no
`INLINE_LEVEL`, no builtins, no 128-bit type) and `__declspec(thread)` /
`thread_local` for its globals; `-DM2_SOFTFLOAT_PORTABLE=ON` forces that
header anywhere. Measured here with it forced: `test_fp` 0 mismatches,
race_basic, time_attack and course_expert screen hashes identical to the
GCC-header build. `setup.ps1` installs (or adds to an existing Visual
Studio) the Clang tools; `setup.py` builds with the ClangCL toolset when
present, MSVC otherwise (`M2_COMPILER=clang|msvc` forces one), and
reconfigures a build directory set up for the other.
`.github/workflows/build.yml` builds and tests both, without a ROM set,
alongside macOS and Linux (GCC, Clang).
Not yet run on a Windows PC here; a tester reports Clang + Ninja builds.
First CI run: SoftFloat compiled under both; both then stopped on
`tests/test_vita_gpu_memory.cpp`, `alignas(262144)` (C2345; clang-cl: 8192
bytes at most on Windows). That buffer is now aligned at run time, and the
nine Vita host tests are opt-in (`-DM2_VITA_TESTS=ON`, 21 tests) instead of
part of every desktop build (12 tests).

**PS Vita frontend merged (PR #3, `c3007c6`).** Desktop unchanged by it,
measured: all 11 scripted scenarios and 3,000 frames of attract give the
same instruction counts and screen hashes as `398eb4b`; 21/21 tests pass.
Native audio is opt-in (launcher checkbox). Its notes live in
`platform/vita/` (HANDOFF, THIRD_PARTY for SDL2); the Vita build itself is
untested here (needs VitaSDK).

**Setup for players.** `docs/getting-started.md` walks from nothing to
playing on each OS, with troubleshooting for what went wrong in practice:
`./setup` for `./setup.sh`, the ROM set under another name (setup silently
built tools only), a different Daytona set (`daytona`, program ROMs
`epr-16722a`/`16723a`: `m2import: missing epr-16530a.12`), Start disabled.
`scripts/setup.py` now says so itself: with no `roms/daytona93.*` it names
any archives in `roms/`; a rejected set gets an explanation instead of a
traceback; on success it prints the command that starts the game. README's
Setup links to the guide.

**macOS (Apple clang, arm64, Metal) builds and plays.** First run on a Mac
with a real `daytona93` set turned up:

- `scripts/recompile.py` built only the check tools after generating code,
  never `daytona` or `m2run`, so a fresh `./setup.sh` left no game. It now
  builds everything.
- The launcher saved the ROM path as typed (`roms/daytona93.7z`), so the
  check failed and Start stayed disabled when run from another directory.
  It now stores the absolute path. A saved GPU choice the host lacks
  (Vulkan on a Mac without MoltenVK) exited at `SDL_CreateGPUDevice`; it now
  falls back to automatic.
- The seeds were short. The windowed game stopped at `0x1d8c`, then longer
  runs at `0x2266f8`, `0x5788`, `0x225028`, `0x223078`, `0x2265c4`: code the
  game reaches only through pointers in states the harvest never visited.
  `scripts/seed_scan.py` finds them statically (see Findings) and added 248
  seeds (334 -> 582): 24,504 -> 34,497 reachable instructions, recompile
  still all native.
- Time attack stopped in the geometrizer: texture-point/header reads and a
  polygon-RAM walk run one past the end of their memory. `GeoPtr`/`GeoPtr16`
  now wrap (see Findings).

Measured after, `m2run`: attract 40,000 frames (11.6 min of game time),
all 11 scripted scenarios in `scripts/inputs/` (races, courses, time attack,
test menus) run to the end, all native; race_basic screen hash unchanged
(`ad67233983ea8808`) by the wrap. `daytona` on Metal: 60 s, 3,396 frames,
no stop. **Not verified against MAME**: the newly seeded code has not been
lockstepped (it only runs in states the scenarios reach, and the harvest
did not), and the wrap has no MAME counterpart to compare with.

**Sound.** The Model 1 sound board runs natively: its 68000 program
(`epr-16489`/`16490`) is statically recompiled (`src/m68k` decoder,
`tools/m2sndrecomp`, runtime context `src/runtime/snd_cpu.h`), with the
YM3438 (ymfm, BSD-3) and both MultiPCMs (MAME's, transplanted into
`src/runtime/multipcm.cpp`) as native code on `snd::SoundBoard`
(`src/runtime/sound_board.cpp`). All 1,916 reachable instructions decode
exactly as MAME's 68000 disassembler prints them; the driver has no
indirect jumps. Lockstep against MAME's 68000 (`tools/m2sndcheck`, MAME
patch 0003, `M2TRACE_SNDLOG`): attract 15.7M instructions and race 77.9M
instructions, 3,648 interrupts, 5.16M device accesses, all identical.
No clock: the driver polls YM timer B (868 Hz tick) in its main loop and
takes the UART's RxRDY on IPL 2, so time is counted in 68000 instructions
(752,000 per second, MAME's rate on this program) and events land on it:
command bytes one line-time apart (31.25 kbit/s), YM timer expiries in
exact YM clocks. `GameLoop` advances the board one frame per video frame
and hands it that frame's UART bytes. `daytona` plays it through two SDL
audio streams (YM at 55.6 kHz, MultiPCMs at 44.6 kHz) with a small speed
trim holding 60 ms of queue; volume and mute in the launcher. `m2run --wav
FILE` writes it headless. Against MAME's own audio (`-wavwrite`) for 26 s
of attract: the same 48 command bytes, per-second loudness within a few
percent, a constant ~125 ms offset (when the i960 sends the first
commands), no tempo drift. Race: 3,636 bytes natively vs MAME's 3,648.
Checked here with SDL's disk audio driver (no sound card in the
container): continuous output from the windowed game.

**Launcher.** `daytona` opens a Dear ImGui launcher in its window
(`src/app/launcher.cpp`): ROM browse (SDL3's native file dialog: Windows,
macOS; xdg-desktop-portal or zenity on Linux; typed path as fallback) with
per-file verification, graphics API and fullscreen; a Controls tab binding
every arcade control to a key and a gamepad button or axis half (press to
bind), analogue triggers for the pedals and a stick for steering, live
meters, dead zone, invert. Settings save to `launcher.ini` in the SDL pref
path as they change. The ROM set is loaded natively from the zip
(`src/runtime/zip.cpp`, own inflate; `rom_import.cpp`, the importer's table):
images byte-identical to `scripts/m2import.py`'s, in 0.8 s. 7z ROM sets too (`src/runtime/archive.cpp`: the
7-Zip LZMA SDK's public-domain decoder; solid LZMA2, LZMA, PPMd checked
byte-identical to the zip import, 0.9 s for 7-Zip's default). The build's
importer is now the same C++ code (`tools/m2import`, used by
`scripts/recompile.py`), so a .7z-only user can build. Esc in game
returns to the launcher (Resume, Reset, Quit). Verified here under Xvfb on
Vulkan: verification, the Controls tab, a key rebind saved to the ini, Start
from the zip, pause, and the zenity file dialog. Resolution and upscaling
options are to come.

**The game is playable in a window.** `daytona` (`src/app/main.cpp`): SDL
3.4.16 (built from source, static) with SDL_GPU: Vulkan or Direct3D 12 on
Windows, Vulkan on Linux, Metal on macOS (`--gpu` to choose). Each composed
frame is uploaded to a GPU texture and blitted, letterboxed 4:3, onto the
swapchain; the game advances at the board's 57.52 frames/s and is presented
at the display's rate. Keyboard and gamepad map to the I/O board; settings
EEPROM and backup RAM persist in the SDL pref path. Verified here on Vulkan
(Mesa lavapipe under Xvfb): the attract demo in the window. Direct3D 12 and
Metal not run yet (no Windows or Mac here). The 3D layer is still drawn by
the CPU reference rasterizer; the GPU rasterizer is a later step.
`rt::GameLoop` (`src/runtime/game_loop.cpp`) holds the frame pacing that
`m2run` and `daytona` share.

**The game runs on its own.** `m2run` runs the recompiled game on the
native board runtime (`src/runtime/m2_board.cpp`) with no trace and no
MAME: boot, the settings screen, then the attract demo in full 3D.
Headless for now (frames dumped every N; `scripts/rgb2png.py`); 600 frames
in 5.7 s including the CPU rasterizer (106 frames/s), 23 M i960 and 12 M TGP
instructions, all native.

How it runs, with no clock:
- Frame pacing is the game's: vblank starts when the game waits in its
  wait-for-vblank loops (0x12b0-0x12bb, 0x12f0-0x12ff: spinning on the
  frame counter at 0x00500000); a CPU-bound frame (the boot texture upload
  at 0x1388) gets vblank after one frame's worth of work (110k
  instructions, as MAME measures 25 MHz / 57.52 Hz). Vblank ends when the
  handler has returned to the wait loop. A windowed build waits for vsync
  there; frame rate is the only limit.
- The TGP runs before the i960 reads buffer RAM: the game sends the TGP a
  command, writes -1 to the mailbox at 0x0091fff0 and spins until the TGP
  writes 0 (0x1166c). Without this the game waits forever.
- I/O board: the dual-port RAM mailbox protocol, native (command 1: latch
  inputs into bytes 0-10; 3: load the 128-byte settings EEPROM into
  0x100-0x17f; 2: store it). Inputs take the scripts/inputs format
  (`m2run ... --inputs FILE`).
- Sound UART: TxRDY is immediate, so the IRQ3 handler drains its queue at
  once; the sound board receives the bytes at the line rate (Sound, above).

With scripted inputs (`--inputs scripts/inputs/race_basic.txt`) the whole
game flow runs standalone: coin-up, Circuit Select, car select, the race's
rolling start (3,000 frames in 23 s headless: 80 M i960 and 62 M TGP
instructions, 381 bytes sent to the sound board).

**The whole screen now renders natively, identical to MAME.** `src/runtime/video.cpp`
adds the segaic24 tilemap chip (four 64x64-tile layers, per-line scroll,
special window modes, 8-pixel window masks), the tilemap palette pens (as
MAME computes them at palette write time, refreshed each frame once a scroll
colour is written), the CRTC offsets, and MAME's composition order (2D back,
3D, 2D front). It runs at the i960 instruction count of each MAME screen
update (vblank end; patch 0002 `su`/`scr` lines) and is held to a hash of
MAME's composed screen: **596 of 596 attract frames and 5,996 of 5,996 race
frames identical**, 3D layer 5,587 of 5,587. Dumped frames (boot settings
screen, attract with HUD) are byte-identical to MAME's.

Everything on screen now comes from native code: the recompiled i960 and
TGP programs, and native C++ for the fixed-function chips (geometrizer,
rasterizer, tilemaps). Still replayed from MAME's trace in the harness: the
sound board (UART bytes) and the I/O board (dual-port RAM). Design change:
the sound 68000 will be statically recompiled, not interpreted.

**M3 started: the 3D layer renders natively, pixel-identical to MAME.**
`src/runtime/raster.cpp` is a CPU reference rasterizer (MAME's Model 2
renderer and the poly.h triangle/polygon setup, transplanted). In
`m2native`, at each vblank it draws the previous display list from our own
video memories (palette, colour translation, luma, texture RAM, all written
by the recompiled i960) and is held to a hash of MAME's 3D layer (patch 0002
`fb` lines): **423 of 423 rendered attract frames and 5,587 of 5,587 race
frames identical**, and a dumped frame is byte-identical to MAME's. A
mutant (one gamma entry off by one) fails every frame. Frame dumps:
`M2NATIVE_FBDUMP_DIR`/`M2NATIVE_FBDUMP_EVERY` (ours),
`M2TRACE_FBDUMP_DIR`/`M2TRACE_FBDUMP_EVERY` (MAME); `scripts/rgb2png.py`
converts either (game output: keep under traces/). Finding on the way: the
original Model 2's texture RAM keeps only 16 bits of each 32-bit write,
packing two writes per stored dword (MAME tex0_w/tex1_w); the bus now does
the same. CRTC offsets are still taken from MAME's log (they come from the
segaic24 tilemap chip, next).

**M2 geometry, native and matching MAME.** The whole geometry path now runs
natively inside `m2native`: recompiled i960, recompiled TGP, buffer RAM, and
the geometrizer (`src/runtime/geo.cpp`, MAME's HLE transplanted; the
original Model 2 geometrizer's DSP code is undumped). At each MAME vblank
(same i960 instruction count) the geometrizer walks our buffer RAM and is
held to MAME's log (patch 0002, `M2TRACE_GEOLOG`): every word it hands the
rasterizer and every polygon kept after culling and clipping, vertices bit
for bit.

| scenario | frames | rasterizer words | polygons kept | result |
| --- | --- | --- | --- | --- |
| attract | 597 | 11,954,823 | 403,475 | identical |
| race_steer_left | 5,997 | 151,874,210 | 6,658,999 | identical |
| time_attack | 5,997 | 136,744,927 | 5,937,107 | identical |

A mutant (luma off by one when it is exactly 100, in all four parsers)
diverges at frame 173, rasterizer word 385. A one-ulp change to a vertex
cannot show: the geometrizer hands the rasterizer 24-bit floats (MAME's
`f2u(x) >> 8`), which drop the low 8 bits. `scripts/m2_check.sh SCENARIO`
runs all of it (the race's geometrizer log is 2.8 GB of text; it is deleted
after the check unless M2_CHECK_KEEP is set).

**M2 started: the TGP program is statically recompiled and matches MAME.**
The TGP runs a 2,024-word program the i960 uploads from the data ROM;
`m2tgprecomp` turns it into native C++ (MAME's MB86233 semantics inlined per
instruction from `src/runtime/tgp.h`; no interpreter, no hand-written HLE),
and `m2tgpcheck` replays MAME's TGP-side log (patch 0002) through it:

| scenario | TGP instructions | input words | output words | banked accesses |
| --- | --- | --- | --- | --- |
| attract, 600 frames | 12,390,181 | 1,293,703 | 398,072 | 7,789 |
| race_steer_left | 230,600,970 | 21,489,825 | 6,727,552 | 3,233,496 |
| time_attack | 97,745,484 | 8,089,105 | 3,291,602 | 2,025,772 |
| test_tgp | 32,597,984 | 3,300,128 | 1,083,012 | 7,789 |

All identical to MAME; in attract every register was also checked after
every instruction (`M2TRACE_TGPPC`). A mutant (fml result off by one ulp when
A = 1.0) diverges at TGP instruction 2,339. Native speed ~500 M TGP
instructions/s (race: 0.48 s for 231 M).

**Native i960 + native TGP together** (`m2native` now models the geometry
ports, TGP FIFOs and buffer RAM instead of replaying them; the TGP runs on
demand, clockless, until its input FIFO is empty):

| scenario | i960 instructions | TGP instructions | TGP output words | buffer RAM hash |
| --- | --- | --- | --- | --- |
| attract | 70,926,456 | 12,390,181 | 398,072 identical | identical at 1,153 of 1,153 samples |
| race_steer_left | 643,000,979 | 230,600,970 | 6,727,552 identical | identical at 11,951 of 11,951 |
| time_attack | 663,975,129 | 97,745,385 | 3,291,602 identical | identical at 11,951 of 11,951 |

Buffer RAM (128 KB) is built natively from its three writers: the i960, the
geometrizer command port (0x800000) and the TGP's banked writes. The only
reads that differ from MAME (103 in attract, 64,956 of 5,451,850 in the race)
are all the i960 polling the TGP's mailbox, the last three dwords of buffer
RAM (0x91fff0-0x91fff8, TGP bank offsets 0x7ffc-0x7ffe): our TGP has already
finished when the i960 looks; MAME's, paced by cycle estimates, has not. A
timing artefact like the UART one, so lockstep uses MAME's value there and
counts it. Finding on the way: races read and write the TGP FIFOs 16 bits at
a time (MAME's 32-bit handlers see the whole dword, other lanes zero).

**M1 met with native code**: `m2recomp` statically recompiles 23,262
instructions (seeds: boot record + `seeds/daytona93.txt`) to portable C++,
and `m2native` runs them with no interpreter and no fallback. It matches
MAME for all 600 attract frames: 70,926,456 instructions, every one native,
1,153 samples, 3,315,201 device events, 627 interrupts; 1.3 s (55 M
instructions/s with lockstep checks on every instruction).
`scripts/recompile.sh` generates and builds it (into git-ignored
build/gen); `scripts/m2_check.sh` traces MAME and runs both harnesses.
An address with no recompiled code is a hard error naming it.

Beyond attract, the same native code matches MAME through seven scripted
scenarios (coin up, selects, races, test mode), every instruction, device
access and interrupt, including the sound-UART interrupts that land mid-code
during races:

| scenario | frames | instructions | device events | interrupts |
| --- | --- | --- | --- | --- |
| race_steer_left | 6,000 | 643,000,979 | 39,750,589 | 11,443 |
| course_advanced | 6,000 | 640,678,236 | 42,356,938 | 12,475 |
| course_expert | 6,000 | 641,018,396 | 42,010,671 | 12,419 |
| time_attack | 6,000 | 663,975,129 | 20,342,792 | 11,506 |
| test_mode | 4,000 | 439,587,066 | 6,954,614 | 4,051 |
| test_tgp | 3,500 | 385,086,539 | 6,708,822 | 3,578 |
| test_memory | 3,500 | 385,423,629 | 6,748,842 | 3,553 |

`scripts/m2_check.sh SCENARIO` reruns one (inputs from
`scripts/inputs/SCENARIO.txt`); each takes ~7 min of MAME plus ~15 s native.

The reference interpreter (`src/refcore`, MAME's executor) is a test
oracle only: `m2replay` uses it; the game build will never link it.

Earlier, **M1 reference milestone**: `m2replay` runs Daytona's own code through
the runtime (MAME's i960 semantics, transplanted) with devices replayed from a
MAME trace, and matches MAME for all 600 attract frames: 70,926,456
instructions, 1,153 samples (RAM hashes + registers), 3,315,201 device
events, 627 interrupts; 2.0 s.

M0 tooling runs against real MAME with the real game (`daytona93`, user's ROM
set, git-ignored `roms/` in this cloud container, never committed). Model-2-only
MAME with the harvest patch builds here (`scripts/build_mame.sh`, ~50 min cold,
15 s incremental); `scripts/run_trace.sh` runs it headless.
Built and tested here: i960 decoder + `i960dis`, trace library + `tracediff`,
and the MAME plugin `tools/mame-plugins/m2trace` (trace recorder, input
recorder/replayer). The plugin is tested only against a mock of MAME's Lua API.

Build: `./setup.sh` (Linux, macOS) or `setup.ps1` (Windows) installs the
toolchain, fetches the pinned dependencies, builds, recompiles the game if
`roms/daytona93.zip` is there, and runs the tests (see README.md). Verified
end to end on Linux with GCC and Clang; macOS and Windows (MSVC) not yet run.
The Lua tests need `pip install lupa` (Lua 5.4) and skip without it.

Running the plugin (user's machine, with their ROM set):

    M2TRACE_OUT=traces/attract.m2tr M2TRACE_FRAMES=600 \
    mame daytona -plugin m2trace -pluginspath "<mame>/plugins;<repo>/tools/mame-plugins"

`M2TRACE_RECORD_INPUT` / `M2TRACE_REPLAY_INPUT` record and replay inputs;
`docs/trace-format.md` has the rest.

## Complete

- Design document.
- Project rules (`rules.md`) and `.gitignore`.
- M0 step 1: "from memory" figures checked against MAME `sega/model2.cpp` at
  `dddd73680656e355bb2b5beecab1167c9f07bf81` and the Model 2 MiSTer core at
  `591e148e87d27e03d50cbf7318bf0b1d1328c4bf`. Design doc corrected in place
  (Target hardware summary, TGP HLE, Floating point, Audio, Open questions).
- M0 i960 disassembler: `src/i960` (decoder + MAME-syntax formatter),
  `tools/i960dis` (linear sweep; `--interleave` joins the ROM_LOAD32_WORD pair),
  `tests/test_decode` (66 hand-encoded checks), `tests/mame_oracle`
  (differential against MAME's own `i960dis.cpp`, compiled unmodified).
- Ghidra SLEIGH cross-check (`tests/ghidra_oracle.py`, third-party module
  mumbel/ghidra_i960 via pypcode, `scripts/fetch_ghidra_i960.sh`).
- `src/i960/reach` (recursive descent + boot-record seeds), `i960dis --follow`,
  `tests/test_reach` (19 checks).
- M0 trace format, `tracediff`, MAME plugin and input recorder
  (`docs/trace-format.md`, `src/trace`, `tools/tracediff`,
  `tools/mame-plugins/m2trace`), with `tests/test_trace` (30 checks),
  `tests/lua_core_test.py` (19) and `tests/lua_plugin_mock_test.py` (17).

## Next, in order

1. Scenarios that reach the other 167 seed_scan seeds (other endings, name
   entry, link play), checked with scripts/m2_check.sh like attract_long and
   race_to_end.
2. Hardware renderer: exact pixels against the CPU reference (stage 4).

## Open decisions

- FP oracle for lockstep: MAME disagrees with the hardware model on cvtri
  ties (see Findings). When a replay diverges there, the trace diff will show
  MAME's value; the recompiled build follows the model (rules: PCB > MAME).
  Settling which the PCB does needs a hardware measurement.
- `addc` carry. MAME never sets it. Recompile to the silicon and flag the diff
  when it fires (the MiSTer core made the same call, its study §2.3).

## Findings

Confirmed from MAME `model2.cpp` (all MAME figures, not PCB measurements):

| Item | Was (from memory) | MAME |
| --- | --- | --- |
| i960KB clock | ~25 MHz | 25 MHz (`50_MHz_XTAL / 2`) |
| TGP | MB86234 + microcode ROM | one MB86234 at 50 MHz; program uploaded by the i960, 2,024 words from the data ROM; run LLE |
| Resolution | ~496x384 | 496x384 active, 656x424 total, 16 MHz pixel clock |
| Refresh | ~57.5 Hz | 57.524 Hz; line rate 24.39 kHz (MAME: "TODO: from System 24") |
| Sound | 68000 + 2x SCSP, ~11 MHz | **Model 1 sound board**: 68000 @ 10 MHz + YM3438 + 2x MultiPCM |
| Main to sound | command latch | i8251 UART at 31.25 kbit/s; IRQ3 handler is the transmit loop |
| I/O | direct ADCs | Model 1 I/O board, own Z80 @ 4 MHz, via MB8421 dual-port RAM |
| IRQ order | unknown | bit 0 vblank -> IRQ0, bits 2-5 timers -> IRQ2, bit 10 UART -> IRQ3; ICR 0f0e0d0c (measured): all priority 1 |

i960 decoder (MAME `i960.cpp` / `i960dis.cpp` at `dddd7368`):

- MAME's executor implements 62 non-REG and 102 REG opcodes; its disassembler
  knows many more (whole-family table). The recompiler accepts only the
  executor's set.
- Executor and disassembler disagree on three encodings; decoder follows the
  executor and flags them: CTRL/COBR bits 1:0 (executor adds them to the
  target), MEMB bits 6:5 (executor ignores), MEMB scale > 4 (executor shifts).
- MAME's disassembler table has `ldtime` at 0x671, shadowed by `ediv`; it can
  never print. `movre` appears at 0x6e1 (undocumented, executed) and 0x6e9.
- Differential test, random + every opcode byte x 65,536 tails: 71,108,864
  words, 0 mismatches, 8.0 s on 4 threads (this container). Mutation check:
  three deliberate faults (MEMB scale bound, COBR displacement mask, one REG
  mnemonic) gave 79,619 / 2,220,826 / 9,837 mismatches, so the test can fail.
- Exhaustive run, `mame_oracle --exhaustive`: all 4,294,967,296 first words
  (second word random per word), 0 mismatches, 596 s on 4 threads (this
  container). Text and reported length both compared.

Trace tooling:

- Every signal the design doc asks for except indirect branch targets is
  reachable from MAME's Lua API alone (write/read taps, `read_range`,
  `state[]`), so the plugin needs no MAME patch yet.
- MAME's end-of-frame notifier is not on a guest instruction boundary, so it
  cannot be a lockstep sample point; the default is the vblank-ack store.
  Unverified for Daytona until the first trace.
- Mock-driven plugin test: record -> replay reproduces the trace exactly; a
  replay value applied one frame late is caught at the right frame; a memory
  change is reported as a region hash at the right epoch.
- The Lua encoder and the C++ writer produce byte-identical traces for the
  same content; the Lua hash equals an independent Python FNV over 8 sizes
  either side of the 1 KiB unpack block.
- Hashing reads 1.4 MiB per sample in Lua; cost unmeasured until a real run.
  If it is too slow, hash fewer regions per sample, not a weaker hash.

First real MAME runs (`daytona93`, MAME `dddd7368` + harvest patch, 600
frames of attract, headless, empty NVRAM each run):

- Plugin loads and every tap fires: 3,567,333 events in 600 frames. 68 s per
  run with tracing on this container (MAME reports 14.9% speed).
- Found and fixed: `screen.frame_number` is a method at this MAME, not the
  property the Lua reference documents; and MAME silently drops errors raised
  in tap callbacks, so the first run had 0 samples and no message. Samples are
  now pcall-wrapped and failures reported.
- **MAME is deterministic for Daytona**: two independent runs gave
  byte-identical traces (60,942,182 bytes, 1,154 epochs, 3,567,333 events)
  and identical branch harvests.
- vblank-ack sampling works, but the handler writes the ack (`fffffffe`) twice
  back to back: steady state is exactly 2 samples per frame, the second epoch
  holding only the second ack. Deterministic, so lockstep is fine; every other
  sample is redundant (cost, not correctness).
- ICR = `0f0e0d0c`, set once (synmov at 0x00000a40): IRQ0-3 -> vectors
  0x0c-0x0f, **all priority 1**. Taken in attract: vector 0x0c (vblank) 576x,
  handler 0x0e00; vector 0x0f (sound UART) 51x, handler 0x0f50. Timers
  (IRQ2) never fire; final enable mask = vblank only.
- Geometrizer program port (0x00804000) **is** written: 411,757 writes, from
  epoch 89. TGP FIFO: 773,279 writes, 697,078 reads.
- Harvest: 16 `bx` sites / 49 targets, 2 `callx` sites / 56 targets; no
  `balx`, no `calls`.
- Static reach with the 107 harvested targets as seeds: 89 -> 13,081
  instructions, 0 stops on non-executable opcodes, **0 quirk encodings in
  reachable code**, 31 indirect sites (18 exercised by attract).

Scripted gameplay in real MAME (`scripts/inputs/race_basic.txt`: 3 coins,
start, confirm selects, hold accelerator; 6,000 frames, no steering):

- Replay self-check passes in real MAME: "replay matched the recording for
  5997 frames", in two independent runs; the two branch harvests are identical.
- First attempt stayed in attract: default settings take 3 coins per credit
  (screen showed CREDIT 1/3). With 3 coins, snapshots show car select, then
  the Beginner course, lap 2 of 8 by frame 6,000.
- Full run 103 s emulated at ~14.5% speed, with or without tracing (MAME's
  software 3D dominates; the Lua taps cost ~nothing).
- Race harvest: 20 bx sites / 53 targets, 6 callx sites / 113 targets.
  Interrupts over 6,000 frames: vblank 5,975, sound UART 4,820, timers never.
- Attract + race seeds (168): static reach 21,850 instructions (attract alone
  13,081), 26 of 35 static indirect sites exercised, still 0 non-executable
  opcodes and 0 quirk encodings reached.

Harvest over 15 scripted runs (attract, races, manual gearbox, time attack,
test mode; all replays self-checked "matched"):

| runs | merged seeds | static reach | indirect sites hit / found |
| attract | 107 | 13,081 | 18 / 31 |
| + race | 168 | 21,850 | 26 / 35 |
| + manual, time attack, test mode | 319 | 23,138 | 46 / 67 |
| + round 3 | 333 | 23,258 | 47 / 68 |

- Still 0 non-executable opcodes and 0 quirk encodings in reachable code.
- FP in reachable code: 108 instructions, only `cvtri` 39, `cmpr` 37, `cvtir`
  22, `scaler` 8, `cvtzri` 2. No FP arithmetic, transcendentals or extended
  forms: the FP oracle question shrinks to five operations.
- Reached and confirmed by snapshot: Beginner race (auto and manual with
  shifting), time attack (start + accelerator at car select), test mode menu
  and sound test.
- Not reached: Advanced/Expert courses (circuit select stays on Beginner with
  steering pulses held 30 frames at 0xe0 from frame 1560 and from 1450), TGP
  and memory test items (7 red presses from frame 1500 land on SOUND TEST
  twice, deterministically). Cause unknown; not guessed further.
- Circuit and car select confirm on an accelerator press ("step to choose");
  holding the accelerator from the start picks the defaults.

FP, step 1 (`src/i960/fp`, `tests/test_fp`, `tests/fp_vs_mame`):

- Operand forms measured: all 108 reachable FP instructions use g/l registers
  (single precision in and out); none uses fp0-fp3 or FP literals. AC =
  `3f001000` in all 1,153 attract samples: round to nearest, exceptions masked.
- Reference = SoftFloat 3e extF80; native fast paths = host float, round to
  nearest. Exhaustive proof: cvtri, cvtzri, cvtir over every 2^32 input, scaler
  over every 2^32 single at n = 0, 1, -1, 127, 128, -126, -127, -149, -150, 254,
  -300; 0 mismatches, ~5 min on 4 cores. Plus 36 hand-computed IEEE values,
  specials cross-product, 2^26 random each. A MAME-style fast cvtri
  (`std::round`) fails with 130,905 mismatches (quick run), so the proof bites.
- Bug found on the way: SoftFloat's `extFloat80_t` field order depends on
  `LITTLEENDIAN`, defined only in its private platform.h; C++ callers saw the
  other order and every result was wrong. Now a public definition.
- MAME vs model, every input (`fp_vs_mame`, ~15 min on 4 cores):
  | op | inputs | disagree (MAME on x86-64) | cause |
  | cvtri | 4,294,967,296 | 8,388,608 | every exact .5 tie: MAME `round()` away from zero, IEEE to even |
  | cvtzri | 4,294,967,296 | 0 | |
  | cvtir | 4,294,967,296 | 0 | |
  | cmpr | 268,435,456 pairs | 0 | |
  | scaler | 77,309,411,328 (18 exponents) | 14 | 0 x 2^n (n >= 1024), inf x 2^n (n <= -1075): MAME pow() gives NaN |
  MAME built for ARM64 also differs on 830,472,191 cvtzri and 830,472,191 +
  8,388,608 cvtri inputs (NaN, out of range): its C casts are UB and
  saturate, so MAME's own result is host-dependent there.
- Practical risk for lockstep: a Daytona cvtri on an exact .5 value. The
  trace diff will show it as a register/RAM divergence right after a cvtri.
- MAME never sets FP exception flags in AC; the model reports them. Where the
  i960 records them and whether Daytona reads them is unconfirmed.

M1 groundwork:

- UART-interrupt lockstep: option (a) chosen (safe points in the shipped
  build; a test harness replays MAME's delivery points).
- Delivery points are keyed by MAME's completed-instruction count (patch):
  a stalled FIFO op counts once. Attract, 600 frames: 70,926,456
  instructions, 1,882 interrupt events (1,254 line changes, 622 immediate
  takes, 5 pending-table takes). Two runs: identical IRQ logs and traces.
- Stalled accesses: MAME's `i960_stall()` rewinds IP to PIP, so the plugin
  marks an access with ip == pip as stalled (new record types 0x12/0x13);
  comparisons drop them by default.
- Read taps now cover every non-RAM range the i960 reads (irq, timers, geo,
  copro status to 0x3f, comm, renderer), so the harness can answer them all.
- `scripts/m2import.py` builds program.bin and main_data.bin from the user's
  zip (CRC-checked, MAME's layout) into git-ignored build/rom_cache.

M1 reference core (`src/runtime`, `tools/m2replay`, test only):

- Semantics: MAME's `i960.cpp` transplanted (BSD-3, notice kept); runs at
  36 M instructions/s interpreted.
- Bugs found on the way to MATCH, in order: (1) the bus treated
  read-only-tapped ranges as write-checked; (2) **MAME's ldl/ldt/ldq and
  stores advance the address only on regions flagged BURST** (RAM/ROM, geo
  program port, TGP function port, comm); elsewhere they repeat the address
  (FIFO pops). The bus now carries MAME's BURST flags per region; (3) **the
  plugin's region hash was wrong**: `read_range(first, last, 32)` steps one
  *byte* at a time, so it hashed a dword at every byte address. Fixed with
  step 4; MAME-vs-MAME comparisons had still passed because it was
  deterministic; (4) buffer RAM is written by the geometrizer data port and
  by the TGP (`copro_tgp_memory_w`), not only the i960, so in M1 it is
  treated as a device: the i960's accesses are recorded and replayed, and
  its hash is left to M2; (5) the plugin's own `read_range` fired the
  buffer-RAM read taps while hashing; taps are now suppressed while sampling.
- A per-instruction log on both sides (`M2TRACE_PCLOG`, `M2REPLAY_PCLOG`:
  count, PIP, AC, register-file hash) located bug (2) at instruction 109.
- Mutation check: addo off by one when src1 == 1 (fired 129 times) diverges
  at epoch 3; one program-ROM byte flipped (copied to RAM at boot) diverges at
  epoch 0. Three earlier mutants never fired and so proved nothing either way
  (operand 12345, base 0x00500000, a data-ROM byte attract never reads).

Real program image (`daytona93`, epr-16530a/16531a, counts only):

- Linear sweep of the 256 KiB image: 55,098 lines, 16,280 undecodable words,
  7,356 flagged quirks, 759 non-executable opcodes. Mostly data decoded as
  code; not meaningful as code statistics.
- Recursive descent from the boot record (`i960dis --follow`, with the
  0x00220000 mirror): reset IP 0x860, 0 interrupt handlers (the PRCB's table
  is in RAM), 4 system procedures; 89 reachable instructions, 1 indirect site,
  0 quirks. The reset path ends in `b .` idle loops. Static analysis cannot
  get past boot without harvested targets.
- Mainline Ghidra has never shipped an i960 module (checked HEAD `8e9a8e7a`,
  the last 40 release tags, and full history). The user pointed to the
  third-party mumbel/ghidra_i960 (Apache-2.0), now used.

Ghidra SLEIGH cross-check (mumbel/ghidra_i960 `727ef787` via pypcode 3.3.3):

- All 23,258 reachable Daytona instructions: 0 differences (validity,
  mnemonic, length, target, operands).
- 1,000,000 random words, 1.5 s per 100k: after normalising syntax (MAME omits
  a x1 index scale, prints negative displacements unsigned, prints mode-5 as an
  absolute address; Ghidra prints `disp (ip)`), 72 differences in 5 groups,
  all known: MAME's disassembler names the integer src1 of cvtir/cvtilr/
  scaler/scalerl as an FP register (executor `get_1_ri` and SLEIGH read an
  integer); and SLEIGH decodes `movre` only at 0x6e1, MAME also at 0x6e9.
- The cross-check found four more encoding classes MAME treats specially;
  now decoder quirks: `sfr` (s1/s2, COBR bit 0: Cx special-function
  registers, ignored by MAME), `literaldst` (literal destination: MAME
  fatalerror, so no longer executable), `fpliteral` (FP literal other than
  fp0-3/+0.0/+1.0: MAME reads 0.0), `testfields` (test* with non-zero unused
  fields). 0 of any quirk in reachable code.
- Mutation check: making MEMB mode 7 read a displacement gives 2,871
  differences, exit 1.

Also found:

- A separate geometrizer (0x00800000 / 0x00804000) walks the display list in
  buffer RAM at vblank. It is not the TGP. MAME's is HLE in host `float`.
- The SCSP figures in the MiSTer core's design study are for 2A-CRX in general;
  its README confirms Daytona's working sound is 68000 + FM + MultiPCM. Both
  sources agree with MAME.
- MAME `subc` carry was fixed upstream since MAME 0.289; `addc` was not
  (operands still `uint32_t` before widening, `i960.cpp` ~line 1359).
- The design doc's "15 kHz capture" was wrong for this timing; changed to
  24 kHz medium resolution, pending PCB confirmation.
- macOS (Apple clang, arm64) failed to link `test_fp`/`fp_vs_mame`: C++ sees
  SoftFloat's globals as `extern thread_local` and calls a TLS wrapper
  function that the C (`_Thread_local`) definitions never emit. Linux accepts
  it, which is why it went unnoticed. C++ now gets `__thread` (GCC/Clang) and
  MSVC keeps `thread_local`. Checked: macOS `./setup.sh` builds with 5/5 tests
  passing; Ubuntu 24.04 arm64 GCC 13 and Clang 18 build `test_fp` with 0
  mismatches. Windows is untested, but MSVC's definition did not change.
- The harvest's seeds covered only the states the scripted MAME runs
  visited. The game reaches code four other ways, all now scanned by
  `scripts/seed_scan.py` to a fixed point (recompiling each round):
  - game-mode table at `0x18cc`, 31 entries, `ld 0x18cc[g0*4]; callx`
    at `0x18bc`, index the mode byte at `0x5010a0` (harvest reached 9);
  - task state chains: each state stores the next state's address with
    `lda` (`0x1dd4`: `lda 0x2266f8,r5; st r5,0xc(r3)`; `0x2266f8` itself
    stores `0x226764` at `0xc(g13)`);
  - jump tables read as `ld T[r*4]` then `bx` (`0x5808`, masked to 4) or
    `lda T[r*4]` then `ld`, `callx` (`0x225010`);
  - handler addresses in ROM data: 24-byte object records in the program
    ROM (`0x2375bc`: `0x223078` then floats) and tables in the main data
    ROM (`0x28660cc` -> `0x2265c4`, a bare `ret`).
  A candidate counts only if `i960dis` decodes it to a `ret` or branch with
  no `?`, `!noexec` or `!quirk` first. That rejected float constants that
  `lda` loads (`0x50d0` = 1.0f, `0xcdd8`). From the committed seeds the scan
  finds 275 entry points in one round and nothing in the next; 248 of them
  are the added seeds, the other 27 fall inside code those reach.
- Geometrizer reads past the end of memory (time attack, frame < 6,000):
  texture point/header data (`GeoPtr16`, 4-8 words from a masked start) and
  a polygon RAM walk (`GeoPtr`, index 32,768 of 32,768). MAME's raw
  pointers read whatever follows the array there. The display list is not
  ours at fault: time attack matched MAME to the word, buffer RAM included.
  Now wrapped to the memory size, as a hardware address counter would;
  unconfirmed on the PCB.

## What not to re-propose

- The SHARC as compact code (shared handlers, step pieces out of line) for
  the Vita's instruction cache: 26 % slower on the console than the
  specialised code (3.22 against 2.37 M instructions/s, same session).
- Applying SHARC flag liveness: ~1.1 dead flag writes an executed instruction.
- SHARC registers as locals in plain runs: GCC already forwards them (18 of
  3,899 loads re-read a stored value in the hot chunk); a few per cent.
- Fastmem (MMU mirroring, SIGSEGV for faults) on the Vita: no user-mode
  fault handlers, no free placement of pages; the SHARC's range test is one
  compare.
- NEON for the emulated geometry (TGP/SHARC programs): per game, and the
  A9's NEON flushes denormals (not bit-identical to MAME for the i960).
- Bigger SHARC chunks (1024, 2048 instructions) to cut chunk calls: slower
  on the PC fight check (~10 %), minutes more to compile for the Vita.
- -marm for the Vita's SHARC code: dropped by the user (Thumb-2 kept; ARM
  code ~25 % bigger, gains on the Cortex-A9 usually 0-5 %).
- An approximate SHARC/TGP (fast-math, skipped flags) "because it is only
  visual": their output FIFO words feed the i960's game logic, and the cost
  is not in the floating-point maths.
- Shifting the SHARC's ASTAT history only in the three instructions before
  a conditional loop end, full steps included: exact only if no jump lands in
  that window; full steps keep shifting it.

- Fewer i960 blocks for VF2 on the Vita (seeds not block starts, or fewer
  seeds): no measurable gain in a fight; the TGP and rendering fill core 0
  (15 Oct 2026).
- Cutting i960 bus calls on the Vita (the inlined bus, gen::BoardMem: RAM,
  ROM, TGP FIFO): no measurable gain without profilers (15 Oct 2026).
- Measuring bus changes with `--i960-profile`: prof_bus() makes every bus
  call dearer, so fewer calls look faster.
- Comparing Vita builds on race windows from separate sessions: ~10 %
  session noise. Use the attract from boot, window by window, same commit.
- SCSP for Daytona. It is the Model 1 sound board (MAME `model2o` config and
  the MiSTer core's working sound on hardware).
- Five TGPs. One device; "5x" was a board-level package count.
- MAME's frame notifier as the lockstep sample point (see Trace tooling).
- `read_range(a, b, 32)` without a step of 4 (reads every byte address).
- A mutation test whose mutant is not shown to fire.
- Using MAME's disassembler as the decode authority. It is the text oracle
  only; semantics come from the executor.
- `THREAD_LOCAL=thread_local` for C++ users of SoftFloat (breaks the macOS
  link; see Findings).
- Seeding one "no recompiled code at X" at a time. Each state stores the
  next, so the game stops again one state on; run `scripts/seed_scan.py`.
- A function-start test (previous word is `ret` or `b`) on its own for code
  pointers: it rejected `0x2266f8`, whose previous word is the second word
  of an 8-byte instruction. `seed_scan.py` uses it only for the data ROM.
- Every aligned word of the 32 MB data ROM as a candidate without that test:
  5,497 candidates, mostly chance matches in graphics data.
- A TGP microcode ROM dump. The program is uploaded at boot from the game's
  data ROM; dump TGP program RAM only after the upload, or it is zeros.
- Looking for srallyc's TGP program in its data ROM (no "Interrupt Halt"
  tag, no copy there): it is in the program ROM at 0x5ae94.
- Reading the srallyc upload as starting at ROM 0x5ae98 because the vector
  table seemed to line up there: a misread hex dump; MAME's program RAM
  equals ROM 0x5ae94.

M1 native (`tools/m2recomp`, `tools/m2native`, `src/runtime/lockstep`):

- One label per instruction; operands, branch targets and FP fast paths
  resolved at recompile time; `goto` for direct transfers, a dispatch switch
  for indirect ones. Unknown instructions or FP operand forms stop the
  recompile (exit 1), so nothing is left to run at runtime.
- Lockstep (`src/runtime/lockstep`) applies MAME's interrupt lines and takes
  at the same completed-instruction counts; shared by both harnesses.
- **IAC 0x93 (reinitialise) is an indirect transfer**: boot reinitialises to
  0x924 through `synmovq`. The first native run stopped there (no code); the
  harvest patch now logs IAC targets and the seeds include it.
- Mutation check: the generator's addo template off by one when src1 == 1
  diverges at epoch 3.
