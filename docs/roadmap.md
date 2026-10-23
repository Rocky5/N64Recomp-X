# Roadmap: from seven games to the top 100 (2026-10-22)

Written for the next agent, from `dev` at `ba03cd1`, after reading every doc,
the runtime, the tool and the app, running the host tests on Linux, a
codegen experiment on the generated code's contract, and a review of the
code by file (`docs/review-2026-10.md`). `docs/PLAN.md` stays binding on the
approach and the architecture; this file says what to do next and in what
order, and why. Where it departs from PLAN.md's milestone order it says so.

## 0. Release blocker (maintainer, 2026-10-23): the first build is the best build

**A user opens the app, gives it a ROM, and gets the best build the project
can make: no learning rounds, no console sessions, no flags. Nothing ships
until that holds.** Set 31 on red showed the gap: GoldenEye built from the
ROM alone (the dry boot and the shipped learnt facts) interprets 123
hand-written routines and plays at 12-34 fps; the maintainer's Silver build
(`vge13`, 31 rounds of console learn logs) interprets 34. The logs' code
bytes (`learn_code.bin`: the game's own code as it sat in RDRAM) can never
be committed (CLAUDE.md's first hard rule); the facts already are
(`games/learned/`), and they are not enough.

What closes it, in order:

1. **The 123 vs 34.** Diff `build-hw19-v31-ge/ngee/pack/interp.txt` (and
   `hazards.txt`, `analysis.txt`) against the learn-log build's. Each
   routine is interpreted either because its bytes are missing (code no
   ROM DMA holds as it runs) or because a rule refuses it (cross branches,
   tables outside a segment, hand-written code). The second kind is an
   analyser fix that helps every game; measure how much of the gap it is.
2. **Recipes, not bytes.** Most captured code is the ROM's own bytes
   unpacked by the game. Commit how to make them from the user's ROM:
   - *unpacking facts*: "ROM offset X, Rare's inflate (or the game's
     gzip/LZ), lands at vram Y" (games/learned gains such lines; the
     analyser inflates at build time, as it already does for Yaz0);
   - *dry-boot routes*: an input script per game (games/learned or a
     sibling file: button presses and frame counts) that drives the dry
     boot past the boot into its levels (mission 1, the first world, a
     match), so the game itself unpacks that code on the user's PC
     (`N64RX_DRYBOOT_SCRIPT` exists; it took Mario Tennis into a match);
   - *checksums*: the SHA-1 of each page the console captured, so a build
     knows it recreated exactly what the maintainer's had (and says so when
     it did not).
   Turn every console learn log the project has (GoldenEye 31 rounds,
   Banjo 22, Mario Tennis 32, DK64, All-Star Baseball 26, ...) into these,
   game by game, until a ROM-only build interprets no more than the
   learn-log build.
3. **The measure.** For each Gold/Silver/priority game: interpreted
   routines and `[PERF2]` interpreter ms in the ROM-only build vs the
   learn-log build, in xemu; a game is done when they match. Then one
   console pass (`docs/compat.md`'s rule: the tier is the ROM-only build's).
4. **Later, not instead:** the app's own learning loop ("Improve this
   game": pull `learn.txt`/`learn_code.bin` from the user's Xbox over the
   FTP it already uses, rebuild with `--learn`, keep the last good build),
   for games no route reaches. The runtime already writes the files in
   every build.

## 1. Where things stand, in one table

| | |
|---|---|
| Gold (console, the maintainer) | Super Mario 64, Pilotwings 64, Super Smash Bros. |
| Silver (console) | Mario Kart 64, GoldenEye 007, Banjo-Kazooie, Ocarina of Time 1.2 |
| Experimental with a console run | Mario Party (board fix staged), Pokémon Snap (tutorial photo check works in xemu), Majora's Mask (boot fix staged), Jet Force Gemini, Kirby 64 (US: textures), Mario Tennis (fix staged), Castlevania LoD (crash) |
| Experimental, xemu only | Diddy Kong Racing, Star Fox 64, Paper Mario, Perfect Dark, Donkey Kong 64, Blast Corps, All-Star Baseball x3, Yoshi's Story, Ridge Racer 64, Dr. Mario 64, Mario Golf, ... |
| The library (246 ROMs, `sweep.py` s12) | 245 build; 163 still present frames at 30 s in xemu with two learning rounds; 55 never present; 24 die on a KUSEG read near NULL |
| Staged for red, not run | set 17: 15 builds (`build-host/hwbuild17.sh`) |
| Never done from PLAN.md | M3 (the measurement gate: no `docs/perf.md`, no T), code on demand (`XeLoadSection`: only `xbe_sections.py` knows the flag), ROM prefetch hints, `XRC_NATIVE_LIBC`, PGO, M7 (the fresh-machine test per OS; nothing is released) |

Two patterns in the record since 2026-10-16 decide this plan:

- **The work has been one game at a time.** Every console round found a
  new per-game fault (a texture rule, a thread order, a libultra shape) and
  fixed it behind a switch. 130+ switches later, most fixes did generalise
  (the rules are behavioural), but the method does not scale to 100 games:
  each round costs a console session and a day.
- **The failures cluster into a handful of causes.** The handoff's open
  items, the sweep's categories and the fixes of the last five days all land
  in the same seven buckets (section 3). Fixing a bucket moves many games at
  once; that is where the time should go.

## 2. The targets

- **25 Gold games.** Gold means the maintainer's rule in `docs/compat.md`:
  played through on a real Xbox, picture, sound, saves, controls and the
  game's own frame rate right, no bug a player would notice.
- **The top 100 supported "to some extent"**: built from the ROM alone by
  `n64rx` (no console learning round needed), boots to gameplay on a real
  Xbox with sound and saves, listed in `docs/compat.md` as Silver or as
  Experimental with a named, non-fatal limitation. The proposed list is in
  section 7 (edit it; it is sales plus "games people play", not a canon).
- A measurement that says how much CPU headroom the port has (the M3 gate),
  so that "the game's own frame rate" can be promised per game class
  instead of hoped for.

## 3. The seven buckets: what stops games, ranked by games moved per week of work

Ranking = (popular games it unblocks or promotes) x (how sure the fix is) /
(effort). Evidence lines cite the record.

### B1. Thread semantics: the kernel's scheduler is not libultra's

**Evidence.** Kirby 64's deadlock (a priority-0 thread holding "interrupts
off" preempted by a spinner: `XRC_INTS_BOOST`), Star Fox 64's boot race and
scheduler lag (`XRC_START_DEFER`, `XRC_RANK_LEVELS`, both off by default),
Castlevania's run-to-run variance (threads at 9, 10, 12, 100, 110, 120...
sharing levels), Mario Party's nnsched done-event mix-up (`XRC_RCP_OWNER`),
Ocarina's title stall (`XRC_RCP_ROOM`), Mario Tennis's VI manager
(`osSetEventMesg` order), Snap's object manager (start/stop on one level),
Castlevania's AI backlog, DK64's audio scheduler (open), Jeremy McGrath's
process switcher (open), the 55 sweep games that never present a frame
(threads up, no graphics task: the commonest shape of a scheduling fault).

**Cause.** `fixed_level` in `xbox/src/rt/ultra/thread.c` maps N64
priorities 13..110 onto one kernel level (10), 1..10 onto another (8); the
Xbox kernel time-slices threads on one level, where libultra runs the
highest-priority runnable thread alone until it blocks. And a game thread
blocked on a *host* thing (the log lock, the runtime's lock, a file write)
lets a lower N64 priority run, which the N64 never did. Every switch above
is a patch over one symptom of that.

**Fix.** A libultra-exact run rule in the runtime, game-agnostic: one
"run token" held by exactly one game thread at a time, the highest-priority
runnable one (the runtime's own threads, VI, timer, audio pump, save writer,
are the "interrupts" and stay outside). A game thread runs only with the
token; it gives it up when it blocks on an N64 object (queue, event, timer,
`pause_self`), never when it blocks on a host lock. A post that makes a
higher-priority thread runnable hands the token over at once (the kernel
levels still do the preemption; the token decides who may run among
equals and keeps a host-blocked thread's turn). Same-priority threads run
FIFO to the block, as libultra's run queue does. The done-event holding
(`XRC_RCP_DEFER/OWNER/ROOM`) stays: it models the RCP's asynchrony, not the
scheduler. Cost: one event signal and one wait per N64 context switch (a few
hundred a second). Keep `XRC_RANK_LEVELS` as the fallback.

**Done when.** The 14-game regression set and the console set run with the
token on by default; Star Fox 64 past its scene change with no per-game
switch; Castlevania's four boots identical; Kirby's boot 10 of 10; the
sweep's "no frames" count falls (expect 10-20 of the 55).

**Effort.** 1-2 weeks including the console rounds. Risk: medium (the
runtime's own blocking paths must all be classified N64 vs host; test
builds should assert a game thread never blocks on an N64 object while
holding the token).

### B2. Code the analyser cannot see: compressed, CPU-copied, relocated

**Evidence.** Rare's games (Banjo 22 learning rounds, GoldenEye 30, Blast
Corps 15, DK64, JFG, Conker), Camelot's (Mario Tennis 32, Mario Golf),
Acclaim's loaders (All-Star Baseball 26), Dr. Mario 64's gzip main
segment, Virtual Pool / Starshot copying code out of the boot image, the
sweep's "boot code came back" class (19) and its second round
(`--learn 2`) adding 16 games. Every one needs the console (or xemu) to run
the game and capture RDRAM, round after round, before a user's build is
whole: a user cannot do that.

**Done (2026-10-23):** `tools/dryboot/`, on by default as `N64RX_DRYBOOT=auto`
(used when it learns 16 functions or more), prebuilt in the app's bundle;
`docs/analyzer.md` has how it works and what it finds.

**Fix: a build-time dry boot.** Run the ROM's boot on the user's PC in a
CPU-only emulator built from pieces the project already has: `interp.c`
(host-tested MIPS interpreter), `mmio.c` (the RCP register model), a PI DMA
model over the ROM, a VI retrace on an instruction budget, SI answering "no
controller", SP/DP tasks marked done at once. No RCP, no graphics: just the
VR4300 and its interrupts, so the game's own libultra, loaders and
decompressors run as they would on the console, until the main code is in
RAM (stop at the first osStartThread of a thread whose entry is in code no
ROM DMA put there, or after N million instructions). Write what the safety
net would have written: `learn.txt` + `learn_code.bin` records, and feed
them to the existing `--learn` path. Nothing new to teach the analyser; the
facts stay in the user's build folder as today. Later overlays a game
decompresses per level still need a run (the console loop stays for those),
but the first build of a Rare or Camelot game is then whole. Expected: the
"boot code came back", gzip and inflate classes close on the first build;
GoldenEye and Perfect Dark's 0x7000xxxx code and their TLB tables come from
the dry boot too.

**Also:** ship the *learnt facts* (function starts, DMA segments, TLB
regions, relocated overlays: addresses and sizes, never bytes) for the 25
games in their packs (`games/<id>/learned.txt`); CLAUDE.md allows facts.
Then a user's first build of Ocarina of Time recompiles the overlays red
has seen, with no round.

**Done when.** Banjo-Kazooie, GoldenEye, Mario Tennis, Dr. Mario 64 and
Blast Corps build from the ROM alone with 0 learning rounds to the state
their best learnt build reaches today (xemu frames and `[LEARN]` counts).

**Effort.** 2-3 weeks (the CPU emulator is mostly glue around `interp.c`;
the stop rule and the interrupt model are the work). Risk: low-medium.

### B3. Microcodes the renderer and the audio HLE do not know

**Evidence.** Graphics: Rare's later GBI (Jet Force Gemini's 3D, Perfect
Dark shares GoldenEye's), DKR (done 2026-10-20), S2DEX's `G_SELECT_DL`,
L3DEX2 lines (drawn as nothing), F3DEX2's forced matrix / `G_MV_POINT` /
`G_LINE3D`, Factor 5 (Rogue Squadron, Battle for Naboo, Indiana Jones),
Boss Game Studios (Top Gear Rally, World Driver Championship, Stunt Racer),
Acclaim's Turok family. Audio: MusyX (Factor 5's driver and its licensees),
Rare's (JFG silent on red), `n_aspMain`'s opcode-14 filters (DK64, Conker),
the MP3 microcode, S8DEC/UNK_25 (SM64 Shindou), everything "unknown
microcode: silent".

**First step: count them.** The s12 sweep logs hold every game's
`[GFX] ucode ...` line and `[AUDIO] ... microcode` line. Add two columns to
`sweep.py --rescan`'s `table.md` (graphics family / version text, audio ABI
or "unknown") and rank the unknowns by the top-100 list. That table decides
the order below; the guesses here are from the record and the research
tables in section 7.

**Order (expected).** 1. Rare's PD/JFG GBI (two Gold candidates: Perfect
Dark, Jet Force Gemini; the DKR work shows the method: read the decomp's
GBI header as the key). 2. MusyX audio HLE (several top-100 games; the ABI
is documented by emulators, read for behaviour, written here). 3. S2DEX
complete + L3DEX2 (Pokémon Snap's lines, Yoshi's Story, Pokémon Stadium's
2D). 4. Rare's audio (if JFG's is not `n_aspMain` + filters). 5. Factor 5 /
Boss / Turok only if the count says so: each is a renderer of its own and a
week or more each.

**Done when.** No top-100 game logs "unknown microcode"; each new family
has a `test_f3dex` or `test_asp_hle` case.

### B4. Framebuffer effects are off by default

**Evidence.** Mario Kart 64's map TV (Silver's listed bug), Pokémon Snap's
photos (the probe is a special case of this), Banjo's jigsaw (readback mode
3, staged), Blast Corps's portrait frame, Ocarina's pause screen and the
Deku bubble, Perfect Dark's screens, Mario Tennis's capture, Smash's screen
flash. PLAN.md 3.7 lists four kinds; one is built behind `XGFX_FB_READBACK`
(off: Pilotwings 64 ran at 14 fps with mode 1).

**Fix.** Make mode 3 (convert on `osInvalDCache`) the default after the
console confirms its cost (two conversions per 600 frames on Banjo), and
build the two kinds that need no readback: a colour image used as a texture
(`SETTIMG` pointing at a swapped framebuffer: bind the render target, the
Melee-X EFB-copy path PLAN.md names) and an off-screen colour image drawn in
the same frame (an NV2A render target per `SETCIMG` size, drawn to and then
sampled). Depth-image reads (the Lens of Truth) stay out.

**Done when.** Mario Kart 64 Gold (the map TV right), Snap's photo check
without the probe special case, Ocarina's pause screen right.

**Effort.** 2 weeks. Risk: medium (the NV2A has the pieces; the cost is
memory at 720p).

### B5. Memory for the big games

**Evidence.** Ocarina at 17 MB of image with every overlay recompiled (so
only learnt ones are), Majora's Mask and DK64 at 8 MB RDRAM, Paper Mario's
40 MB ROM, 720p's 9 MB of buffers, 11-15 MB free on Banjo and Smash at 720p,
the page cache's 16/12 MB caps on a 10 ms-seek HDD.

**Fix.** PLAN.md 3.4's code on demand (`XRC_OVL_SECTIONS`: per-section PE
sections, the preload bit cleared, `XeLoadSection` on first map) was never
built in the runtime. Build it for the Zelda engine's relocated overlays
first (they already are relocatable sections), then measure `[MEMB]` with
all 468 of Ocarina's overlays recompiled. Prefetch hints for the page cache
(audio and animation tables) are the other half; `[PI]` stall numbers on
red decide.

**Done when.** Ocarina and Majora's Mask run with every overlay recompiled
(no safety-net hitches at scene loads) under 60 MB at 480 and 720p.

### B6. Performance for every game: the translation factor was never measured

**Evidence.** PLAN.md §3.11/M3: the gate before any second game; never run.
The record's numbers: Pilotwings 53-59 fps recompiled vs Wings64-X native
(no side-by-side), GoldenEye's dips to 40 fps, Ocarina's 20 fps with
6-13 ms display lists and 6-7 ms audio, Mario Tennis's match at 25 fps in
xemu, the interpreter ~8-10 ms a frame on GoldenEye's Dam in xemu against
1.5 ms for the back end.

**A measured lever (this review).** The generated code keeps every MIPS
register in `ctx->rN` memory and, under `-fno-strict-aliasing`, LLVM must
assume every RDRAM store may alias them: after each `sw` it reloads the
registers it had in x86 registers. Compiled with the project's own flags
and header (`xbox/include/xrc/recomp.h`), a four-instruction MIPS fill loop
becomes 6 x86 instructions with 4 memory operands per iteration at 32-bit
registers and 15 instructions with 8 memory operands at 64-bit; with
`restrict` on the two parameters it is 4 register-only instructions in both
modes. The generator never takes the address of a context field; the one
indirect access is `ctx->f_odd[(i-1)*2]` for odd FPRs, a pointer loaded
from the context, which LLVM's `noalias` would (wrongly) assume cannot
alias `ctx->fN.u32h`. So the change is: (1) make `f_odd` an in-struct array
alias (an anonymous union over the FPR block: `uint32_t f_odd[63]` at
offset 4 of `f0`), dropping the pointer and `xrc_ctx_init`'s assignment;
(2) have `n64rx` add `restrict` to both parameters in `funcs_N.c` and
`funcs.h` (a text rewrite like the `$zero` one), behind
`N64RX_RESTRICT=0`; (3) `test_recomp_h` and `test_interp` as they are, plus
an odd-FPR case; (4) measure on red: Pilotwings 64, Super Mario 64, Smash.
Expected: a double-digit percentage of game-code time; the 64-bit games
(Banjo-Kazooie) gain most.

**The rest of the M3 table**, in this order: the `restrict` A/B; weak
`RECOMP_FUNC` vs `XRC_FUNC_PLAIN` under ThinLTO; `-O2` vs `-O3`; the
interpreter's share (`[DLBENCH]` exists; add an `[INTERP]` bucket for the
safety net's time per frame); the texture cache's hashing and uploads as
their own bucket; audio per command kind (`ASP_HLE_OPTIME`); kernel calls
from `XRC_INTS_BOOST` (count them per frame: a game that toggles interrupts
in a hot loop pays two kernel calls each time); the PAL 50 Hz skip
(5 frames in 6 vblanks: uneven; a 50 Hz output mode or a fractional pacer
would be right); the interpreter: a pre-decoded page cache (decode once per
4 KB page, invalidated by the guard's DMA/invalidate hooks) for the code
that stays interpreted (relocatable code, hand-written routines, the
learning loop's first builds).

**Done when.** `docs/perf.md` exists with the table, repeated runs within
3 % (PLAN.md's rule), and each lever on or off by the numbers.

### B7. libultra the rules do not name, and what follows from it

**Evidence.** The debug libultra (Mischief Makers: `__osError`), GCC-built
libultra (Paper Mario's Flash and Pfs families), the -O0 shapes, games
whose own schedulers switch stacks (Jeremy McGrath; Chopper Attack), the
24 KUSEG NULL reads (mostly a symptom of B1/B2: a value an unnamed routine
should have set).

**Fix.** Keep the rule work data-driven: after B2's dry boot, re-run
`decomp_check.py` on every decomp pair and `test_analyze`; add `__osError`
by shape; name the Flash/Pfs families (the runtime has them); a strands
teacher for the two known process-switcher shapes. Each rule with a negative
test in `test_entry.py`.

## 4. The order of work

Each phase ends with: host tests green, the 14-game xemu regression set
presenting with no crash, a console set on red with the logs pulled, the
handoff entry, `docs/compat.md` moved only by console runs.

| phase | weeks | what | exit check |
|---|---|---|---|
| 0. Measure and tidy | 1 | the M3 gate's first table (B6: PW64 vs Wings64-X, SM64, Smash on red; the `restrict` A/B); the review's high findings fixed (`docs/review-2026-10.md`); the sweep's new columns (B3's count); `docs/compat.md` as the only status table (CLAUDE.md's status paragraph becomes a pointer) | `docs/perf.md` with numbers; the microcode count table; review items closed or ticketed |
| 1. Structure | 3 | B1 the run token; B2 the dry boot and learnt facts in the 25 packs | Star Fox 64, Castlevania, Kirby stable without per-game switches; Banjo/GoldenEye/Mario Tennis whole on the first build |
| 2. Picture | 2 | B4 framebuffer effects by default; B3's first two families (Rare PD/JFG, S2DEX+L3DEX2) | Mario Kart 64 Gold; Perfect Dark and JFG in game; Snap's photos |
| 3. Sound and memory | 2 | B3 MusyX and Rare audio; B5 code on demand; prefetch | no top-100 game silent; the Zeldas with every overlay recompiled |
| 4. The 25 | ongoing | per-game console loops on the 25 (section 6), two or three games a set, the regression set every time | `docs/compat.md`: 25 Gold |
| 5. Release | 2 | M7: the app on a fresh machine per OS, code signing, the GPL source offer for the bundle, the README's three steps timed | a release tag; the toolchain release; the first public beta |

Why this order and not PLAN.md's (M3, M4 G1, M5 OoT, M6 video, M7 tool,
M8 MM): M4-M6 happened out of order already (the record), M3 did not, and
the two structural items (B1, B2) remove the per-game grind that would
otherwise eat phases 2-4.

## 5. Process: how to stop working one game at a time

1. **Every console round carries the regression set**, never one game: the
   Gold three plus the Silver four as a minimum, each boot's `[MEM]`,
   `[CLOCK]`, `[PERF]` lines compared with the last (a script that diffs the
   pulled logs: `tools/xbox/console_round.py` as Melee-X has).
2. **The sweep is the compatibility metric.** Run it per week from a pinned
   worktree (`docs/testing.md`), diff with `sweep_diff.py`, and put the three
   numbers (present at 30 s, never present, crash classes) at the top of
   `docs/handoff.md`. A fix that moves the sweep is a holistic fix; one that
   moves one game is a pack's.
3. **A bucket ledger, not per-game notes.** Each new fault is filed under
   B1-B7 (or a new bucket) with the games it hits before it is fixed; the
   fix is judged by the bucket's count.
4. **A fix lands with a host test or a replay.** `bench_dl` dumps for the
   renderer, `test_asp_hle` vectors for audio, `test_entry` negatives for
   the analyser, a scenario for the runtime. The review found the biggest
   untested files (`docs/review-2026-10.md` §8); fill those first.
5. **Shorten the docs' status surface.** `docs/compat.md` is the status;
   CLAUDE.md's status paragraph and README's list point at it; the handoff
   keeps the record. The review found the contradictions (review §7).
6. **Users as testers, by consent.** The app can offer "copy a compatibility
   report" (the boot log's first lines, `[PERF]`, the tier) and "export
   learn.txt" for the user to attach to an issue; nothing leaves the
   machine by itself (CLAUDE.md).

## 6. The 25 Gold candidates and the gap for each

Chosen for play value and feasibility (4 MB RDRAM, a known microcode, a
decomp to read). Status from `docs/compat.md` and the handoff; the gap is
the bucket that closes it.

| # | game | now | gap to Gold |
|---|---|---|---|
| 1 | Super Mario 64 | Gold | keep (regression set) |
| 2 | Pilotwings 64 | Gold | keep |
| 3 | Super Smash Bros. | Gold | keep; the audio-boot freeze watched (`[AUDIO] step:`) |
| 4 | Mario Kart 64 | Silver | B4 (map TV); drift boosts: a reference run to compare |
| 5 | GoldenEye 007 | Silver | B6 (dips to 40 fps: the interpreter's share, round 31's page learnt); B2 |
| 6 | Banjo-Kazooie | Silver | START-screen static (not reproduced in xemu: console screenshots); B2 (22 rounds); B6 (64-bit registers) |
| 7 | Ocarina of Time | Silver | B5 (overlays recompiled, hitches), B4 (pause), audio skips (B1/B5) |
| 8 | Mario Party | Exp. | board freeze fix staged (`vmp17`); textures shuffled on the board (open); B1 |
| 9 | Mario Party 2 | xemu | B1 (its audio thread restart) |
| 10 | Mario Party 3 | xemu | B1 |
| 11 | Pokémon Snap | Exp. | the tutorial's unlock on red (follow the prompts); B4 for the photo review |
| 12 | Star Fox 64 | xemu | B1 (needs both thread switches today); scene-load stall |
| 13 | Diddy Kong Racing | xemu | first console run (`vdkr1`); a race not tried; B3 audio (Rare) |
| 14 | Kirby 64 (US) | Exp. | console textures (likely the stale page tables, fixed in set 17) |
| 15 | Paper Mario | xemu | B2 (effects, maps), B7 (Flash/Pfs names), 40 MB ROM (B5) |
| 16 | Majora's Mask | Exp. | boot fix staged (`vmm16`); 8 MB + overlays (B5); `njpgdsp` task |
| 17 | Perfect Dark | xemu | B2 (past its boot), B3 (Rare's GBI), 8 MB (B5) |
| 18 | Mario Tennis | Exp. | fix staged (`vmt5`); B2 (32 rounds); match speed (B6) |
| 19 | Mario Golf | xemu | B2 (Camelot engine) |
| 20 | Yoshi's Story | xemu | S2DEX (B3); not on a console |
| 21 | F-Zero X | xemu | stalls with no frames (B1); not looked at since 2026-10-16 |
| 22 | Wave Race 64 | xemu | presents in the sweep; never played |
| 23 | Blast Corps | xemu | B2 (15 rounds); its portrait frame (B4) |
| 24 | Jet Force Gemini | Exp. | B3 (Rare's 3D GBI and audio); B2 |
| 25 | Donkey Kong 64 | xemu | audio stops (B1: its scheduler), B2, 8 MB (B5) |

Alternates if one proves out of reach: Banjo-Tooie, Conker (Rare's later
engine: B2 + B3), 1080°, Excitebike 64, WWF No Mercy, Bomberman 64 (its
loader, open), Mischief Makers (B7).

## 7. The top 100 (proposed) with the bucket each sits in

The proposed list (section 2: edit it), with what the record says about
each. **Code** and **ROM MB**: the No-Intro DAT (libretro-database,
2026-10-07), the USA retail dump, checked for all 100 rows; the code is
the header's game code, the one `n64rx` reads (1080° carries `NTEA`, its
box NUS-NTEE-USA); Sin and Punishment is Japan only. **Save**,
**players**, **paks**: the mupen64plus community database (`(U) [!]`
entries), not checked against carts: EEP 4K and EEP 16K are EEPROMs of 4
and 16 Kbit (the database writes 4KB and 16KB), CPak = no cartridge save,
the Controller Pak only, `?` = no entry; paks R = Rumble Pak, C =
Controller Pak, T = Transfer Pak, `-` = none listed (not "none used").
**Record**: the last state in `docs/handoff.md`, `docs/compat.md`,
`docs/analyzer.md` and `docs/decisions.md` at `ba03cd1`, xemu unless it
says red; Gold, Silver and Exp. are `docs/compat.md`'s tiers; "sweep sN"
= a `tools/xbox/sweep.py` run (30-45 s boots from the ROM alone); `-` =
not in the record. **Engine / microcode**: "record" = the project has
seen it; the rest from public decomp and emulator sources read on
2026-10-22, `?` = a search snippet or an inference, "verify" = nothing
found; every cell is checked against the sweep's new columns (B3) before
it is relied on. "decomp" = a public decompilation (an answer key, never
shipped); "recomp" = an N64Recomp-based port exists (GPL: read for
behaviour only).

How to read the buckets: B1-B7 are section 3's (B1 threads, B2 code the
analyser cannot see, B3 microcodes, B4 framebuffer effects, B5 memory, B6
performance, B7 unnamed libultra), the likeliest first; "keep" = Gold, in
the regression set; "console run" = the next step is a run on red; `?` =
no evidence yet, and the game's first sweep row decides.

| # | game | code | ROM MB | save | players | paks | record | bucket | engine / microcode (verify) |
|---|---|---|---|---|---|---|---|---|---|
| 1 | Super Mario 64 | NSME | 8 | EEP 4K | 1 | - | Gold (red: 29.9 fps) | keep | Fast3D ?; decomp (CC0) |
| 2 | Mario Kart 64 | NKTE | 12 | EEP 4K | 4 | C | Silver (red: races at 29.9 fps; the map-screen TV wrong; drift boosts in question) | B4 | verify; decomp WIP; recomp |
| 3 | GoldenEye 007 | NGEE | 12 | EEP 4K | 4 | R | Silver (red: fine, dips to ~40 fps) | B6, B2 | verify (Perfect Dark shares its GBI); audio Rare's own ?; decomp 100 % ? |
| 4 | Ocarina of Time 1.2 | CZLE | 32 | SRAM | 1 | R | Silver (red: its own 20 fps; scene-load hitches, audio skips, pop-in) | B5, B4 | F3DZEX2 (record); decomp WIP; recomp |
| 5 | Super Smash Bros. | NALE | 16 | SRAM | 4 | R | Gold (red: 59.9 fps; one audio-boot freeze, watched) | keep | verify; decomp (its status conflicts) |
| 6 | Donkey Kong 64 | NDOE | 32 | EEP 16K | 4 | R | xemu: file screen, draws on; audio stops once graphics start (its scheduler); 8 MB | B1, B2, B5 | verify; audio `n_aspMain` with opcode-14 filters (record); needs the Expansion Pak |
| 7 | Diddy Kong Racing | NDYE | 12 | EEP 4K | 4 | R C | xemu: intro, menus and a 3D scene whole since the DKR microcode (2026-10-20); `vdkr1` staged | B3 (audio), console run | Rare's DKR microcode (record); audio Rare's own ?; decomp ~80 % ? |
| 8 | Pokémon Stadium | NPOE | 32 | Flash | 4 | T | sweep s7: 20 frames | B1, B3 | S2DEX for its 2D ? (B3); the Transfer Pak out of scope |
| 9 | Star Fox 64 | NFXE | 12 | EEP 4K | 4 | R | xemu: past the scene change only with `XRC_START_DEFER=2` and `XRC_RANK_LEVELS=1`; `vsf1` staged | B1 | verify; decomp complete (CC0); recomp |
| 10 | Banjo-Kazooie | NBKE | 16 | EEP 4K | 1 | R | Silver (red: ~30 fps; START-screen static open) | B2, B6 | verify; audio Rare's own ?; decomp ?; recomp |
| 11 | Majora's Mask | NZSE | 32 | Flash | 1 | R | Exp. (red: a crash after the Nintendo logo; fix staged, `vmm16`); xemu: name entry, audio | B5, B1 | F3DZEX2, S2DEX2, aspMain, njpgdspMain ? (decomp file names); needs the Expansion Pak; decomp WIP; recomp |
| 12 | Pokémon Snap | NPFE | 16 | Flash | 1 | - | Exp. (red: Beach at 29.7 fps; the camera lock is the tutorial; photo check works in xemu) | console run, B4 | F3DEX2 (record), L3DEX2 lines (B3); decomp WIP; recomp |
| 13 | Mario Party | CLBE | 32 | EEP 4K | 4 | R | Exp. (red: to a board; board freeze fixed, `vmp17` staged; textures shuffled, open) | B1 | verify |
| 14 | Mario Party 2 | NMWE | 32 | EEP 4K | 4 | R | xemu: intro, menus; survives its audio thread restart | B1 | verify |
| 15 | Mario Party 3 | NMVE | 32 | EEP 16K | 4 | R | xemu: 3377 frames at 59 fps (overlay precheck) | B1 | verify; decomp ongoing ? |
| 16 | Perfect Dark | NPDE | 32 | EEP 16K | 4 | R C T | xemu: past its boot (round 8), no frame yet; 0x70000000 TLB code; 8 MB | B2, B3, B5 | Rare's later GBI (shares GoldenEye's, B3); audio Rare's own ?; Expansion Pak required (disputed); decomp complete (MIT) |
| 17 | Paper Mario | NMQE | 40 | Flash | 1 | R | xemu: 3139 frames in 70 s (round 8: effects recompiled); maps and effects through its TLB at 0xE0xxxxxx | B2, B7, B5 | verify; GCC-built libultra (B7); decomp WIP |
| 18 | Kirby 64 (US) | NK4E | 32 | EEP 16K | 4 | R | Exp. (red: "major texture failure", likely the stale page tables fixed in set 17); (E) to its tutorial in xemu | console run | F3DEX2 (record); decomp ongoing ? |
| 19 | Wave Race 64 | NWRE | 8 | EEP 4K | 2 | C | sweep: presents the whole 30 s; never played | B1? | verify |
| 20 | Pokémon Stadium 2 | NP3E | 64 | Flash | 4 | T | sweep s12: presents | B3, B1 | S2DEX for its 2D ? (B3) |
| 21 | F-Zero X | CFZE | 16 | SRAM | 4 | R | sweep s7: 42 frames, then stops; "stalls with no frames" (2026-10-16) | B1 | verify |
| 22 | Yoshi's Story | NYSE | 16 | EEP 16K | 1 | R | xemu: title and story pages draw (S2DEX objects, 2026-10-18) | B3, console run | S2DEX (record; `G_SELECT_DL` missing) |
| 23 | Banjo-Tooie | NB7E | 32 | EEP 16K | 4 | R | sweep (E): fails in N64Recomp or the compile (2026-10-16; not revisited) | B2, B3 | Rare's later engine ?; audio Rare's own ? |
| 24 | Conker's Bad Fur Day | NFUE | 64 | EEP 16K | 4 | R | xemu: pages its 0x15000000 code through its own exception handler; stalls before its first graphics task | B2, B1 | verify (Rare); audio `n_aspMain` with opcode-14 filters (B3); decomp WIP (archived); recomp |
| 25 | Mario Tennis | NM8E | 16 | EEP 16K | 4 | R T | Exp. (red: white screen, frozen; two causes fixed, `vmt5` staged); xemu: matches at 25 fps after 32 rounds | B2, B1, B6 | verify (Camelot) |
| 26 | Mario Golf | NMFE | 32 | SRAM | 4 | R T | xemu: builds since `N64RX_J_LATER`; 70-125 frames, then stops (Camelot engine) | B2 | verify (Camelot) |
| 27 | 1080° Snowboarding | NTEA | 16 | SRAM | 2 | R | - | ? | verify |
| 28 | Excitebike 64 | NMXE | 16 | EEP 16K | 4 | R C | - | ? | verify; Expansion Pak optional (high resolution) ? |
| 29 | WWF No Mercy | NW4E | 32 | Flash | 4 | R C | sweep s7: 849 frames | B1? | verify |
| 30 | Jet Force Gemini | NJFE | 32 | Flash | 4 | R | Exp. (red: Rare logo, broken menu, slow, silent); xemu: menus whole since opcode 0x07 (2026-10-20); 3D not read | B3, B2 | Rare's later GBI (opcode 0x07, record); its 3D and audio unknown |
| 31 | Blast Corps | NBCE | 8 | EEP 4K | 1 | C | xemu: Rare logo, title, profile (portrait frame empty), attract demo at 50-56 fps (15 rounds) | B2, B4 | verify (Rare); decomp in progress ? |
| 32 | Pilotwings 64 | NPWE | 8 | EEP 4K | 1 | - | Gold (red: 53-59 fps) | keep | verify |
| 33 | Cruis'n USA | NCUE | 8 | EEP 4K | 2 | C | sweep (mmio round): 1 frame | B1, B7 | verify |
| 34 | Star Wars: Shadows of the Empire | NSWE | 12 | EEP 4K | 1 | - | - | ? | verify |
| 35 | Star Wars: Rogue Squadron | NRSE | 16 | EEP 4K | 1 | R | sweep s2: "the entry code is not the SDK's" (Factor 5 loader); not revisited | B2, B3 | Factor 5 microcode ?; MusyX ?; Expansion Pak optional (high resolution) |
| 36 | Star Wars Episode I: Racer | NEPE | 32 | EEP 16K | 2 | R | sweep: 8 MB; clears 640x480 framebuffers, the main thread wakes every retrace, never makes a graphics task | B1, B7, B5 | verify; Expansion Pak optional (high resolution) ? |
| 37 | Tony Hawk's Pro Skater | NTFE | 12 | CPak | 2 | R C | sweep s12: presents | B1? | verify |
| 38 | Tony Hawk's Pro Skater 2 | NTQE | 16 | CPak | 2 | R C | sweep s12: presents | B1? | verify |
| 39 | Resident Evil 2 | NREE | 64 | SRAM | 1 | R | - | B3, B5 | MusyX ? (record); Expansion Pak optional (high resolution) ? |
| 40 | Harvest Moon 64 | NYWE | 16 | SRAM | 1 | - | - (a recomp exists) | ? | verify; recomp |
| 41 | Ogre Battle 64 | NOBE | 40 | SRAM | 1 | C | sweep: `osCartRomInit` polling PI_STATUS (named since); 1 frame (mmio round) | B7, B5 | verify |
| 42 | Mischief Makers | NTME | 8 | EEP 4K | 1 | - | sweep: never presents | B7 | verify; ships the debug libultra (`__osError`, record) |
| 43 | Bomberman 64 | NBME | 8 | EEP 4K | 4 | C | sweep s10: presents; its loader's DMA over a thread's stack (open) | B2, B7 | verify; decomp in progress ?; recomp |
| 44 | Castlevania (1999) | ND3E | 12 | CPak | 1 | C | - | ? | verify; Expansion Pak optional (high resolution) ? |
| 45 | Castlevania: Legacy of Darkness | ND4E | 16 | CPak | 1 | C | Exp. (red: ~70 s, then a crash; fixed since, `vcv3` staged); xemu: Foggy Lake | B1, B2 | verify |
| 46 | Quest 64 | NETE | 16 | CPak | 1 | C | - | ? | verify |
| 47 | Body Harvest | NBHE | 12 | EEP 4K | 1 | R | sweep s2: presents; s9: a KUSEG NULL read (no cause yet) | B1, B7 | verify; decomp in progress ? |
| 48 | Space Station Silicon Valley | NSVE | 8 | EEP 4K | 1 | R | - | ? | verify; decomp WIP; recomp |
| 49 | Beetle Adventure Racing | NNSE | 16 | CPak | 4 | R C | sweep s12: presents | B1? | verify |
| 50 | Mega Man 64 | NM6E | 32 | ? | ? | - | sweep: fails in N64Recomp or the compile (2026-10-16; not revisited) | B2, B7 | verify |
| 51 | Mystical Ninja Starring Goemon | NG5E | 16 | CPak | 1 | R C | sweep s2: presents | B1? | verify |
| 52 | Goemon's Great Adventure | NGME | 16 | CPak | 2 | R C | sweep s10: presents (a KUSEG write before) | B1? | verify |
| 53 | Doom 64 | NDME | 8 | CPak | 1 | C | - | ? | verify |
| 54 | Duke Nukem 64 | NDNE | 8 | CPak | 4 | R C | - | ? | verify |
| 55 | Turok: Dinosaur Hunter | NTUE | 8 | CPak | 1 | C | - | ? | verify |
| 56 | Turok 2: Seeds of Evil | NT2E | 32 | CPak | 4 | R C | sweep: runs its main, stops past the 2 MB its TLB entry maps; entry code "not the SDK's" (s2) | B2, B3 | Turok microcode ? (record) |
| 57 | Turok 3: Shadow of Oblivion | NTKE | 32 | CPak | 4 | R C | sweep s2: entry code not the SDK's | B2, B3 | Turok microcode ? (record) |
| 58 | Rayman 2: The Great Escape | NY2E | 32 | CPak | 1 | C | sweep s12: presents (32 MB, through the page cache) | B1? | verify |
| 59 | Glover | NGVE | 8 | EEP 4K | 1 | - | sweep s9: presents | B1? | verify |
| 60 | Snowboard Kids | NSKE | 8 | CPak | 4 | R C | sweep s10: Snowboard Kids 2 presents | ? | verify |
| 61 | Rampage: World Tour | NRPE | 12 | CPak | 2 | R C | sweep s2: presents (Rampage 2 timing-dependent) | ? | verify |
| 62 | Gauntlet Legends | NGXE | 16 | CPak | 4 | R C | - | ? | verify |
| 63 | NFL Blitz | NBZE | 16 | CPak | 2 | R C | - | ? | verify |
| 64 | Wayne Gretzky's 3D Hockey | NWGE | 8 | CPak | 4 | C | sweep s2: presents | ? | verify |
| 65 | WCW/nWo Revenge | NW2E | 16 | SRAM | 4 | R | - | ? | verify |
| 66 | WWF WrestleMania 2000 | NWXE | 32 | SRAM | 4 | R C | - | ? | verify |
| 67 | Killer Instinct Gold | NKIE | 12 | EEP 4K | 2 | C | sweep s12: one frame; none in the mmio round | B1, B4 | verify |
| 68 | Mortal Kombat Trilogy | NMKE | 12 | none | 2 | - | - | ? | verify |
| 69 | Mortal Kombat 4 | NM4E | 16 | CPak | 2 | R C | builds and runs 70 s, no frames yet (2026-10-20) | B1, B7 | verify |
| 70 | Fighters Destiny | NKAE | 12 | EEP 4K | 2 | R C | sweep s10: lost (a KUSEG write) | B1, B7 | verify |
| 71 | Ridge Racer 64 | NRZE | 32 | EEP 16K | 4 | R | xemu: 1732 frames in 30 s (~44 fps); took no A press (2026-10-16) | B1? | F3DEX2 (record) |
| 72 | San Francisco Rush | NSFE | 8 | CPak | 2 | R C | - | ? | verify |
| 73 | San Francisco Rush 2049 | NRUE | 12 | CPak | 4 | R C | - | ? | verify |
| 74 | Top Gear Rally | NGRE | 8 | CPak | 2 | R C | - (Top Gear Overdrive presents, s10) | B3 | Boss Game Studios microcode ? (record) |
| 75 | Extreme-G | NEGE | 8 | CPak | 4 | R C | sweep s11: faults reading N64 0x1000 (a compressed loader) | B2 | verify |
| 76 | Wipeout 64 | NWPE | 8 | CPak | 4 | R C | sweep s9: lost (not rerun) | ? | verify |
| 77 | Pokémon Puzzle League | NPNE | 32 | Flash | 2 | - | sweep s2: `mtc0 EPC` (an exception handler); s9: a KUSEG NULL read, no cause | B7 | verify |
| 78 | Dr. Mario 64 | NN6E | 4 | EEP 4K | 4 | - | xemu: 3 learning rounds; presents ~28 s, then no graphics task (main thread busy); gzip main segment | B2, B1 | verify |
| 79 | Tetrisphere | NTPE | 8 | EEP 4K | 2 | - | sweep s2: presents | ? | verify |
| 80 | Bust-A-Move '99 | NB3E | 8 | CPak | 4 | R C | sweep s2: entry code not the SDK's | B2, B7 | verify |
| 81 | Chameleon Twist | NCTE | 12 | EEP 4K | 4 | R | sweep (mmio round): no frames (Chameleon Twist 2: 1 frame) | B1, B7 | verify |
| 82 | Rocket: Robot on Wheels | NSUE | 12 | EEP 4K | 1 | R | - | ? | verify |
| 83 | Bomberman Hero | NBDE | 12 | EEP 4K | 1 | R | sweep s10: presents | ? | verify |
| 84 | Hybrid Heaven | NHVE | 16 | CPak | 2 | R C | sweep: its Expansion Pak probe reads 0 (4 MB seen) | B5 | verify |
| 85 | Shadow Man | NSDE | 32 | CPak | 1 | R C | boots (`N64RX_TLB_ODD`), then a TLB miss at 0x00800004 before its exception code is in (2026-10-20) | B2 | verify |
| 86 | Indiana Jones and the Infernal Machine | NIJE | 32 | EEP 4K | 1 | R | sweep s2: entry code not the SDK's (Factor 5) | B2, B3 | Factor 5 microcode ?; MusyX ?; Expansion Pak optional (high resolution) ? |
| 87 | Star Wars Episode I: Battle for Naboo | NNAE | 32 | EEP 4K | 1 | R | sweep s2: entry code not the SDK's (Factor 5) | B2, B3 | Factor 5 microcode ? (Indiana Jones's) |
| 88 | Road Rash 64 | NROE | 32 | CPak | 4 | R C | sweep s2: entry code not the SDK's | B2 | verify |
| 89 | Vigilante 8 | NV8E | 8 | CPak | 4 | R C | sweep s12: presents (the zero-divisor fix) | ? | verify |
| 90 | Army Men: Sarge's Heroes | NAME | 8 | CPak | 4 | R C | - | ? | verify |
| 91 | Earthworm Jim 3D | NJME | 16 | EEP 4K | 1 | - | sweep s2: an "undeclared label" compile failure (the class is fixed) | ? | verify |
| 92 | Spider-Man | NSLE | 32 | CPak | 1 | R C | sweep s12: presents | ? | verify |
| 93 | StarCraft 64 | NSQE | 32 | Flash | 2 | R | - | ? | verify; the Expansion Pak for the Brood War levels ? |
| 94 | Command & Conquer | NCCE | 32 | Flash | 1 | R | - | ? | verify |
| 95 | Hey You, Pikachu! | NPGE | 16 | EEP 4K | 1 | R | - (the microphone) | out of scope | verify |
| 96 | Sin and Punishment (Japan) | NGUJ | 32 | ? | 2 | R | - | ? | verify |
| 97 | Mickey's Speedway USA | NMLE | 32 | EEP 4K | 4 | R T | - (Rare) | B2, B3 | verify (Rare) |
| 98 | International Superstar Soccer '98 | NWSE | 12 | CPak | 4 | C | sweep s2/s5: ISS 64 presents | ? | verify |
| 99 | Hydro Thunder | NHTE | 32 | CPak | 4 | R C | sweep (mmio round): no frames | B1, B7 | verify |
| 100 | Mission: Impossible | NMIE | 12 | EEP 4K | 1 | R | - | ? | verify |

## 8. What the review found (summary; the full list is `docs/review-2026-10.md`)

Ranked by severity, then by how many games or users each reaches; the
numbers are the review's items.

1. **3.1** (high, security): `n64rx` deletes any existing folder named
   like the ROM header's title inside the output folder the user picked.
2. **1.1, 1.2** (high): the host tests are red on Linux: `test_mmio` does
   not link, `test_f3dex` fails under GCC's `-Werror` (CI's Linux job
   would stop there).
3. **5b.1** (high): `osViSetEvent` never posts a retrace to a 1- or 2-slot
   queue (`XRC_VI_RESERVE`): a game that waits on one hangs.
4. **5a.1** (high): `ovl.c`'s `section_addresses` is overrun when
   function-less sections come first (the second learning round of
   GoldenEye or Conker): heap corruption, random crashes later.
5. **4.1** (high): the analyser's "exactly one candidate" rule does not
   run at the per-function sites, so the first fit gets the name (the
   record's wrong names; 4.10 lists three more of the kind).
6. **5d.2, 5d.3** (medium): the atomic writer's shared verify buffer and
   the dirty flag cleared before the write: a save can be lost, the exit
   combo's flush included.
7. **5b.5-5b.7, 5b.10** (medium): races in the thread layer (a stopped
   thread keeps the runtime's lock, `osStartThread`'s unbounded wait,
   `osSetThreadPri` dropping the interrupts-off boost, unboosted lock
   holders): more evidence for B1.
8. **5c.1-5c.3** (medium): the display-list interpreter reads past the
   RDRAM window (vertices, texture hashing, the command stream) and has no
   command budget: a self-referencing list hangs it.
9. **5a.2** (medium): a store to the read-only cartridge map retries for
   ever: 100 % CPU, no crash card, no log line.
10. **5b.2, 5b.8** (medium): PI DMA bounds (a DMA across the ROM's end
    zero-fills its in-ROM part, a destination past RDRAM wraps onto
    physical 0) and the EEPROM length overflow.
11. **5a.3-5a.5** (medium): the safety net's limits: 512 stub slots behind
    a 4096-address table, stubs an overlay switch clears never entered
    again, `learn.txt` silently capped at 48 KB (Ocarina's learning cannot
    converge).
12. **3.2** (medium): `rom_leak_check` misses archives, encoded ROMs,
    generated code under `tests/` and frames at the allowed paths, and
    nothing runs it automatically.
13. **§2** (performance, measured): `restrict` on the generated code's two
    parameters (B6); `xrc_pbkit` is built with no `-O`.

Everything else (the rest of the PC tool's security and robustness items,
the analyser's costs, the licences, the docs debt, the coverage map and
what the review did not cover) is in `docs/review-2026-10.md`.

## 9. Questions for the maintainer

1. The 25: is the list in section 6 the one you want? (Banjo-Tooie or
   Conker instead of Blast Corps / Wave Race?)
2. B1 changes the runtime's threading for every game; it goes in behind a
   switch, but the default flips only after a console set. Fine?
3. The dry boot (B2) runs the game's boot code on the user's PC in an
   interpreter at build time (a few seconds). Any objection in principle
   (nothing leaves the machine; nothing is kept but facts and the captured
   code in the build folder, as the console loop does today)?
4. Learnt facts for the 25 in their packs (addresses, sizes; no bytes):
   consistent with CLAUDE.md's "packs hold facts"? It makes the first build
   of Ocarina whole for a user.
5. Console time: the plan assumes one set on red every two or three days
   and a 128 MB / upgraded-CPU console once per release (PLAN.md §8 Q9).
6. MusyX and the Factor 5 / Boss renderers are each a week or more; say
   which of their games matter to you (Rogue Squadron? Top Gear Rally?).
