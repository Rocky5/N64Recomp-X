# N64Recomp-X: implementation plan

**N64 games on the original Xbox by static recompilation.** The user's own ROM
goes through [N64Recomp](https://github.com/N64Recomp/N64Recomp) (MIT), which
turns the game's MIPS code into C; that C is compiled for the Pentium III with
nxdk and LLVM 21 and linked with an Xbox runtime that replaces libultra, feeds
the game's display lists to the NV2A and its audio command lists to the AC97.
Not an emulator: no interpreter, no dynarec, no RDP rasteriser on the CPU. The
platform layer is the one Melee-X, OpenCrossing-Xbox and Wings64-X run on.
Target: retail Xbox, stock 64 MB. Deliverable: a `default.xbe` per game, built
on the user's own machine by one command, plus the runtime, the interpreter and
the game packs that make that possible.

This file is the plan for the engineer or agent who builds it. It decides the
approach and the architecture, orders the work into milestones with exit checks,
and lists the risks and the questions only the maintainer can answer. It was
written 2026-10-16 from the sibling repos (`Melee-X`, `OpenCrossing-Xbox`,
`Wings64-X`, `wings64-dc`, `Snap64-X`, all under `github.com/TechProGabe`), from
shallow clones of N64Recomp, N64ModernRuntime, Zelda64Recomp and nxdk read on
that day, and from the survey in `docs/research.md`. The repo holds no code yet.

Read in this order: this file; `docs/research.md`; then in the sibling
checkouts Wings64-X `CLAUDE.md`, `PLAN.md` (D1-D13), `docs/architecture.md`,
`docs/platform.md`, `docs/toolchain.md`, `docs/testing.md`, `docs/handoff.md`,
`xbox/src/game/ultra/os_shim.c`, `xbox/src/gfx/f3d_interp.c`,
`xbox/src/hw/asp_hle.c`, `xbox/CMakeLists.txt`, `tools/xbox/compile_game.py`,
`tools/xbox/thinlto_link.py`; Melee-X `docs/architecture.md`, `docs/platform.md`
(the lazy reservation and the crash guard), `docs/renderer.md`,
`docs/decisions.md`; Snap64-X `docs/PLAN.md` §3.4 (the F3DEX2 interpreter
design); OpenCrossing-Xbox `docs/traps.md`. In the N64Recomp clone:
`include/recomp.h` (the whole contract between generated code and runtime),
`src/symbol_lists.cpp`, `src/config.cpp`, `src/main.cpp`; and in
Zelda64Recomp: `us.rev1.toml`, `patches.toml`, `patches/Makefile`.

---

## 0. Ground truth

| fact | value | source |
|---|---|---|
| Recompiler | `N64Recomp/N64Recomp` @ `ffb39cd` (2026-05-26), MIT (c) 2024 Wiseguy; C++20, CMake; libs rabbitizer, ELFIO, fmt, toml++, sljit | clone, `LICENSE`, `CMakeLists.txt` |
| Generated code | one C function per MIPS function: `RECOMP_FUNC void f(uint8_t* rdram, recomp_context* ctx)`; registers in `ctx` as `gpr` = `uint64_t`; memory through `MEM_W/H/B` macros on a word-swapped RDRAM image; indirect calls `LOOKUP_FUNC(v)(rdram, ctx)`; overlays through `RELOC_HI16/LO16(section, offset)` and `section_addresses[]`; a branch to itself becomes `pause_self(rdram)`; 50 functions per output file by default (`funcs_N.c`, `funcs.h`, `lookup.cpp`, `recomp_overlays.inl`) | `include/recomp.h`, `src/cgenerator.cpp`, `src/main.cpp` |
| What the runtime must supply | the macro header (`recomp.h` is MIT and ours to replace), `get_function`, `section_addresses`, `recomp_syscall_handler`, `do_break`, `switch_error`, `cop0_status_read/write`, `pause_self`, `recomp_trigger_event`, and one `<name>_recomp(rdram, ctx)` per libultra function in the recompiler's `reimplemented_funcs` list (threads, queues, events, timers, time, VI, SP, DP, AI, PI, SI, EEPROM, SRAM/Flash, PFS, motor, cache ops); libc/math names in `renamed_funcs` stay recompiled under a renamed symbol | `src/symbol_lists.cpp`, N64ModernRuntime `librecomp/src/ultra_translation.cpp` (133 entry points, read for the surface only) |
| Reference runtime | `N64Recomp/N64ModernRuntime` @ `cdf5abb` (2026-08-30), **GPL-3.0**: `ultramodern` (threads as host threads with a priority run queue, VI/task/gfx threads, RT64 renderer callbacks) + `librecomp` (overlays by PI-DMA source, PI/saves, RSP for recompiled microcode). **No code is taken from it**; it is read for behaviour and for the list of entry points | clone, `COPYING` |
| Flagship | `Zelda64Recomp/Zelda64Recomp` @ `b65c482` (2026-09-25), GPL-3.0: Majora's Mask from a symbol file (17,765 functions, 617 relocatable sections), two RSP microcodes recompiled (`aspMain`, `njpgdspMain`), patches written in C compiled to MIPS by clang and recompiled in single-file mode; requires D3D12 SM6 / Vulkan 1.2 / Metal and SSE4.1 | clone, `us.rev1.toml`, `patches.toml`, `patches/Makefile`, `README.md` |
| Symbols | the recompiler takes an ELF (`elf_path`, `use_mdebug` for IDO symbols) or a TOML of sections and functions (`symbols_file_path`: `[[section]] name/rom/vram/size`, `functions = [{name, vram, size}]`, `relocs` for relocatable sections); `--dump-context` writes that TOML from an ELF | `src/config.cpp`, `src/main.cpp`, Zelda64RecompSyms |
| Xbox | Pentium III 733 MHz (SSE1, MMX, no SSE2, 16 KB L1I/L1D, 128 KB L2), 64 MB shared, NV2A (vertex programs, 8+1 register combiners, 4 texture units, Z24S8), AC97; the kernel loads XBE sections on demand (`XeLoadSection`/`XeUnloadSection`, ref-counted) and nxdk's `cxbe` marks every section preload | Melee-X `docs/architecture.md`, nxdk `lib/xboxkrnl/xboxkrnl.h`, `tools/cxbe/Xbe.cpp` |
| N64 | VR4300 93.75 MHz (MIPS III, 64-bit registers, O32 code), RSP 62.5 MHz (8-lane 16-bit vector unit), 4 MB RDRAM (8 with the Expansion Pak), VI 60 Hz, cart reads at a few MB/s | public hardware facts |
| Platform layer | Wings64-X `xbox/src/hw` (25 C files: boot, splash, video, pad, AC97, clock, RAM lock, crash/hang reporters, profiler, fbdump, autopad, NV2A back end, N64 combiner compiler, `asp_hle.c`), `xbox/src/game/ultra/os_shim.c` (kernel-thread libultra shim), `xbox/src/gfx/f3d_interp.c` (Fast3D on the CPU); Melee-X `xhw_reserve_lazy`/`xhw_commit`/crash-guard commit-on-fault; Snap64-X's F3DEX2 interpreter design | the sibling trees at Melee-X `abdfd0f`, OpenCrossing `e6be908`, Wings64-X `83f1258`, wings64-dc `0c384bd`, Snap64-X `43e3803` |
| Maintainer requirement (2026-10-16) | **the tool works on Windows, macOS and Linux and is really easy to use** | this session |
| Maintainer requirement (2026-10-16) | **up to 720p, and widescreen, for every game** (§5.4) | this session |
| Maintainer requirement (2026-10-16) | **a BACK-button menu over the running game that controls the port's settings, game-agnostic** (§5.2) | this session |
| Maintainer requirement (2026-10-16) | **a loading screen worth looking at, in the line of the siblings' "TechProGabe Presents..." splash, with safe 480i fallbacks** (§5.5) | this session |
| Maintainer requirement (2026-10-16) | **64 MB and 128 MB consoles, stock and upgraded or overclocked CPUs, all behave the same** (§3.13) | this session |
| Maintainer requirement (2026-10-16) | **`L + R + BACK + BLACK` returns to the dashboard in every game** (and `L + R + BACK + WHITE` restarts), in every state, with no per-game work (§3.9) | this session |
| The sibling splash | `xhw_splash.c` (166 lines, Melee-X's, from OpenCrossing's `xbox_splash.c`): CPU-drawn into the `XVideoSetMode(640, 480, 32)` framebuffer before pbkit, so it shows on any TV; unscii-16 at 3x; navy gradient `rgb(14, 22, 52)` → `rgb(6, 10, 28)`; fade-in 17 steps / ~500 ms; a 400x8 bar in `rgb(120, 200, 110)`; "Hold BACK for 480i (safe video)"; 2 s hold, any button skips; `xhw_splash_progress`, `xhw_splash_release` before the mode change; no `XVideoWaitForVBlank` (`pb_init` fails with -4 after it); `-DXHW_NO_SPLASH`, `-DXHW_SPLASH_MS`, `-DXHW_SPLASH_DUMP` | Wings64-X `xbox/src/hw/xhw_splash.c` |

---

## 1. Summary

**Approach: N64Recomp's output, our runtime, the siblings' platform layer.**
Four parts, in the order a user meets them:

1. **`n64rx`, the build tool** (§5.1): one command on Windows, macOS or Linux.
   `n64rx build <rom>` identifies the game from the ROM (byte order
   normalised, SHA-1 looked up in the game packs), runs the recompiler on it,
   compiles the result with the runtime for the Xbox and leaves a folder
   (`default.xbe` with the normalised ROM inside it, `default.tbn`, a
   one-page README: §3.5) that the user copies to the console. It runs inside a published Docker image
   (the same bytes on every OS) or on a native toolchain it can install;
   `n64rx doctor` says which is available and what is missing.
2. **The ROM analyser** (§3.14, maintainer 2026-10-16): **any ROM in, an XBE
   out, with no decomp and no pack.** `n64rx` finds the game's code, its
   segments and its libultra functions from the ROM itself, and learns the
   code a run loads that the analysis missed.
   **Game packs** (`games/<id>/`, §4.2) are optional: hints and fixes on top
   of the analysis, with no game data: a symbol file where one helps,
   the overlay list, stubs and instruction patches, the RSP microcode
   recipes, the save type, the memory size, the microcode families, the
   knobs, and the MIPS-C patches that change the game (widescreen culling,
   crash fixes, an in-game menu later).
3. **The runtime** (`xbox/src/rt`, §3.3-3.6): a C reimplementation of the
   libultra surface the recompiler expects (`<name>_recomp` entry points) on
   Wings64-X's kernel-thread shim design; RDRAM as a physically contiguous
   window the recompiled code indexes without arithmetic; PI DMA from a
   RAM-resident ROM or a page cache; overlay tracking and function lookup;
   saves and settings on E:. Written from the N64 programming manual's
   semantics and the siblings' shims, never from the GPL runtime.
4. **HLE for the RCP** (§3.7-3.8): the display lists go through a table-driven
   interpreter (Wings64-X's Fast3D interpreter extended with the F3DEX,
   F3DEX2/F3DZEX2 and S2DEX tables Snap64-X designed) into the Melee-X NV2A
   back end; the audio command lists go through `asp_hle.c` where the game
   uses the stock `aspMain` ABI, and through RSPRecomp's recompiled microcode
   over a scalar/MMX vector unit where it does not.

Why this and not the other ways:

- **Decomp-based ports** (Melee-X, Wings64-X, Snap64-X) are faster (native C,
  no translation overhead) and let the port change the game in its own
  style. They need a decompilation that compiles as C. Recompilation needs
  only function boundaries, which every splat-style disassembly has long
  before any function matches, so it covers games the maintainer will never
  port by hand, and it is the same pipeline for every game. The two lines
  coexist: N64Recomp-X does not replace Wings64-X or the Snap64-X plan; it
  measures itself against Wings64-X (§6 M3) and may take games Snap64-X's
  plan rejected or games no decomp exists for.
- **The upstream runtime and renderer** do not fit: RT64 needs a shader-model-6
  GPU and compute shaders (the NV2A has vertex programs and register
  combiners); `N64ModernRuntime` is GPL-3.0, C++20 with `std::thread` and
  semaphores, and shaped around a PC. The recompiler itself is MIT, portable
  C++20 for the *host*, and emits plain C: it is used unmodified at a pin.
- **Emulation** on the P3 (Surreal64) is the existence proof of what does not
  work: the RCP cannot be emulated at speed; HLE of its two jobs is what every
  working N64 port does.

What it costs, said plainly:

- **No XBE can be published**: the recompiled code is the game's code. Every
  user builds with their own ROM. The whole product is therefore the build
  tool plus the runtime, which is why the tool is a first-class deliverable
  (the maintainer's requirement) and why CI never touches a ROM.
- **Translation overhead on a 733 MHz CPU.** 64-bit MIPS registers on a
  32-bit x86 cost two instructions where one would do; the context struct
  costs loads and stores around every call. The plan's first measurement
  (M3) puts a number on it with the one game that exists both ways,
  Pilotwings 64, and the 32-bit-register mode of §3.2 is the first lever.
- **Memory.** A 64 MB console holds the recompiled code (4-6x the MIPS text),
  the N64's RAM, the ROM or its cache and the GPU's buffers at once. Games
  with 4 MB of RAM and ROMs up to 16 MB fit easily; 32-64 MB ROMs need a page
  cache and games with hundreds of overlays need their code loaded on demand
  (§3.4); Expansion Pak games are the stretch (§6 M8).

The headline facts that shape everything else:

| fact | consequence |
|---|---|
| `recomp.h` is MIT and the only contract between generated code and runtime | we write our own header: register width, memory window, float conversions, 64x64 multiply, rounding modes are ours to implement for the P3 (§3.2) |
| the recompiler replaces libultra by *name* (`reimplemented_funcs`) and emits `<name>_recomp` calls | the runtime is a fixed list of ~130 C entry points with a known calling convention (args in `ctx->r4..r7`, then `0x10(sp)`; result in `ctx->r2`) (§3.3) |
| libultra objects (`OSThread`, `OSMesgQueue`, `OSTask`) live in the game's RDRAM and game code reads their fields directly | the runtime keeps those structs in RDRAM in N64 layout and hangs host state (kernel threads, events) off side tables keyed by N64 address, unlike Wings64-X's shim, which shares C structs with the game (§3.3) |
| overlays are recognised by the ROM address a PI DMA reads from (the upstream idea), function pointers by a vram lookup | the runtime tracks sections by DMA source and keeps a two-level vram table; with on-demand XBE sections the code of an overlay is in RAM only while the game has it loaded (§3.4) |
| the generated code is compiled by us | the game triple, ThinLTO, `-fno-strict-aliasing`, and a per-section output grouping are build decisions, not recompiler changes; a recompiler fork is only needed for codegen changes that M3 proves worthwhile (§3.2) |
| the Xbox kernel loads XBE sections on demand and `cxbe` preloads all of them | one post-link bit per section turns an overlay's code into a demand-loaded section; no loader of our own (§3.4) |
| the tool must be easy on three OSes | Docker image first (identical everywhere, Apple Silicon included through a multi-arch image), native toolchains second, a `doctor` command, per-OS single-file binaries of the CLI, a tri-OS CI matrix (§5.1, §6 M0, M7) |
| 720p, widescreen and a BACK menu are goals for every game, not per-game features | the video rules, the 16:9 mapping and the settings menu live in the platform layer and the interpreter; the only per-game piece is the culling fix that stops edge pop-in with hor+ (§5.2, §5.4); 1080i is not offered |
| the loading screen is part of the product | built on `xhw_splash.c`, CPU-drawn in the dashboard's mode so it shows on any TV: the family's wordmark and palette, an animated background, the game card, real stage progress, a video line that says which mode is coming and how to hold BACK for 480i, a bridge frame across the mode switch, and four ways back to a picture (§5.5) |
| the exit combos belong to the platform, not the game | `L + R + BACK + BLACK` (dashboard) and `L + R + BACK + WHITE` (restart) are read from the raw pad state before any N64 mapping, at the game's reads, from the VI thread when the game stops reading and from the watchdog as the last resort, so they work in every game and every state and no pack can lose them (§3.9) |

---

## 2. Research findings

Full detail with file paths: `docs/research.md`. The table says what each
source gives this project and under what terms.

| source | what it is | what we reuse | licence | use? |
|---|---|---|---|---|
| `N64Recomp/N64Recomp` @ `ffb39cd` | MIPS→C static recompiler; RSPRecomp; mod tools; live recompiler (x86-64/ARM64 only) | the recompiler and RSPRecomp **unmodified at the pin**, run on the host at build time; `include/recomp.h` as the specification of the generated code's contract (replaced by ours); the symbol-file format; `--dump-context` | MIT | yes |
| `N64Recomp/N64ModernRuntime` @ `cdf5abb` | `ultramodern` + `librecomp`: the PC runtime for recompiled games | **knowledge only**: the list of `<name>_recomp` entry points and their argument conventions, the "structs live in RDRAM" rule, overlay tracking by DMA source, the RSP task/DMEM model, the save types | GPL-3.0 | no code |
| `Zelda64Recomp/Zelda64Recomp` @ `b65c482` | Majora's Mask recomp | knowledge only: how a game is configured (`us.rev1.toml`: symbol file, overlay list, stubs, single-instruction patches such as rewriting a KSEG1 `lui` to KSEG0), how patches are built (clang `-target mips -mips2 -mabi=32 -O2 -G0 -mno-abicalls`, lld `--emit-relocs`, `single_file_output`), that compressed games are recompiled from a decompressed image, which microcodes needed recompiling | GPL-3.0 | no code; the patch *mechanism* is upstream's MIT tooling, the patch *contents* are theirs |
| `Zelda64Recomp/Zelda64RecompSyms` | MM symbol files generated from the `zeldaret/mm` decomp | the format; a data point on size (81,965 lines, 17,765 functions) | **no licence file** | not committed here; §8 Q7 |
| Ares (via `librecomp/rsp_vu*.hpp`) | the RSP vector unit, SSE4.1 or scalar | the scalar reference as a test oracle and a model for the MMX/scalar VU of §3.8 | ISC (header) inside a GPL repo | from Ares directly, with attribution |
| `TechProGabe/Wings64-X` @ `83f1258` | Pilotwings 64 on the Xbox, from the decomp | `xbox/src/hw/*` as is (boot, video, pad, AC97, clock, RAM lock, crash/hang, profiler, fbdump, autopad, `nv2a*.c`, `asp_hle.c`) and `xhw_splash.c` as the loading screen's base (§5.5); the libultra shim's design and most of its code (`os_shim.c`: levels, lock, queues, VI thread, synchronous tasks, virtual DAC, EEPROM); `f3d_interp.c` + `texconv.c`; `xhw_rom.c` (find, order, header, SHA-1); `settings*.c`; build (`compile_game.py`, `thinlto_link.py`, CMake, MSYS2 and Docker wrappers); console and xemu tooling; the game as the **A/B reference** | MIT (the maintainer's) | yes |
| `TechProGabe/Melee-X` @ `abdfd0f` | the origin of the platform layer | `xhw_reserve_lazy`/`xhw_commit`/crash-guard commit-on-fault (the memory window of §3.2), the EFB-copy readback machinery, `settings.c`/`menu.c` semantics, PGO and order-file machinery, `docs/testing.md` method | GPL-3.0-or-later on paper because of melee-pc; the maintainer relicenses the Xbox files as their author (Wings64-X's position) | yes |
| `TechProGabe/Snap64-X` @ `43e3803` | plan for Pokémon Snap from the decomp | the F3DEX2 interpreter design (§3.4 there), the readback rules, the memory-budget method, the plan shape | the maintainer's | yes, as design |
| `TechProGabe/OpenCrossing-Xbox` @ `e6be908` | Animal Crossing on the Xbox | `traps.md` (clock, aliasing, rdtsc rewrite), `xbox_ramlock.c`, `xbox_code_repair.c`, the video rules | MIT | yes |
| `sm64-port` `gfx_pc.c` family, libultraship `src/fast` | CPU Fast3D-family interpreters | the opcode tables for F3DEX/F3DEX2/S2DEX (libultraship's copy is plain MIT) | MIT (libultraship) | yes, as reference |
| community recomps (Snap64Recomp, Starfox64Recomp, MarioKart64Recomp, BanjoRecomp, DK64, Goemon64Recomp, HarvestMoon64Recomp, the announced Smash 64 recomp) | N64Recomp + N64ModernRuntime + RT64 ports | which games have symbol files and what each needed stubbed or patched (read per game when its pack is made) | GPL-3.0 (N64ModernRuntime forces it) | knowledge only |
| `gcsmith/Pilotwings64Recomp` | listed by search engines as "static recompilation of Pilotwings 64 (WIP)" | nothing: the repository was not reachable on 2026-10-16 (404; the author's profile lists a Superman 64 decomp instead) | unknown | no |
| nxdk @ `14d5ee9` | the SDK | `XeLoadSection`/`XeUnloadSection`, `PXBE_SECTION_HEADER`, `cxbe`'s section handling (preload bit set on every section), kernel thread and memory calls | nxdk's | yes (at Melee-X's pin for the build; the clone was read for the API) |
| Surreal64 CE | emulation on the Xbox | the lesson only | n/a | no |

---

## 3. Architecture

### 3.1 Layering

```
user's ROM (inside default.xbe on the console, §3.5; given to n64rx on the PC)
        |
games/<id>/              game pack: game.toml (SHA-1s, title ID, save type, RDRAM size, ucode families,
                         audio path, knobs), <id>.syms.toml (+ datasyms), overlays.txt, recomp.toml,
                         rsp/*.toml, patches/ (MIPS C + patches.toml), icon.png (original art)
tools/recomp/            N64Recomp at the pin (submodule) + patches/recomp/ if ever needed
        |  n64rx: normalise ROM -> SHA-1 -> pack -> N64Recomp (+ RSPRecomp) -> build-xbox/<id>/gen/*.c
        v
xbox/include/xrc/recomp.h   OUR header: gpr width (64 or 32), MEM macros on the RDRAM window, FP conversions,
                            64x64 multiply, rounding, LOOKUP_FUNC, RELOC_*; the generated code includes nothing else
xbox/src/rt/   (game triple)  the runtime: ultra/ (the <name>_recomp entry points over the shim: threads, queues,
                              events, timers, time, VI, SP, DP, AI, PI, SI, EEPROM/SRAM/Flash, PFS, motor),
                              mem.c (window, boot block, fault policy), pi.c (ROM resident or page cache, PI manager),
                              ovl.c (sections, two-level vram table, XeLoadSection), save.c, vi.c, sp.c, pad.c,
                              settings.c, settings_menu.c (the BACK menu), hooks.c (recomp_trigger_event, host hooks)
xbox/src/gfx/  (game triple)  ucode registry (text hashes -> family), f3d_interp.c (table-driven: F3D, F3DEX, F3DEX2,
                              F3DZEX2, L3DEX2, S2DEX/S2DEX2), tmem.c, texconv.c, vi_map.c (VI mode -> content rect)
xbox/include/xhw.h, xgfx.h, xrom.h   the only structs crossing the triples: 32-bit scalars, floats, byte arrays
xbox/src/hw/   (nxdk triple)  Wings64-X's platform layer as is + nv2a.c, nv2a_cc.c, nv2a_fog.c, asp_hle.c,
                              rsp_vu.c (scalar/MMX vector unit for recompiled microcode), xhw_section.c (XeLoadSection)
tools/xbox/                   build (per game), compile_game.py, thinlto_link.py, syms tools, xemu runner, console.py,
                              host tests, rom_leak_check.py, package_release.py
tools/n64rx/                  the user-facing CLI (Python; packaged per OS), Docker engine, native engine, doctor
```

Two compile setups, as in every sibling: the generated code, the runtime and
the interpreter use Melee-X's **game triple** (`--target=i686-pc-windows-gnu
-mno-ms-bitfields -march=pentium3 -msse -mfpmath=sse -ffp-contract=off
-fno-strict-aliasing -fwrapv -fno-auto-import -ffunction-sections`, see
Wings64-X `docs/toolchain.md` for the whole set and the reasons), compiled by
`compile_game.py` to objects or ThinLTO bitcode; `xbox/src/hw` uses nxdk's
`i386-pc-win32`. The generated code needs `-fno-strict-aliasing` (upstream
compiles it that way: the memory macros type-pun through the RDRAM bytes) and
single-precision SSE floats (the VR4300 rounds `float` ops to single;
`-mfpmath=sse` keeps x87 excess precision out); `double` ops go through x87
at the 53-bit control word Melee-X measured (`027f`), which is IEEE double.

Every port-side departure from the game's behaviour is marked `DIVERGES:
<reason>`. Every optimisation and risky change has a kill switch and the
default is the good build. Nothing in the tree is game data or derived from a
ROM (§4.5).

### 3.2 The generated code on a Pentium III: our `recomp.h`

Upstream's header (`include/recomp.h`, 475 lines) is the specification; ours
implements the same names for this CPU. The differences, each with its reason:

| upstream | here | why |
|---|---|---|
| `typedef uint64_t gpr` | `gpr` is `uint64_t` by default and **`uint32_t` when the game pack says `gpr = 32`** (`-DXRC_GPR32=1`) | the VR4300 is 64-bit but O32 code compiled with IDO `-mips2` never executes a 64-bit-ISA instruction: every 32-bit op sign-extends into the upper half, so a 32-bit register holds the same value. `tools/xbox/recomp_scan64.py` disassembles every recompiled function with rabbitizer and lists `dadd/dsub/dsll/dsrl/dsra*/dmult*/ddiv*/ld/sd/ldl/ldr/sdl/sdr/lld/scd/dmfc1/dmtc1/cvt.*.l/trunc.l/round.l/ceil.l/floor.l/lwu`; **zero hits in game code ⇒ 32-bit registers are exact** and halve the register pressure on a CPU with seven usable GPRs. libultra's own 64-bit code (`__osException`, `osGetTime` math, `bcopy`) is replaced by the runtime and never counted. A game with hits runs 64-bit; a per-function mixed mode is recompiler work and waits for M3's numbers |
| `MEM_W(off, reg) = *(int32_t*)(rdram + ((reg + off) - 0xFFFFFFFF80000000))` | `*(int32_t*)(rdram + (uint32_t)((reg) + (off)))` with **`rdram` = the window's physical base P** | §3.2.1: the RDRAM buffer is a physically contiguous allocation (Wings64-X's `xhw_rdram_alloc`), which the kernel maps at `0x80000000 + P`; a KSEG0 address `0x80000000 + x` is then host address `P + 0x80000000 + x` = `rdram + addr`. One addressing mode, no subtraction, no mask. The `^2`/`^3` of halfword and byte accesses stay (the RDRAM image is word-swapped, as upstream, so 32-bit accesses, the common case, are native) |
| `DMULT`/`DMULTU` need `__int128` or MSVC | written out as four 32x32 multiplies | no 128-bit type on i686; emitted only for `dmult`, which `gpr = 32` games never reach |
| `do_cvt_w_s` via `lrintf`, rounding via `fesetround` | `cvtss2si`/`cvttss2si` for `float` (MXCSR rounding), x87 `fistp`/`fisttp`-free sequences for `double` (control-word rounding); `set_cop1_cs` writes MXCSR and the x87 control word together | nxdk's `lrintf` is a stub that asserts (Wings64-X found it), pdclib has no working `fenv`; the P3 has SSE1 (float) but no SSE2 (double) |
| `lroundf`/`llround` family emitted for `round.w.*` | own implementations | not in nxdk's libc |
| `NAN_CHECK` = `assert`, `CHECK_FR` = `assert` | empty in release, a logged fatal in test builds | no `assert` in the game loop |
| `RECOMP_FUNC` = `extern inline __attribute__((weak,noinline))` | the same by default; `-DXRC_FUNC_PLAIN=1` makes it a plain external function | upstream uses weak symbols so a patch library linked first replaces a function. Weak + noinline forbids ThinLTO from inlining the game's two-line helpers into their callers; a game pack without patches can drop it. Measured in M3 |
| `recomp_trigger_event`, `recomp_syscall_handler`, `do_break`, `switch_error`, `cop0_status_*`, `pause_self` | runtime functions: hooks, a fatal card with the vram, `__osDisableInt`'s interrupt bit, "block this kernel thread forever" | §3.3 |

Host test `test_recomp_h.py` compiles upstream's header and ours on the host
(x86-64 and `-m32`) and feeds every macro the same random inputs: results and
the RDRAM bytes must be identical, and `gpr = 32` must agree with `gpr = 64`
on sign-extended inputs. This is the one test that makes the header a safe
place to be clever.

#### 3.2.1 The RDRAM window and the fault policy

- `xhw_rdram_alloc(size)` (Wings64-X): one contiguous, cached allocation of
  4 MB or 8 MB (the pack's `rdram`), physical base P, host address
  `0x80000000 + P`. The runtime fills the boot block the IPL3 would have
  (`osTvType` 0x80000300 = NTSC, `osRomType`, `osRomBase`, `osResetType`,
  `osCicId`, `osVersion`, `osMemSize` 0x80000318 = 4 or 8 MB, the NMI
  buffer), copies the boot segment from ROM 0x1000 to the entrypoint (what
  upstream's `init` does with 1 MB; here the pack's `boot_size`) and calls
  the entrypoint on the first game thread.
- **KSEG1** (`0xA0000000 + x`, uncached on the N64, the same bytes) and
  **physical addresses** handed to the RCP map to host addresses
  `0xA0000000 + P + x`, which the Xbox leaves unmapped. Policy, in order:
  (1) the pack's single-instruction patches rewrite the few game sites that
  build KSEG1 addresses for CPU access, as Zelda64Recomp does for its audio
  display lists (`us.rev1.toml`); (2) addresses that reach the runtime
  (`osVirtualToPhysical`, DMA, task pointers) are masked with `0x1FFFFFFF`,
  as the N64 did; (3) `recomp_scan64.py` also reports `lui` of `0xA0xx` in
  game code so the sites are known before the first boot; (4) the crash
  guard turns a fault in that range into a `crash.log` naming the function
  and the N64 address, not a hang. Kill switch `-DXRC_MEM_MASK=1`: every
  access masked (`and` + base register), slower, for a game whose KSEG1 use
  is pervasive. Mapping the alias lazily from the fault handler (page-table
  writes; the Xbox runs in ring 0) is a recorded option, not a plan.
- **SP DMEM/IMEM, PIF, cartridge domain reads by the CPU** (`0xB0000000`:
  some games read the ROM header or anti-piracy words directly): the same
  fault policy; a pack lists the sites and patches or stubs them (upstream's
  `CIC6105_Init` stub is the model). TLB-mapped games are out of scope until
  upstream emits MIPS32 relocations for them (its README lists it as
  planned).
- The window is not lazily committed (the game touches all of it); Melee-X's
  `xhw_reserve_lazy` is used for the ROM page cache (§3.5) instead.

### 3.3 The libultra surface (`xbox/src/rt/ultra/`)

The recompiler renames every function in its `reimplemented_funcs` list to
`<name>_recomp` and never recompiles it; the runtime provides them. Upstream's
convention, kept: arguments in `ctx->r4..r7` and `MEM_W(0x10, ctx->r29)`
onwards, floats in `ctx->f12`/`f14`, results in `ctx->r2` (and `r3` for 64-bit
values, O32 style). Each entry point is a four-line shim over a C function
that takes N64 addresses (`int32_t`), as in `ultra_translation.cpp` upstream.
The functions behind them are Wings64-X's `os_shim.c` re-targeted; what changes
is the data model:

- **Structs live in RDRAM in N64 layout.** `OSMesgQueue` (`mtqueue`,
  `fullqueue`, `validCount`, `first`, `msgCount`, `msg`), `OSThread` (`id`,
  `priority`, `state`...), `OSTask`, `OSIoMesg`, `OSTimer`, `OSViMode` are read
  and written through `MEM_*` accessors, because game code reads their
  fields inline (`validCount` checks, `priority`, task headers). Host state
  (the kernel thread, its wake-up event, the wait object) is in side tables
  keyed by the struct's N64 address (`thread_of(vram)`, `queue_of(vram)`);
  Wings64-X's `Thr` records become exactly that.
- **Threads**: `osCreateThread(t, id, entry, arg, sp, pri)` records entry/arg/sp;
  `osStartThread` spawns a kernel thread (`xhw_thread_spawn`, 128 KB host
  stack) at the level `level_of(pri)` gives (Wings64-X: idle 2 ... VI 14,
  strictly priority-preemptive, which is libultra's order) whose body builds a
  fresh `recomp_context` (`r4 = arg`, `r29 = sp`, `r31` = a return trap) and
  calls `get_function(entry)(rdram, &ctx)`. The game's N64 stack is in RDRAM
  (the recompiled code pushes its frames there itself); the host stack is the
  kernel thread's. `osSetThreadPri`, `osGetThreadPri`, `osGetThreadId`,
  `osStopThread`, `osDestroyThread`, `osYieldThread` as in the shim.
  `pause_self` (emitted for a branch to itself, the idle thread's loop)
  blocks the kernel thread on its event forever: that is the Xbox idle.
- **Message queues, events, timers, time**: the shim's ring-under-one-lock
  with real blocking, the event table, the timer thread, `osGetCount`/
  `osGetTime` from the ACPI clock scaled to 46.875 MHz (never the TSC), with
  the queue fields mirrored into RDRAM after every change. `OS_EVENT_AI`
  is posted by the virtual DAC when a buffer finishes (some games wait on it;
  the shim never needed it); `OS_EVENT_PI` by the PI manager; `OS_EVENT_SP`
  and `OS_EVENT_DP` when a task returns; `OS_EVENT_SI` after a controller
  read; `OS_EVENT_COUNTER` by timers; `OS_EVENT_PRENMI` never (reset is our
  restart).
- **Interrupts**: `osSetIntMask`, `__osDisableInt`/`__osRestoreInt` and
  `cop0_status_write` keep an interrupt-enable bit per thread and take the
  recursive lock while it is clear (Melee-X's interrupt lock), so a game's
  critical section excludes the runtime's own queue updates. `__osSetFpcCsr`
  → `set_cop1_cs`.
- **PI**: `osCreatePiManager` starts the PI thread (level 12), `osPiStartDma`/
  `osEPiStartDma`/`osPiRawStartDma` queue an `OSIoMesg` it serves in priority
  order from the ROM source of §3.5 (the copy swaps bytes into the word-swapped
  image), then posts the message; `osPiReadIo`/`osEPiReadIo` read one word.
  A write to the cart domain is SRAM/Flash (§3.6), anything else is logged.
- **VI**: the VI thread posts the retrace message every `osViSetEvent` count
  (with the shim's queue-reserve DIVERGES), latches `osViSwapBuffer`, keeps
  `osViSetMode`'s mode (width, height, interlace, x/y scale) for the
  interpreter's content rect (§3.7), `osViBlack`, `osViSetSpecialFeatures`,
  `osViRepeatLine`, `osViGetCurrentFramebuffer/Next`.
- **SP/DP**: `osSpTaskStartGo` runs the task **synchronously on the calling
  thread**, as every sibling does: `M_GFXTASK` → the ucode registry picks the
  interpreter table from the task's `ucode`/`ucode_data` hash and the display
  list is drawn and presented; `M_AUDTASK` → `asp_hle` or the recompiled
  microcode (§3.8); then SP and DP done. `osSpTaskYield` is ignored and
  `osSpTaskYielded` returns 0 (upstream does the same). `osDpSetNextBuffer`,
  `osDpGetStatus`/`osDpSetStatus` report idle. One CPU means a render thread
  buys nothing; CPU/GPU overlap is the back end's (`XGFX_OVERLAP`).
- **SI**: `osContInit`/`osContStartReadData`/`osContGetReadData`/
  `osContStartQuery`/`osContGetQuery`/`osContSetCh`/`osContReset` over
  `pad.c` for four ports (§3.9); `osMotorInit/Start/Stop` and
  `__osMotorAccess` drive the pad's motors (a Rumble Pak is always present
  when the pack says so); `osPfs*` report no Controller Pak unless the pack
  asks for a 32 KB image per port.
- **EEPROM/SRAM/Flash**: §3.6. **Cache ops**: no-ops. **`osVirtualToPhysical`**:
  `& 0x1FFFFFFF`. **`osGetMemSize`**: the pack's size. **`osInitialize`**:
  the boot block and the first thread record.
- **libc/math** in `renamed_funcs` (`memcpy`, `bzero`, `sinf`, `sqrtf`,
  `sprintf`...) stay recompiled, so the game's own implementations run and no
  host libc semantics leak in; `-DXRC_NATIVE_LIBC=1` swaps in `xhw_string.c`'s
  `rep movs` versions where the profiler shows them.

Fallback if priority semantics prove load-bearing for some game: the
cooperative single-thread shim the siblings keep in reserve, same API.

### 3.4 Function lookup, overlays and code on demand (`ovl.c`, `xhw_section.c`)

- **Lookup**: `get_function(vram)` is a two-level table over the N64 address
  space at 4-byte granularity: a 4 KB page map (2,048 entries for 8 MB) of
  pointers to per-page arrays of 1,024 function indices (`uint16_t`, 65,535
  functions; MM has 17,765), allocated only for pages that contain function
  starts. O(1), two loads and a bounds check; ~1 MB for 2 MB of resident code.
  The generated `recomp_overlays.inl` (section table: `rom_addr`, `ram_addr`,
  `size`, functions with offsets, relocations) is the input; static sections
  are entered at boot.
- **Overlays**: the upstream idea, reimplemented: when the PI manager serves a
  read whose ROM range starts a relocatable section, the section is "loaded"
  at the destination address: `section_addresses[index]` is set (the
  `RELOC_HI16/LO16` macros read it) and its functions are entered into the
  table; a later read into the same RAM range or a load of a section that
  overlaps it unloads the old one. The game's own relocation of data (jump
  tables, pointers in `.data`) runs as recompiled code on RDRAM bytes and
  needs nothing from us.
- **Code on demand** (`-DXRC_OVL_SECTIONS=1`, default on when a pack's
  overlay code exceeds 4 MB of x86): the build groups each relocatable
  section's functions into their own PE section (`.ovl_<n>`; the generator's
  per-file grouping is by count, so `n64rx` splits the generated files per
  section before compiling, a text operation on `funcs_N.c` that needs no
  recompiler change), the post-link XBE pass (`xbe_title_image.py`'s
  successor, `xbe_sections.py`) clears the preload bit on them, and `ovl.c`
  calls `XeLoadSection` on first load and `XeUnloadSection` on unload. The
  kernel reads the bytes from the XBE on D: (HDD) and keeps a reference
  count; sections are page-aligned by the linker. With it, MM's resident code
  is the boot/code segments plus the overlays in use, not all 617. Without
  it (switch off, or a DVD-R user who cannot afford the seek), every section
  is preloaded and the budget of §3.10 says whether the game fits.
- A function pointer that misses the table (an overlay the game never
  DMA'd through PI, hand-rolled loaders, TLB) is a fatal card with the vram
  and the caller, logged, never a silent jump.

### 3.5 The ROM (`pi.c`, `xhw_rom.c`)

**The ROM lives inside the XBE** (maintainer, 2026-10-16: one file per game
to copy and manage; built 2026-10-16, `xbe_sections.py --rom`,
`xhw_rom.c`). `n64rx` puts the normalised
(decompressed, for Yaz0 games) image into `default.xbe` as a section of its
own, `$ROM`, that the kernel never loads (no preload flag, not in any
`XeLoadSection` call, and no virtual range: VirtualSize 0, SizeOfImage
unchanged, since the kernel builds page tables for the whole image range
and a 64 MB range would cost up to 64 KB): it costs no RAM until the
runtime reads it (xemu: `[MEM]` the same to the KB as with the ROM beside
the XBE). At boot
the runtime finds `$ROM` in its own section headers (the image header the
kernel mapped at 0x10000, `xrom_xbe_section`), reads its bytes from
`D:\default.xbe` at the section's raw offset (the same file reads the
resident load and the page cache already do, so both sources below stay
as they are), SHA-1s them against the table the build wrote, and refuses
anything else with the hash shown. The output folder is then
`default.xbe`, `default.tbn` and the README; the built XBE already holds
the game's recompiled code, so publishing it was never allowed and nothing
changes there. The fallback the boot keeps for a build without `$ROM`
(`n64rx build --rom-beside`; at run time `-DXHW_ROM_IN_XBE=0`): the ROM
next to `default.xbe`, Wings64-X §3.1
generalised (any name, `.z64/.n64/.v64/.rom/.bin`, normalised in place).
Then one of two sources:

| source | when | how |
|---|---|---|
| **resident** | normalised image ≤ `XRC_ROM_RESIDENT_MB` (16) | the whole file in RAM (Wings64-X D2); PI DMA is a swap-copy |
| **page cache** | larger ROMs (32-64 MB; decompressed Zelda images) | `xhw_reserve_lazy` of the ROM's size; 64 KB chunks filled from the file on first touch by the PI thread (never from a fault inside the kernel's file system: reads commit first, Melee-X's rule); an LRU over `XRC_ROM_CACHE_MB` (16, 12 at 720p) decommits cold chunks; the pack can pin ranges (`[rom] pin = [...]`: animation and audio tables that are read every frame) and list prefetch hints; `[PI]` counters log hits, misses, stall time per second |

Status (2026-10-16): both sources built (`xhw_rom.c`, `xhw_romcache.c`);
every reader goes through `xhw_rom_read` (`xrom.h`); pins from `[rom] pin`;
`[rom] resident` and `-DXRC_ROM_RESIDENT_MB` choose. Prefetch hints and
`dvd_cache_mb` are not built. Verified in xemu with the cache forced on
Pilotwings 64; console numbers (the HDD's seeks) are M5's.

HDD latency (~10 ms a seek) against the N64's microsecond cart latency is the
risk (§7 R6): per-frame synchronous DMAs of a few KB (Zelda's animation
frames) must hit the cache, which pins and prefetch provide; measured on the
console in M5, with the resident path as the comparison where the ROM fits.
**Compressed games** (Yaz0 in both Zeldas): the recompiler needs the
decompressed image, so `n64rx` decompresses at build time and writes the
decompressed, normalised image into the output folder as the ROM the console
reads; the known table holds the hash of the user's file and of the
decompressed image; the console's check is on the latter. With the ROM in
the XBE the decompressed image is what `$ROM` holds. DVD-R users get the
same folder on a disc (`n64rx xiso`), slower, with the page cache sized for
it (`[rom] dvd_cache_mb`).

### 3.6 Saves and settings on E:

Per game a title ID (`4E52 nnnn`, "NR" + the pack's index; §8 Q5) and its
folder `E:\UDATA\4E52nnnn\`:

- **Save image** by the pack's `save_type`: `eep4k` (512 B), `eep16k` (2 KB),
  `sram` (32 KB), `flash` (128 KB), `none`; `mempak = true` adds a 32 KB
  Controller Pak image per port. File names and byte order are the emulators'
  (`.eep`, `.sra`, `.fla`, `.mpk` as Project64 and Mupen64Plus write them),
  so saves move both ways. The PI-domain writes that SRAM/Flash games do
  (`osPiRawStartDma` to `0x08000000`, `osFlash*`) hit the image; a dirty flag
  and a writer thread flush through `xhw_write_file_atomic` within a second
  and at scene changes (Wings64-X `eeprom.c`, Snap64-X §3.6).
- **`settings.ini`** (Melee-X `settings.c`): §5.2's keys, per game.
- **Logs**: `boot.log`, `boot_prev.log`, `crash.log`, `hang.log`,
  `lastexit.txt`, `shotNN.bmp` as in every sibling; `[RECOMP]` lines add the
  pack id, the register width, the code size and the section loads.

### 3.7 Graphics: the RSP on the CPU, the RDP on the NV2A

```
recompiled game -> OSTask (M_GFXTASK) -> ucode registry: hash(ucode text) -> family table
  -> xbox/src/gfx/f3d_interp.c   one interpreter, per-family opcode tables (Wings64-X's F3D core; Snap64-X's
                                 F3DEX2 design: 32-slot vertex cache, F3DEX2 matrix semantics, G_TRI2, G_CULLDL,
                                 G_BRANCH_Z, G_LOAD_UCODE, G_LINE3D/W3D; F3DEX and F3DZEX2 variants; S2DEX/S2DEX2
                                 backgrounds, sprites and objects), segment table, lights, texgen, fog, clipping
  -> xgfx.h (XgfxState: combiner, othermode, blender, tiles, fog, scissor; canonical vertices)
  -> xbox/src/hw/nv2a.c + nv2a_cc.c + nv2a_fog.c   Melee-X's back end as Wings64-X ships it
```

- **Registry** (`ucode_registry.c`, generated from `games/*/game.toml`): the
  task's `ucode`, `ucode_size` and `ucode_data` identify the microcode; the
  registry maps a hash of the text to a family and its table. An unknown
  microcode stops with a card and the hash in the log, so a new game starts
  from a fact. Families planned, in order of need: F3D (Wings64-X, done),
  F3DEX2 and L3DEX2 (Snap64-X's design), F3DEX (the 1.x opcode numbering),
  F3DZEX2 (the Zelda variant), S2DEX2 (2D). Custom microcodes (Rare, Factor 5,
  Boss Game Studios, Acclaim) are out of scope until someone writes their
  table; libultraship's MIT tables are the reference when they are.
- **Memory the interpreter reads**: display lists, vertices, matrices,
  textures and TLUTs are RDRAM bytes in the word-swapped image; the loaders
  read words natively and halfwords/bytes with the `^2`/`^3` of §3.2, so
  `texconv.c`'s row readers change accordingly (a host test pins it).
  Segment addresses and `SETTIMG` pointers go through `& 0x1FFFFFFF`.
- **Content rect from the VI**: unlike the siblings' fixed 320x240, the N64
  resolution comes from the current `OSViMode` (320x240, 640x480 interlaced,
  letterboxed modes, `osViSetXScale/YScale`) and the swapped framebuffer's
  `SETCIMG` size; the mapping onto 640x480 / 1280x720 / pillarboxed rects is
  Melee-X's.
- **Framebuffer effects**, in the order games need them: colour image used
  as a texture (pause backgrounds, motion blur, the Deku bubble: `SETTIMG`
  pointing at a known colour image → the render target as texture, Melee-X's
  EFB-copy path), off-screen colour images (render targets, Snap64-X §3.4),
  CPU reads of a colour image (`[video] fb_readback = true` in the pack:
  the frame is copied to RDRAM after the DP task; expensive, per game), CPU
  writes into a colour image (`fb_cpu_write = true`: the dirty region is
  uploaded over the frame, Wings64-X's snow). Depth-image reads (lens of
  truth) are the known hard case: Z24 → the N64's 16-bit encoding on
  readback, measured when a game needs it.
- **Widescreen** (a goal for every game, maintainer 2026-10-16; §5.4): three
  generic pieces and one per-game piece. Generic: (1) a perspective
  projection load (`G_MTX` with the projection flag, a matrix that derives w
  from z) is widened by 3/4 on its x column, so the vertical field of view is
  unchanged and more world is visible at the sides; orthographic loads
  (menus, HUD) are left alone; (2) the viewport and scissor follow the
  content rect, and 2D drawn under an orthographic projection or by
  `TEXRECT`/`FILLRECT` lands in a centred 4:3 box unless it spans the N64
  frame (fades, backdrops, letterbox bars), which stretches to the edges;
  (3) the trivial-reject bounds scale with the drawn width (Wings64-X
  `XGFX_REJECT_RATIO`). Per game: the game's own object culling is 4:3, so
  objects can pop at the edges until the pack brings a culling patch
  (Wings64-X 030/050, Snap64Recomp's cull patch as knowledge), recorded as
  `cull_patch = true`. The menu's row offers Auto (hor+ at 16:9 output; the
  dashboard's flag at 480, anamorphic), On, Off (centred 4:3 with black
  bars) and Stretch. A pack whose projections the detection gets wrong sets
  `widescreen = "off"` as its default and says why in its README. Kill
  switches `-DXGFX_HORPLUS=0`, `-DXGFX_HUD_BOX=0`.
- **Frame rate**: the game's own. No interpolation, no decoupling at the
  framework level; a pack may patch a draw interval the way Snap64-X's plan
  does, per game.
- Kill switches: Wings64-X's `XGFX_*` set plus `-DXGFX_UCODE_FORCE=<family>`,
  `-DXGFX_S2DEX=0`, `-DXGFX_FB_TEX=0`, `-DXGFX_HORPLUS=0`.

### 3.8 Audio

Two paths, chosen per pack (`[rsp] audio = "hle" | "recomp"`):

- **`hle`** (default; Wings64-X's `asp_hle.c`, 914 lines, MMX inner loops,
  Birdman64's `abi.rs` as oracle): the game's synthesizer is recompiled game
  code that builds Acmd lists; `asp_hle` runs them for the stock `aspMain` ABI
  (ADPCM, RESAMPLE, ENVMIXER, MIXER, LOADBUFF/SAVEBUFF, CLEARBUFF, SETBUFF,
  SETVOL, LOADADPCM, SETLOOP, DMEMMOVE, INTERLEAVE, POLEF, SEGMENT). Addresses
  in the lists are N64 addresses here (`ASP_HLE_SEGMENTS 1`, the Dreamcast
  mode, over the window). Microcode *versions* differ in a few opcodes and
  flags (1.0 `aspMain`, the later `n_aspMain` with `A_ENVMIXER` state changes,
  the Zelda `naudio`): the registry's hash picks the ABI table; a hash with no
  table falls through to `recomp` when the pack has it, else stops with a card.
- **`recomp`** (M4 if G1 needs it, else M8): RSPRecomp turns the game's audio
  microcode into C (`rsp/<name>.toml` in the pack, as Zelda64Recomp's
  `aspMain.us.rev1.toml` with its `extra_indirect_branch_targets`); the
  output needs an RSP context, a 4 KB DMEM with the task at `0xFC0` and the
  ucode data DMA'd to 0 (upstream's model, reimplemented in `rsp_vu.c`), and
  the vector unit: 8 lanes of 16 bits with 48-bit accumulators, reciprocal
  and square-root tables. Upstream's VU is Ares's, SSE4.1 or scalar; ours is
  scalar C with MMX for the lane-parallel ops (the P3 has no SSE2; LLVM's MMX
  intrinsics are gone, so inline assembly as `asp_hle.c` does), tested on the
  host against Ares's scalar implementation (ISC, attributed) per
  instruction. Exact by construction, dearer than HLE: budget 5-10 % of the
  CPU at 32 kHz, measured.

Output: `osAiSetNextBuffer` → the virtual DAC (paces the game's audio clock
in wall time, posts `OS_EVENT_AI`, `osAiGetLength` = the playing buffer only)
→ `xhw_audio_write` → Melee-X's 48 kHz AC97 pump (polled, APU voice under
xemu). `osAiSetFrequency` sets the pump's resampler. Kill switch `-DXRC_AUDIO=0`.

### 3.9 Input

Four ports through `xhw_pad.c`; player N is physical port N (the Xbox has
four, like the N64; Smash and Mario Kart need all four). Wings64-X's default
layout by position (left stick → stick with the radial dead-zone row; A → A,
X → B, Y/B/White/Black and the right stick → C buttons, triggers → Z and R,
L unbound), remappable per port in `settings.ini` and in the panel's Buttons
page; rumble through the Rumble Pak emulation; the exit combos below. BACK opens the port's settings menu (§5.2). A pack can ship
its own default layout (`[input] layout = ...`) for games whose buttons mean
something else.

**The exit combos, in every game** (maintainer 2026-10-16): **`L + R + BACK +
BLACK` returns to the dashboard and `L + R + BACK + WHITE` restarts the XBE,
on any port, in every state, in every pack, with no per-game work.**
Wings64-X's mechanics, made a rule:

- **Detected on the raw Xbox state, before any N64 mapping**: a remapped
  layout cannot hide the combo and the Buttons page cannot assign it away.
  Both triggers past 200 of 255 plus BACK and BLACK (or WHITE) fire at once
  (no hold time, as Melee-X), once per press; the game sees neutral input on
  that read.
- **Three detection points cover every state**: (1) every controller read
  the game makes (`osContStartReadData`/`osContGetReadData`); (2) the VI
  thread, when the game has not read for 30 retraces (a stalled game, a
  cutscene that does not poll, the BACK menu, the loading screen's hold);
  (3) the watchdog's one-second tick as the last resort when the VI thread
  itself is stuck (`-DXRC_COMBO_WATCHDOG=1`, default on). The three share one
  try-acquire flag so SDL is never entered from two threads at once
  (Wings64-X's `WX_PAD_COMBO_WATCH`). The loading screen and the error cards
  honour the same combo.
- **What happens**: `[PAD] port N: L + R + BACK + BLACK` is logged,
  `lastexit.txt` is written ("quit to the dashboard: L + R + BACK + BLACK on
  port N", or "restart: ..."), the action runs 200 ms later once the
  controllers are free (`act()`): the audio is stopped, `xhw_pad_shutdown`
  releases the controllers, then `xhw_quit_to_dashboard` (`XLaunchXBE(NULL)`)
  or `xhw_reboot_self`. A dirty save image is flushed first (the save
  writer's flush is awaited, bounded to one second), so a quit never loses a
  save the game thought it had written.
- **Nothing in a pack can turn it off**: there is no `game.toml` key for it;
  `-DXRC_COMBOS=0` exists for diagnosis builds only. The BACK menu's "Exit to
  dashboard" and "Restart" rows do the same thing by menu; the combo is the
  way that needs no picture.
- Host test `test_pad_combo.py` feeds synthetic pad states through the
  detector: fires once per press, never on a partial combo or with a trigger
  under 200, identically with a remapped layout, and on the VI path when
  reads stop.

### 3.10 Memory budget (64 MB, enforced)

Three classes of game; the first is where the project starts.

| item | A: 4 MB RDRAM, ROM ≤ 16 MB (PW64, Smash, SF64, MK64, Snap) | B: 4 MB RDRAM, 32-64 MB ROM (OoT, Paper Mario, Kirby 64) | C: 8 MB RDRAM, 32-64 MB ROM (MM, DK64) |
|---|---|---|---|
| XBE: platform + runtime + interpreter | ~3 MB | ~3 MB | ~3 MB |
| recompiled code resident (x86 ≈ 4-6x the MIPS text; overlays on demand in B/C) | 4-8 MB (all) | 8-12 MB | 10-14 MB |
| function tables, section tables | ~1 MB | ~2 MB | ~2 MB |
| RDRAM window | 4 MB | 4 MB | 8 MB |
| ROM: resident / page cache | 8-16 MB | 12-16 MB cache | 10-12 MB cache |
| framebuffers + depth (480: 640x480x32 + Z24S8; 720p: R5G6B5 x3 + Z24S8) | 4.8 / 9.1 MB | 4.8 / 9.1 MB | 4.8 / 9.1 MB |
| texture pool | 8 / 6 MB | 8 / 6 MB | 6 / 4 MB |
| pushbuffer 1 MB + vertex ring 1.5 MB | 2.5 MB | 2.5 MB | 2.5 MB |
| audio ring, DMEM models, save images | < 1 MB | < 1 MB | < 1 MB |
| kernel, nxdk heap, 16 thread stacks, logs | ~4 MB | ~4 MB | ~4 MB |
| **total, 480 / 720p** | **40-52 / 42-54 MB** | **48-54 / 50-56 MB** | **51-57 / 53-59 MB** |

Class A fits with room at both modes. Class B fits at 480 and at 720p with
the cache at its floor. Class C is 480 first and 720p only if `[MEMB]` on the
console shows it; it is the stretch milestone for that reason, not only for
CPU time. `xhw_ramlock.c` holds everything above 64 MB on a 128 MB console
(kill switch `-DXHW_RAM_LOCK64=0`); `xhw_mem_free_kb()` gates 720p (32 MB
free rule) before `pb_init`; 720p is the top mode (§5.4) and 1080i is not
offered (Wings64-X's console run 15: it works and the frame-rate cost is too
high).

### 3.11 Performance model (a hypothesis until M3)

The VR4300 retires about one instruction per cycle at 93.75 MHz. Recompiled
MIPS costs 3-6 x86 instructions per MIPS instruction with 64-bit registers
(sign-extension pairs, the context loads and stores around calls, the
addressing of §3.2) and 2-3 with 32-bit ones; the P3 sustains 1-1.5 x86
instructions per cycle at 733 MHz. That is 1.5-4x the N64's CPU at 64 bits
and 2.5-6x at 32, before the RSP's geometry work (now on the CPU, cheap for
N64 triangle counts), the audio (3-10 %) and the back end (Melee-X's numbers:
a draw costs ~31 µs, hence the batching). A 20-30 fps CPU-bound N64 game
should hold its rate; 60 fps games and the Zeldas' heaviest scenes are the
question. **The number that settles it is the translation factor T measured
in M3**: the per-frame CPU time of recompiled Pilotwings 64 over Wings64-X's
native build at the same scenes on the console, at 64 and at 32 bits, with
ThinLTO on and off and `RECOMP_FUNC` weak and plain. T decides the game ladder
(§5.3) and whether the recompiler fork of §3.2 (per-function register
caching, a mixed-width mode) is worth doing (§6 M9).

### 3.12 Threading summary

| thread | from | level | does |
|---|---|---|---|
| game threads (idle, main, scheduler, audio, PI manager, VI manager as the game creates them) | runtime (`osCreateThread`) | `level_of(pri)`: 2 ... 14 | run recompiled code; the scheduler thread runs the interpreter and the audio path inside `osSpTaskStartGo` |
| VI thread | runtime | 14 | retrace: swap latch, retrace message, pad watch |
| PI thread | runtime (`osCreatePiManager`) | 12 | serves DMA requests from the ROM source, posts `OS_EVENT_PI` / the request's queue |
| timer thread | runtime | 14 | `osSetTimer` deadlines |
| AC97 pump | platform | 14 | 48 kHz resample, descriptors |
| save writer | runtime | low | flushes dirty save images |
| ROM prefetch | runtime (page-cache games) | low | fills chunks named by the pack's hints |
| watchdog | platform | 15 | `[BEAT]`, `hang.log`, log flush, retrace-rate check |
| profiler | platform (test builds) | time-critical | samples the game threads' EIP |

Locks block in the kernel, never spin; the one recursive lock of the shim
stands for libultra's interrupt mask.

### 3.13 Console variants: 64 MB and 128 MB, stock and upgraded CPUs

Maintainer requirement (2026-10-16): **every build behaves the same on a
stock 64 MB console, a 128 MB console, and a console with an upgraded or
overclocked CPU.** Wings64-X's and OpenCrossing-Xbox's rules, binding here:

| variant | what can go wrong | the rule | where |
|---|---|---|---|
| **128 MB** (RAM upgrade, a 128 MB-aware BIOS) | the kernel hands out pages above 64 MB: GPU addresses masked with `0x03FFFFFF` alias, allocations that fail at 64 MB succeed, so a fallback is never tested; a pack that "fits" only on 128 MB | **the console runs as 64 MB**: cxbe's "limit to 64 MB" flag, and `xhw_ram_lock64` holds every page at or above 64 MB first thing in `main_body`, before anything allocates; `[MEM]` logs what it held. The budget of §3.10 is a 64 MB budget for every pack; no pack, no knob, no cache and no opt-in ever uses the upper half: a 128 MB console uses 64 MB, for stability (maintainer, 2026-10-16; decided, not open). Kill switch `-DXHW_RAM_LOCK64=0` for diagnosis only | `xhw_ramlock.c` (M0) |
| **upgraded CPU** (1.0-1.4 GHz) or **overclocked FSB** | anything timed on the TSC or an assumed 733 MHz runs fast: game speed, audio pitch, timeouts; nxdk's `QueryPerformanceCounter` guesses the TSC rate from the multiplier | **time comes from the ACPI timer (3.375 MHz, its own crystal) and the vblank, never the TSC**: `osGetCount`/`osGetTime` are the ACPI clock scaled to 46.875 MHz, the retrace is pbkit's vblank (or 60.000 Hz from ACPI), the AI DAC drains in ACPI time; the TSC is read once at boot for the `[CLOCK]` line only. A game's own busy-waits on `osGetCount` (recompiled code) therefore wait real time on any CPU. The game's frame rate is its own (retrace messages), so a faster CPU only buys headroom | `xhw_clock.c`, `rt/ultra/time.c`, `vi.c`, `ai.c` (M0/M1) |
| **patched `rdtsc`** (modded kernels rewrite `0f 31` in loaded code) | the XBE's own code changes under it | `xhw_code_repair.c` puts `.text` back as `default.xbe` has it before the clock reads the TSC and before any game code runs; `[BOOT] code check` logs it | M0 |
| **PAL consoles / 50 Hz** | a 50 Hz retrace slows the game | the retrace is 60.000 Hz from ACPI when the output is not 60 Hz (Melee-X's pacer); 480p and 720p are NTSC-only in nxdk's tables (§5.4) | `xhw_video.c` |

Checks, every milestone from M0: the boot log's `[MEM]` and `[CLOCK]` lines
in the `docs/handoff.md` entry; xemu with 128 MB (a slot with `mem_limit =
128`) shows the RAM lock holding the upper half and the pack still inside
§3.10's numbers; on the console, when §8 Q9's hardware exists, one run on a
128 MB and one on a CPU-upgraded console per release (M6 and M7 are the
gates), with `[CLOCK] cpu N MHz` and the game's speed and audio pitch judged
against a stock console. Host test `test_time.py` (M1) feeds the time
functions a fake ACPI clock at several TSC rates and checks that nothing
moves.

### 3.14 From the ROM alone: the analyser and the learning loop

Maintainer decision (2026-10-16): **someone downloads the toolkit, gives it
their ROM, and gets an XBE of a native game; no decomp.** N64Recomp needs
three things a decomp used to give: where the functions are, which code is
loaded where (segments and overlays), and which functions are libultra (the
runtime replaces those by name, §3.3). `n64rx` derives all three from the
ROM (`tools/n64rx/n64rx_lib/analyze/`), deterministically, on the user's
machine; nothing it derives is committed.

1. **The boot image.** IPL3 copies 1 MB from ROM 0x1000 to the header's
   entrypoint; that is the first segment.
2. **Functions** (`funcs.py`): recursive descent from the entrypoint over
   `jal` targets, each function walked through its branches to its last
   reachable instruction (`jr ra`, tail `j`, jump tables read from their
   `lui`/`lw`/`jr` pattern and bounded by the `sltiu` before them); then
   every word in the data that points at a plausible function start in the
   code (thread entries, callbacks, function tables), then the gaps between
   functions that hold code. Boundaries are exactly what N64Recomp needs:
   contiguous, non-overlapping, sizes to the last instruction.
3. **libultra by behaviour** (`libultra.py`), no fingerprints: anchors that
   only libultra has (the COP0 interrupt mask: `__osDisableInt`,
   `__osRestoreInt`; `osGetCount`; `__osSetSR`/`__osGetSR`; the cache ops of
   `osInvalDCache`/`osWritebackDCache`; the hardware registers each function
   touches: VI `0xA440xxxx`, PI `0xA460xxxx`, SI `0xA480xxxx`, AI
   `0xA450xxxx`, SP `0xA404xxxx`, MI `0xA430xxxx`), then names spread along
   the call graph by shape (`osSendMesg` and `osRecvMesg` are the two
   callers of `__osEnqueueAndYield` with the queue's validCount at offset 8;
   `osCreateThread` stores the priority at 4 and the entry point in the
   context; ...). Every name carries the rule that gave it in the report.
   A function the runtime must replace and the rules cannot name stops the
   build with what was found, not a silent wrong port; a pack may name it.
4. **Segments** (`segments.py`): constant propagation over the identified
   PI DMA calls (`osPiStartDma`, `osEPiStartDma`, `osPiRawStartDma`) and over
   data tables of `{rom start, rom end, vram}` triples gives the segments
   loaded with constant addresses (most games' main code and overlay
   tables); each is analysed as in 2.
5. **The learning loop.** What static analysis cannot see (a segment whose
   address is computed) shows up at run time: the runtime logs every PI DMA
   into RDRAM (`[PI] dma`) and every jump to an address with no function
   (`[RECOMP] miss vram X`, then the card names it); `n64rx learn <log>`
   pairs a miss with the DMA that brought its bytes, adds that ROM range at
   that vram to the ROM's analysis cache (the user's build folder), and the
   next build includes it. Test builds can run the loop in xemu by
   themselves (`n64rx build --learn`): build, run the attract mode, learn,
   rebuild, until no miss.
6. **Packs on top.** A pack, when one exists, adds what analysis cannot
   know: names for the report, stubs, instruction patches, the culling
   patch, the save type when the game's own probe is not enough. Pilotwings
   64's committed symbol file (MIT decomp) is the analyser's test oracle:
   `test_analyze.py` compares the analysis of the user's own PW64 ROM with
   it when the ROM is there, and synthetic MIPS fixtures (assembled in the
   test, as `make_test_rom.py`) cover every rule on CI with no ROM.

Without a pack the game's title is the header's name, the title ID is
`0x4E52` + a hash of the game code (never `0x0001`-`0x00FF`, kept for packs),
the save type comes from what the game probes (`osEepromProbe` answers 16K
when the ROM never calls the 4K path; SRAM and Flash from the PI domain-2
accesses the analysis sees), and the icon is the family's.

---

## 4. Build system and the game packs

### 4.1 Toolchain

nxdk at Melee-X's pin `58427c078b4ccb0121f359b6b6c536b3b2914976` built with
LLVM 21.1.8, CMake + Ninja with nxdk's toolchain file; N64Recomp at `ffb39cd`
(a submodule under `tools/recomp/`, built with the host's clang or MSVC-free
toolchain; it needs only CMake and a C++20 compiler); Python ≥ 3.11 with
`rabbitizer` (for `recomp_scan64.py`) and Pillow (icons). `tools/xbox/setup.sh`
installs the native toolchain on Debian/Ubuntu, macOS (Homebrew) and Windows
(MSYS2, Wings64-X's recipe); `tools/xbox/docker/Dockerfile` builds the SDK
image `ghcr.io/techprogabe/n64recomp-x-sdk:<pin>` for **amd64 and arm64**
(Apple Silicon runs the arm64 image natively; nxdk and N64Recomp are
cross-tools and build on both). The image holds the toolchain and the
recompiler, never a game or a ROM. CI builds and pushes it when the pins
change.

Per game: `n64rx build` → `build-xbox/<id>/` (generated C, objects or bitcode,
`default.xbe`, `n64recomp_x_<id>.map`). `XBOX_CFLAGS` is reset on every run
(OpenCrossing's trap); `XBOX_LTO=1` is the default and releases refuse a
non-LTO build (`package_release.py`, Wings64-X's rule); `XBOX_LTO=0` is the
diagnosis switch.

### 4.2 Game packs (`games/<id>/`)

```
games/pw64/
  game.toml          the pack (below)
  pw64.us.syms.toml  sections and functions (from the decomp's ELF via N64Recomp --dump-context)
  overlays.txt       relocatable section names (empty for PW64)
  recomp.toml        the N64Recomp config: entrypoint, paths (filled in by n64rx), stubs, ignored,
                     [[patches.instruction]] single-word patches with the reason as a comment
  rsp/               RSPRecomp configs when audio = "recomp" (text offset, size, address, branch targets)
  patches/           MIPS C patches + patches.toml + Makefile (clang -target mips, as upstream), optional
  icon.png           original art for the dashboard (never game art)
  README.md          what the pack supports, known issues, the knobs
```

`game.toml`:

```toml
[game]
id = "pw64"                    # folder, output folder, title-ID suffix
title = "Pilotwings 64"
code = "NPWE"                  # header game code; region byte checked too
title_id = 0x4E520001
sha1 = ["ec771aedf54ee1b214c25404fb4ec51cfd43191a"]   # the user's file (normalised); plus the decompressed image for Yaz0 games
rom_size = 8388608
[rom]
compressed = "none"            # none | yaz0-oot | yaz0-mm: n64rx decompresses at build time
resident = "auto"              # auto | always | never (page cache)
pin = []                       # ROM ranges kept in the cache
[cpu]
entrypoint = 0x80000400
boot_size = 0x100000
rdram_mb = 4                   # 8 for Expansion Pak games
gpr = "auto"                   # auto (recomp_scan64 decides) | 32 | 64
[rsp]
gfx = ["F3D"]                  # families the registry must know; hashes are added by the first run's log
audio = "hle"                  # hle | recomp
[save]
type = "eep4k"                 # eep4k | eep16k | sram | flash | none
mempak = false
[video]
widescreen = "auto"            # auto (generic hor+ at 16:9 output) | off (the pack found the detection wrong)
cull_patch = false             # true once patches/ widens the game's own culling (no edge pop-in)
fb_readback = false
fb_cpu_write = false
pause = "retrace"              # retrace (the BACK menu freezes the game by holding the retrace) | none
[input]
layout = "default"
[status]
stage = "planned"              # planned | boots | playable | complete; shown by n64rx games
```

Since §3.14 a pack is optional: the analyser makes everything above but the
patches, so a pack holds only hints and fixes. A pack holds addresses,
names, sizes and hashes: facts about the ROM and names from a decomp. It
holds no bytes of the ROM, no extracted asset, no screenshot. Symbol files from an **MIT decomp** (PW64) are committed; symbol
files derived from an unlicensed decomp are generated on the user's machine
by `n64rx` from a pinned decomp checkout or taken from a public symbol repo
the user fetches, per §8 Q7.

### 4.3 The pipeline, step by step (what `n64rx build` runs)

1. **ROM**: read, normalise byte order (`80 37 12 40` / `37 80 40 12` /
   `40 12 37 80`), parse the header, SHA-1, match a pack (or say which game it
   is and that no pack exists). Yaz0 games: decompress (the Zelda table
   formats, in Python) and hash again.
2. **Recompile**: write `recomp.toml` with the paths filled in, run
   `N64Recomp` (and `RSPRecomp` per `rsp/*.toml`), under a cache keyed by
   (ROM SHA-1, pack hash, recompiler pin) so a rebuild with the same inputs
   skips the step; run `recomp_scan64.py` → the register width (`auto`) and
   the KSEG1 report into the build log.
3. **Split by section** (`XRC_OVL_SECTIONS`): regroup `funcs_N.c` into
   `sec_<n>.c` per relocatable section with a `section(".ovl_<n>")` attribute
   wrapper, static sections untouched.
4. **Compile**: `compile_game.py` over the generated C + `xbox/src/rt` +
   `xbox/src/gfx` with the game triple (bitcode under LTO); CMake builds
   `xbox/src/hw` and pbkit (patched by `patch_pbkit.py`) with nxdk's triple;
   `thinlto_link.py` links; `cxbe`; `xbe_sections.py` sets the title ID, the
   icon, clears the preload bit on `.ovl_*`; `default.tbn`.
5. **Output**: `out/<Title>/default.xbe`, `default.tbn`, the normalised (and
   decompressed) ROM under the pack's name, `README.txt` (copy this folder to
   `E:\Games\<Title>\` or `F:\...`, controls, the BACK panel, the safe-video
   hold), the link map kept under `out/<Title>/debug/` for `sym.py`.

Steps 2-4 run in the SDK container by default (the ROM and the output folder
are the only mounts) or natively when `n64rx` finds the toolchain.

### 4.4 LTO

Melee-X's machinery through Wings64-X verbatim: `XBOX_LTO=1` → game-triple
bitcode, `thinlto_link.py` (`-thinlto-index-only`, backends per module with the
game triple's codegen flags, native objects linked as usual),
`-import-instr-limit=10`, `-opt:noicf` (the game compares function pointers,
and `get_function` tables must not fold), `-stack:1048576`, the map file.
Rules kept: never LTO the two triples together; `-fno-strict-aliasing
-fwrapv` stay under LTO; no `-ffast-math`; the same float flags in the
backends as at compile time. New here: the generated code is tens of
thousands of small functions; `RECOMP_FUNC`'s weak+noinline blocks the inlining
ThinLTO is for (§3.2), so M3 measures plain vs weak and the game packs pick.
PGO (`XBOX_PGO`) rides on the same path and is per game (M9).

### 4.5 Nothing derived from a ROM in the tree

`tools/xbox/rom_leak_check.py` (CI) fails on: any file containing an N64 ROM
header word, any `.z64/.n64/.v64/.rom/.eep/.sra/.fla/.mpk/.xbe/.xiso/.iso/.bmp`,
anything under `build-xbox/`, `out/`, `external/`, `gen/`, `RecompiledFuncs/`,
`rsp/*.c`, and any C file containing `recomp_context* ctx` outside
`xbox/src/rt` and `tests/` (the generated code is ROM-derived and must never
be committed). `.gitignore` carries the same list. Screenshots of game frames
stay out of the tree (Wings64-X's rule).

### 4.6 Host tests (no ROM, CI on three OSes)

| test | checks |
|---|---|
| `test_recomp_h.py` | our `recomp.h` vs upstream's: every macro on random inputs, both register widths, the RDRAM byte layout, rounding modes, `DMULT` vs `__int128` on the host |
| `test_rom_check.py` | byte orders, header parsing, the known table, verdicts and messages; Yaz0 decompression on a synthetic table |
| `test_syms.py` | a syms TOML parses, sections and functions do not overlap, the two-level table built from it finds every function and nothing else |
| `test_scan64.py` | `recomp_scan64.py` on hand-assembled functions: 64-bit ISA hits, KSEG1 `lui` report |
| `test_ucode_hash.py` | the registry: known hashes map to families; a synthetic unknown is reported |
| `test_f3d.py` | the interpreter on synthetic display lists per family: vertex cache sizes, matrix semantics, `G_TRI2`, `G_LOAD_UCODE`, S2DEX rectangles, the word-swapped readers |
| `test_cc.py`, `test_texconv.py`, `test_asp_hle.py` | Wings64-X's, with the word-swapped memory model |
| `test_vu.py` | the scalar/MMX vector unit vs Ares's scalar reference per instruction (M4/M8) |
| `test_settings.py` | `settings.ini` round trip per game |
| `test_pad_combo.py` | the exit-combo detector (§3.9): once per press, never partial, remap-proof, the VI path when reads stop |
| `test_n64rx.py` | the CLI on a synthetic ROM and a fake pack: detection, messages, cache keys, output folder layout, `doctor` on a machine without Docker |
| `rom_leak_check.py` | the tree |

CI (`.github/workflows/build.yml`, matrix `ubuntu-latest`, `macos-latest`,
`windows-latest`): the native toolchain from `setup.sh` (cached), N64Recomp
built, the platform code and runtime compiled without a game, the host tests,
and a **ROM-free end-to-end run**: a tiny libdragon-built test ROM (MIT; it
does not use libultra, so it exercises the recompiler, the header, the window
and the build, not the OS layer) goes through `n64rx build` to a linking XBE.
A fourth job builds and pushes the multi-arch SDK image. No job ever has a
game ROM.

---

## 5. Feature specs

### 5.1 `n64rx`: one command on Windows, macOS and Linux

The maintainer's requirement of 2026-10-16. What "really easy" means here:

- **Three steps for a user**: install Docker Desktop (or let `n64rx doctor`
  install the native toolchain), run `n64rx build "Pilotwings 64 (USA).z64"`,
  copy the folder to the Xbox. No flags, no editing, no decomp, no IDO.
  `n64rx` is shipped as a single file per OS (`n64rx.exe`, `n64rx` for macOS
  universal and Linux x86-64/arm64; PyInstaller from `tools/n64rx/`) and as
  the Python source for people who have Python.
- **Commands**: `build <rom> [--out DIR] [--engine docker|native|auto]
  [--video auto|480|720] [--fast]`; `doctor` (what is installed, what is
  missing, the one command that fixes it per OS); `games` (packs and their
  status); `deploy <folder> --ftp HOST [--user xbox --pass xbox]` (the
  console tooling: `E:\Games\<Title>\` or the drive the user names, logs
  pulled first); `xiso <folder>` (a DVD-R image with the folder, via
  extract-xiso); `clean`; `log` (shows the last build's log and, after a
  deploy, pulls `boot.log` and `crash.log` symbolised with the kept map).
  Dropping a ROM onto `n64rx.exe` is `build` (argv[1]); macOS ships a
  `.command` wrapper for the same.
- **Engines**: Docker first (identical results on every OS; the image is
  pulled once; multi-arch so Apple Silicon is native speed), native second
  (`setup.sh`'s toolchain on Debian/Ubuntu, Homebrew, MSYS2); `auto` prefers
  what is present and says which it used. Windows without WSL2 and without
  MSYS2 gets the exact instructions from `doctor`, not a stack trace.
- **Feedback**: a progress line per step with times (recompiling N functions,
  compiling N files, linking), the expected duration for the game ("about
  5 minutes" for class A, "15-40 minutes" for MM on a laptop), `--fast`
  (`-O1`, no LTO) for a first look, the cache making a rebuild of the same
  inputs seconds. Errors say what to do: wrong ROM ("this is Pilotwings 64
  (Europe); the pack supports the US release, SHA-1 ..."), unsupported game
  (the game's name from the header and a pointer to `games/README.md` on how
  a pack is made), disk space, Docker not running.
- **Nothing leaves the machine**: the ROM is read from where it is, the
  output folder is the user's, the cache is local (`~/.n64rx/` or
  `%LOCALAPPDATA%\n64rx\`), and the tool never uploads anything. The README
  says so.
- A GUI (a window with a drop target and a log) is §8 Q4; the CLI's functions
  are a library so one can be added without touching the pipeline.
  Answered 2026-10-18: the desktop app (`app/`, `docs/app.md`), portable on
  all three OSes, with a prebuilt toolchain download instead of Docker
  (`docs/decisions.md`).

### 5.2 The BACK menu: the port's settings, game-agnostic

Maintainer decision (2026-10-16): **BACK opens a menu over the running game
that controls the port's settings, in every game, with no per-game work.**
The siblings put their settings in the game's own menus, which takes the
decomp or a MIPS-C patch per game; here the menu is the platform's
(Zelda64Recomp's menu is likewise the port's, not the game's), so a pack gets
it the day it boots. It is drawn by the platform (`xhw_overlay.c`'s
successor, `settings_menu.c` in `xbox/src/rt`) over the frozen frame in the
splash's dress: a navy panel, unscii-16 at 2x (the splash's own font), and
Melee-X's menu semantics (`*` rows need a restart, "720p -> 480i" when the
dashboard disallows, held-direction repeat by retrace count, A keeps, B
cancels, the stick's live readout on the dead-zone row). While it is open the
VI thread stops posting retrace messages, so the game waits where it would
wait for the retrace; controller reads return neutral; the audio ring drains
to silence (`DIVERGES`: a pause the N64 did not have). A game that does not
block on the retrace keeps running underneath; the menu still works and the
pack says `pause = "none"`. BACK on any port opens it and closes it; a pack
can move the opener (BACK + START) for a game that uses BACK's position for
something else. Rows:

| row | values | applies | key |
|---|---|---|---|
| Video output | Auto, 480i, 480p, 720p; shown as "Auto, 720p" / "720p -> 480i" when the dashboard disallows | `*` | `[video] output`, `progressive` |
| Widescreen | Auto, On, Off, Stretch (§5.4); shown as "Auto, 16:9" / "Auto, 4:3" | at once | `[video] widescreen` |
| Texture filter | N64 (bilinear), Sharp | at once | `[video] texture_filter` |
| FPS counter | On, Off | at once | `[video] fps` |
| Volume | 0-100 | at once | `[audio] volume` |
| Stick dead zone | 0-60 % in fives, live readout, per port | at once | `[portN] stick_deadzone` |
| Controller Pak | per port; A opens the **Paks page**: what is in the port (Auto, Controller Pak, Rumble Pak, Nothing), which of the game's four paks (A-D; taking another port's pak swaps the two), the pak's notes (name, pages; A deletes one, X copies one to another of the game's paks, after a confirm), Format pak (after a confirm), Back. A change is a hot-swap the game sees (the status's pull bit, a new pak ID) | at once | `[portN] pak`, `pak_file` |
| Controls | per port; A opens the **Controls page**: the Xbox controller drawn, each input's callout naming the N64 control it gives; Up / Down pick an input, Left / Right what it gives, Y = default, the live pad lit up (decisions.md "The Controls page") | at once | `[portN] a = a ...` |
| Rumble | On, Off | at once | `[input] rumble` |
| Save slot | 1-4 (the image file swapped at boot) | `*` | `[save] slot` |
| Screenshots (BACK+WHITE) | On, Off; test builds | at once | `[system] screenshots` |
| Restart | relaunches the XBE (`xhw_reboot_self`); also the "Save and restart" of a changed `*` row | | |
| Exit to dashboard | writes `lastexit.txt`, quits (`xhw_quit_to_dashboard`); `L + R + BACK + BLACK` does the same from anywhere, menu or no menu (§3.9) | | |
| Return | saves `settings.ini` when something changed (B too) | | |

Stored per game in `E:\UDATA\<title id>\settings.ini` (Melee-X `settings.c`:
defaults written on first boot, atomic replace, unknown keys dropped). BACK
held at boot = safe video (480i 4:3, saved). The menu is the same in every
pack, which is the point: a user who learnt it in one game knows it in all of
them. A pack that later adds a game-style menu through a MIPS-C patch adds
it; the BACK menu stays as it is, never replaced, so the port's settings are
reachable the same way everywhere. Kill switch `-DXRC_MENU=0` (the ini still
works).

### 5.3 The game ladder

Which games, in what order, and why (facts and "verify" marks in
`docs/research.md` §6). Where each game stands is its tier in
`docs/compat.md` (maintainer, 2026-10-18): **Gold** fully working on a
real Xbox, **Silver** playable there with bugs that are not
game-breaking, **Experimental** everything else; only a console run
promotes. A rung is done when its game is Gold.

| rung | game | class | why it is where it is |
|---|---|---|---|
| **G0** | Pilotwings 64 (US `NPWE`) | A | the A/B: Wings64-X runs it natively on the same platform layer, so every difference is the recompiler's; Fast3D and `asp_hle` already exist; 8 MB ROM, 4 MB RAM, EEPROM; the decomp is MIT so its symbol file is committed; the decomp's ELF comes from its own IDO build, done once by the maintainer in Docker |
| **G1** (maintainer's pick, §8 Q1; default **Super Smash Bros.** `NALE`) | the first game that only exists here | A | 16 MB ROM, 4 MB, four players (the Xbox's four ports), a complete decomp for the symbol file (ssb-decomp-re; status and licence to verify), a recomp announced in October 2026 whose stubs and patches will be public knowledge; thematically next to Melee-X. Alternates: Star Fox 64 (`NFXE`, 12 MB, F3DEX, custom audio microcode likely), Mario Kart 64 (`NKTE`, 12 MB, F3DEX, four players) |
| **G2** | Ocarina of Time (US 1.0/1.2) | B | the big-ROM machinery: Yaz0 decompression, the page cache with pins, hundreds of overlays as on-demand sections, F3DZEX2, SRAM; 4 MB RAM keeps the budget sane; a complete decomp and the upstream syms format; Zelda64Recomp's OoT work as knowledge |
| **G3** (stretch) | Majora's Mask (`NZSE`) | C | 8 MB RAM, 17,765 functions, 617 sections, two RSP microcodes (`naudio` + the JPEG decoder) → the `recomp` audio path and the vector unit; the memory budget's edge |
| open | Pokémon Snap | A | Snap64-X plans it from the decomp; §8 Q2 asks whether Snap moves here instead (F3DEX2 + L3DEX2 and the readbacks are the same work either way; recomp costs speed, decomp costs the asset pipeline and a licence-free vendoring) |
| later | Banjo-Kazooie, DK64, Kirby 64, Paper Mario, Goemon, Harvest Moon 64 | A/B/C | each has a recomp or a decomp; custom microcodes (Rare) need interpreter tables and the `recomp` audio path |

### 5.4 Up to 720p, and widescreen

Maintainer decision (2026-10-16): **up to 720p, and widescreen, for every
game.** Wings64-X's video rules, with 1080i left out (its console run 15
measured the mode as working and too slow for the frame rate; nothing here
changes that trade, and the memory of §3.10 is better spent on the ROM cache
and the texture pool).

| tier | framebuffer | aspect | when |
|---|---|---|---|
| 480i | 640x480x32 + Z24S8 | 4:3, or 16:9 anamorphic when Widescreen is On | always available; the safe mode (L + R held at boot) |
| 480p | same | same | HDTV/VGA pack, the dashboard's 480p flag, NTSC |
| **720p** | 1280x720 R5G6B5 + Z24S8 | 16:9 | HDTV/VGA pack, the dashboard's 720p flag, ≥ 32 MB free before `pb_init`; **Auto's choice where allowed** |

Defaults: `output = auto` (720p where the dashboard allows it, else 480p,
else 480i) and `widescreen = auto`. The ways back to a picture are §5.5's
four: L + R held during the loading screen (480i 4:3, saved), L + R + START
(the same plus default settings), a `pb_init` or allocation failure falling
to 480, and a mode not yet confirmed on this console on probation
(`video_pending.txt`, "New video mode 720p. Press A to keep it.", 15 s,
restart into 480i otherwise). The N64's picture, whatever the VI mode
says (320x240, 640x480 interlaced, letterboxed modes, `osViSetXScale`), maps
onto the content rect: all of 640x480 at 4:3, all of 1280x720 with hor+, a
centred 960x720 box with black bars when Widescreen is Off, the whole frame
with Stretch. What widescreen does mechanically is §3.7: the projection
widened by 3/4 for 3D, 2D in the 4:3 box, full-frame 2D stretched, the
trivial reject following the width, and the one per-game piece, the culling
patch, without which objects pop at the edges. Cost at 720p is the budget of
§3.10 (frame buffers 9.1 MB, texture pool 6 MB) and fill rate, measured per
pack on the console with `[PERF]`; a pack that cannot hold its rate at 720p
says so in its README and the menu still offers the mode and the 480p
alternative. Order of work in a pack: Off and Stretch cost nothing; Auto is
right for most 3D on the first day; edge pop-in is the listed defect until
the culling patch lands.

### 5.5 The loading screen, and the ways back to a picture

> **Changed 2026-10-18 (maintainer):** the screen is now the "deep space
> title card" (mockup A2, approved, then its console render approved:
> `docs/look/loading-space-host.png`): a cached starfield, nebula and
> planet limb, the game's title in Sora (SIL OFL) with the ROM's chips
> (code, region, revision; size and place; SHA-1 verified), "step n of 4"
> over a slim bar, nothing popping in (fades, an eased bar). "TechProGabe
> Presents" stays as a small label; the app's logo is never shown as if it
> were the game's. The holds swapped: **L + R** (both triggers) held is
> safe video, **L + R + START** safe video and defaults, **BACK** held opens
> the BACK menu before the game starts (the same menu, drawn over the
> frozen screen; a video change is used at once). The text below is the
> first version; where it differs, `docs/decisions.md` "The deep-space
> title card" and `xhw_splash_draw.c` are current.

Maintainer request (2026-10-16): **a loading screen worth looking at, in the
line of the siblings' "TechProGabe Presents..." splash, with safe 480i
fallbacks.** The base is Melee-X's `xhw_splash.c` (§0 has its facts). It is
drawn by the CPU into the framebuffer `XVideoSetMode(640, 480, 32)` returns,
before pbkit exists, in the dashboard's own mode, which is why it shows on
every TV and why it is the right place for the safety story. Everything
below keeps its lineage (the wordmark, the font, the palette, the BACK line,
the timings, the switches) and adds to it. Nothing on it is game art.

**What it shows**, top to bottom, at 640x480:

1. **Background**: the navy gradient as the base, with a slow aurora over
   it: three soft colour bands inside the family's palette (navy, teal,
   violet) drifting diagonally, computed at 160x120 from a sine table and
   upscaled 4x with a 2x2 smoothing step, at ~30 fps. Cost: ~20k samples and
   one 1.2 MB write of the framebuffer per frame, sequential rows (the
   framebuffer is write-combined), nothing for the P3. A sparse field of
   slow rising sparks (at most 48) over it. `-DXHW_SPLASH_STILL=1` freezes
   both, for xemu shot comparisons.
2. **The wordmark** "TechProGabe Presents..." at 3x in unscii-16, fading in
   as today, then one light sweep (a brighter diagonal band crossing the
   glyphs over ~700 ms), then settled with a 1-px shadow so it stays legible
   over the moving background.
3. **The game card**: the pack's icon (original art, `games/<id>/icon.png`,
   64x64, baked into a C array at build time) and the pack's title
   ("Pilotwings 64"), with "N64Recomp-X <version>" small at the bottom
   right. Until the ROM is identified the card reads "Looking for a ROM...";
   a ROM no pack knows shows the header's name and "no pack for this game",
   then the error card.
4. **The stage line and the bar**, with real progress behind them:
   "Reading ROM (8 MB)" (bytes read), "Checking the ROM (SHA-1)" (chunks
   hashed), "Preparing memory" (the window, the tables), "Loading code" (the
   section table, pinned ROM ranges, the page cache's first fills),
   "Starting <title>". The bar keeps the sibling's green, with a soft
   gradient and a moving highlight. Test builds print the elapsed time per
   stage at the right and log `[BOOT] stage <name> <ms>`.
5. **The video line**, the new part of the safety story: it says what is
   about to happen and how to stop it. "Video: 720p 16:9 (the dashboard
   allows 720p). Hold BACK for 480i (safe video)." While BACK is held it
   brightens and reads "Safe video: this boot runs 480i 4:3 and saves it";
   with BACK + START, "Safe video and default settings". When the coming
   mode is unconfirmed on this console it adds "First time at 720p here:
   after the switch, press A within 15 s to keep it."

**Timing.** The minimum hold stays `XHW_SPLASH_MS` (2,000 ms); the work
(the ROM read, 0.5-3 s from the HDD; SHA-1 at ~0.2 s per 8 MB on the P3;
the tables) usually covers it. Any button skips the hold, never the work. A
splash thread (level 3, started by `xhw_splash_show`, joined by
`xhw_splash_release`) animates while the boot thread works; stage and
progress are shared state, as `xhw_splash_progress` is today. No sound: the
AC97 pump starts after the mode switch, and a chime there would delay the
boot for nothing.

**The bridge across the mode switch** (`-DXHW_SPLASH_BRIDGE=1`, default on):
`xhw_splash_release` keeps a copy of the last frame (640x480x32, 1.2 MB);
after `pb_init` the back end draws it as one textured quad scaled into the
new mode's content rect, with the stage line still updating ("Starting
Pilotwings 64...", or "720p unavailable, using 480p" after a fallback),
until the game's first present; then the texture is freed. One continuous
picture from power-on to the game's first frame, at 480 and at 720p alike,
instead of the siblings' black gap. Off: black between, as the siblings.

**The ways back to a picture** (the siblings' three nets plus one; all
logged):

| # | when | what happens |
|---|---|---|
| 1 | L + R (both triggers; BACK before 2026-10-18) held while the loading screen shows (the controllers are opened before it, Melee-X's order; without a splash the boot still waits 1.5 s for them, OpenCrossing's rule) | this boot runs 480i 4:3 and saves `output = 480i`, `progressive = 0`, `widescreen = off`; the menu's Video output row goes back to Auto later |
| 2 | L + R + START held (BACK + START before 2026-10-18) | as 1, and `settings.ini` is rewritten with the defaults, so a bad binding, dead zone or volume cannot lock a user out; `[SETTINGS] reset at boot` |
| 3 | `pb_init` or a contiguous allocation fails at 720p | `xhw_video_fallback_480`: 480p where allowed, else 480i; the bridge frame says so; never a black screen because of memory |
| 4 | a mode not yet confirmed on this console | `video_pending.txt` is written before `XVideoSetMode`; the first frames carry "New video mode 720p. Press A to keep it. Back to 480i in 15 s." as CPU text (`xhw_overlay.c`); A confirms and saves `video_confirmed`; no A → restart into 480i, saved; a reset or power-off with the file present → 480i next boot. 480i is never on probation |

The error cards (no ROM, wrong size, bad header, not a supported game, bad
dump with both hashes shown, E: not writable) are drawn in the same dress by
`xhw_error_card` with "Press any button or wait to return to the dashboard"
(30 s), and `lastexit.txt` names the reason for the next boot's log.
`L + R + BACK + BLACK` works on the loading screen and the cards as it does
in the game (§3.9).

Kill switches: `-DXHW_NO_SPLASH` (straight to the mode switch), `-DXHW_SPLASH_MS=<n>`,
`-DXHW_SPLASH_STILL=1`, `-DXHW_SPLASH_BRIDGE=0`, `-DXHW_SPLASH_DUMP`. Host
test `test_splash.py` renders the screen on the host into a buffer (the
drawing code depends on nothing but the framebuffer pointer and the font)
and checks the layout: nothing outside the TV-safe area (the frame minus
32 px), the palette inside the family's, every stage label fitting its line.
The first xemu `[FBDUMP]` of M0 is the visual check, and the console the
real one (interlace flicker on the sweep and the sparks is judged there).

---

## 6. Milestones

Each milestone ends with a `default.xbe` that boots on the console (from
M1), the host tests green on the three OSes, a `docs/handoff.md` entry
(what was verified, how; "xemu" results say so) and a commit on `dev`.
"console" is the maintainer's hardware (red: AV pack 1, 480i; gold: HDTV pack, 720p, softmod with no F:)
through `console.py`/`n64rx deploy`, logs pulled before every relaunch.

**Progress (2026-10-16; `docs/handoff.md` "Where things stand").** M0 and M1
done; M2 mostly done. Built ahead of the order, for the ROM-only goal (V):
from M4 the F3DEX/F3DEX2 command sets and the graphics microcode registry
(by version text; S2DEX's own commands not yet), the audio registry with the
nead and `n_aspMain` ABIs, rumble; from M5 the ROM page cache (pins; no
prefetch hints, no PI thread), SRAM and FlashRAM, the Controller Pak; from
M6 the BACK menu's Paks page; and the TLB, the safety net with the learning
loop, `decomp_check.py`, the library sweep. Not yet: overlay tracking by DMA
source (M5's `[OVL]`), Yaz0 in `n64rx`, `XeLoadSection`, F3DZEX2, M3's
measurement gate, G1's pack.

### M0: skeleton, toolchain on three OSes, the tool's shape
- **Goal**: Wings64-X's platform layer copied (Melee parts already gone), an
  XBE that boots to the loading screen of §5.5 in its first form (the
  animated background, the wordmark, the stage line and bar, the video line
  with the BACK and BACK + START holds) and a "no game built in" card;
  N64Recomp as a submodule building on Linux, macOS and Windows; `n64rx doctor/build/games` scaffolded with both engines; the SDK
  image building for amd64 and arm64; CI matrix green with the libdragon
  test ROM through the recompiler and a linking XBE.
- **Files**: `xbox/src/hw/*` (from Wings64-X `83f1258`), `xbox/include/xhw.h`,
  `xbox/CMakeLists.txt`, `xbox/build.sh`, `tools/xbox/{setup.sh,docker/,msys/,
  compile_game.py,thinlto_link.py,patch_pbkit.py,xemu_run.sh,fbdump_to_png.py,
  console.py,sym.py,rom_leak_check.py,xbe_sections.py,render_logo.js}`,
  `tools/recomp/` (submodule at `ffb39cd`), `tools/n64rx/` (CLI, engines,
  doctor, packaging spec), `games/README.md` (how a pack is made),
  `.github/workflows/build.yml`, `docs/` skeleton (`README.md`, `handoff.md`,
  `decisions.md`, `testing.md`, `toolchain.md`, `architecture.md`).
- **Done when**: `n64rx doctor` gives the right verdict on a clean Windows,
  macOS and Linux machine (one with Docker, one without); `n64rx build
  test.z64` (the libdragon ROM) produces an XBE on each through the Docker
  engine and the native one; xemu boots the loading screen and the card
  (`[FBDUMP]` shots with `-DXHW_SPLASH_STILL=1`, kept as the look's
  baseline), BACK and BACK + START during it take effect (`[SETTINGS]`
  lines), `L + R + BACK + BLACK` on the card returns to the dashboard and
  the next boot logs `previous exit: quit to the dashboard: L + R + BACK +
  BLACK`; `test_splash.py` and `test_pad_combo.py` pass; CI is green; the
  image is on GHCR.

### M1: the runtime core on Pilotwings 64: first frame
- **Goal**: the PW64 pack (`pw64.us.syms.toml` from the decomp ELF via
  `--dump-context`, committed; `recomp.toml` with the stubs the first runs
  need); the generated code compiles under the game triple with our
  `recomp.h` at both widths (`recomp_scan64.py` says which applies to PW64);
  the window, the boot block, threads, queues, events, timers, time, PI from
  the resident ROM, VI, SP dispatch; the game reaches its first graphics task
  and Wings64-X's interpreter draws the first frame in xemu.
- **Files**: `xbox/include/xrc/recomp.h`, `xbox/src/rt/{mem.c,pi.c,ovl.c,vi.c,
  sp.c,hooks.c,ultra/*.c}`, `xbox/src/gfx/{f3d_interp.c,texconv.c,tmem.c,
  vi_map.c,ucode_registry.c}` (word-swapped readers), `tools/xbox/
  {recomp_scan64.py,syms_tools.py,gen_registry.py}`, `tests/xbox/
  test_recomp_h.c`, `test_syms.py`, `test_scan64.py`, `test_f3d.py`.
- **Done when**: `[RECOMP]` logs the pack, width and code size; `[OS]` shows
  the threads at their levels; the title's first frame matches Wings64-X's
  `[FBDUMP]` of the same frame by eye; `test_recomp_h.py` passes on the host
  at both widths; no `crash.log` through the intro.

### M2: Pilotwings 64 complete on the recompiled path
- **Goal**: title, menus, every vehicle, results, the album, EEPROM saves
  moving both ways with Wings64-X's `.eep`, audio through `asp_hle`,
  controllers and combos; **the BACK menu** with its first rows (Video
  output, Widescreen, FPS counter, Volume, Stick dead zone, Restart, Exit to
  dashboard, Return) and `settings.ini`; 480i, 480p and **720p with
  widescreen Auto** (generic hor+, the 4:3 box); the console boots it.
- **Files**: `xbox/src/rt/{save.c,pad.c,settings.c,settings_menu.c,
  widescreen.c}`, `xhw_overlay.c`, `xhw_video.c` as Wings64-X's (480/720p,
  fallback), the AI path, the `ASP_HLE_SEGMENTS 1` mode of `asp_hle.c`,
  `xhw_rom.c`'s known table from the pack, `games/pw64/README.md`; the first
  `[[patches.instruction]]` entries with reasons.
- **Done when**: Wings64-X's xemu scenarios (`title`, `rb_a`, the attract
  demos) run on this build with the same shots; the loading screen shows
  the Pilotwings 64 card with real stage progress and the bridge carries it
  across the 720p switch with no black frame; BACK opens and closes the
  menu in flight with the game frozen underneath and silent, and a row
  changed in it is in `settings.ini` after Return; `L + R + BACK + BLACK`
  from flight, from the BACK menu and during the loading screen quits to
  the dashboard and `L + R + BACK + WHITE` restarts, each logged and named
  by the next boot's `[BOOT] previous exit`; 720p shots show more world
  at the sides with the HUD box undistorted, Off shows the centred 4:3 box;
  console: the maintainer flies a mission with sound at 720p; a 30-minute
  soak logs no underrun, crash or hang.

### V: the ROM-only validation track (maintainer, 2026-10-16: the end goal)

**Someone provides a ROM, `n64rx` puts out a working XBE.** The analyser
(§3.14, `docs/analyzer.md`) is checked game by game against the decomps:
each one is an answer key for where the functions are, which segments the
game loads and which functions are libultra. The decomps are read, never
built and never shipped: what the comparison uses (their function lists,
splat segment tables, symbol names) stays in `build-host/`, and the
analyser's rules carry only what generalises (behaviour, offsets, shapes),
never a game's addresses.

- **Order**: games whose decomps are complete or near it, across compilers,
  libultra versions and microcodes, so every rule meets every shape: Super
  Mario 64 (US/JP, IDO, libultra 2.0D, Fast3D), Mario Kart 64 (F3DEX,
  hand-written decoders), Pilotwings 64 (done: G0), Paper Mario (GCC,
  F3DEX2), Ocarina of Time and Majora's Mask (Yaz0, overlays), Banjo-Kazooie
  (Rare, compressed code), Super Smash Bros. (G1's candidate), Star Fox 64,
  Kirby 64, Dr. Mario 64, Pokémon Snap, Perfect Dark, GoldenEye (verify the
  status and licence of each before use: `docs/research.md`).
- **The harness** (`tools/xbox/decomp_check.py`, V's first file): given the
  user's ROM and a decomp checkout, it reads the decomp's splat config and
  symbol lists, runs the analyser, and reports functions missed, extra and
  mis-sized, segments missed, libultra names right / wrong / missing, and
  the live set's difference; with `--run` it builds the XBE and runs xemu's
  attract-mode scenario. A host test runs it on every decomp present in
  `build-host/decomps/` (none on CI: no ROM there).
- **Per game, done when**: every function the decomp has that the game can
  reach is found with its exact bounds, every segment the game loads with a
  constant address is found, every replaced libultra function is named with
  none wrong, and the XBE built from the ROM alone reaches the game's first
  gameplay in xemu. What only a run can find goes to the learning loop.
- **What it drives**: the analyser's rules, the interpreter for
  hand-written code, the learning loop (§3.14 step 5), the microcode
  registry (F3DEX/F3DEX2/S2DEX, the audio ABIs), saves (SRAM, Flash, the
  Controller Pak). The game ladder (§5.3) still sets which games get packs
  and console polish; V makes every rung start from the ROM alone.
- **Runs alongside M2-M6**; each handoff entry records V's table
  (`docs/analyzer.md` "How well it does").

### M3: the measurement gate (A/B against Wings64-X)
- **Goal**: the translation factor T (§3.11) on the console, per `[PERF]`
  bucket (game, interpreter, back end, audio, waits) at the same scenes, for
  gpr 64 vs 32, ThinLTO on vs off, `RECOMP_FUNC` weak vs plain; code size
  and `[MEMB]` per variant; a decision on the recompiler fork (register
  caching, mixed width) and on the ladder written into `docs/perf.md` and
  `docs/decisions.md`.
- **Files**: `xhw_perf.c` buckets for the runtime, `prof_report.py`
  symbolising recompiled names, `tools/xbox/ab_report.py` (two logs → one
  table), `docs/perf.md`.
- **Done when**: the table exists with console numbers, repeated runs within
  3 %; the plan's §3.11 hypothesis is replaced by measurements; the maintainer
  has chosen G1 with the numbers in hand.

### M4: the first game that only exists here (G1)
- **Goal**: the maintainer's G1 pack from boot to a complete game: the F3DEX
  (or F3DEX2) and S2DEX tables, the ucode registry's first non-F3D hashes,
  four players, rumble, the game's audio path (`hle` with its ABI table, or
  `recomp` with the vector unit if the microcode is custom), the pack's
  patches for what breaks, hor+ with a culling patch if cheap.
- **Files**: `f3d_interp.c` tables, `s2dex.c`, `ucode_registry.c`,
  `rsp_vu.c` + `test_vu.py` if needed, `games/<g1>/*`, `pad.c` four ports.
- **Done when**: a completion checklist in the pack's README ticked on the
  console at 480 and 720p; four pads in a match; `[PERF]` at the game's
  native rate with no missed retrace in its heaviest scene, or the deficit
  named; a 2-hour burn-in clean.

### M5: big ROMs and code on demand (G2: Ocarina of Time)
- **Goal**: Yaz0 decompression in `n64rx`, the page cache with pins and
  prefetch, the PI thread, overlay tracking by DMA source, `XRC_OVL_SECTIONS`
  with `XeLoadSection`, F3DZEX2, SRAM; OoT from the title to Kokiri Forest
  and the Deku Tree with `[PI]` and `[OVL]` counters on the console.
- **Files**: `pi.c` (cache), `ovl.c` (sections), `xhw_section.c`,
  `xbe_sections.py`, `tools/n64rx/yaz0.py`, `games/oot/*`.
- **Done when**: stall time per second from `[PI]` under 1 ms in the field
  and in Hyrule Field's loads; `[MEMB]` shows resident code following the
  overlays in use; the preloaded variant (`XRC_OVL_SECTIONS=0`) is measured
  for comparison; a DVD-R boot of the folder is tried once and its stall
  numbers recorded.

### M6: video, widescreen, the menu complete, hardening
- **Goal**: confirm-or-revert for 720p (Wings64-X's third net), the BACK
  menu complete (§5.2: the per-port Buttons sub-page, Rumble, Texture
  filter, Save slot, Screenshots), the culling patches for the packs that
  exist so hor+ has no edge pop-in there (`cull_patch = true`), the
  save-slot swap, `[MEMB]` flat over two hours on every pack at 720p, 128 MB
  consoles as 64, CPU-upgraded consoles (`[CLOCK]`).
- **Files**: `settings_menu.c`, `xhw_video.c` (probation), `widescreen.c`,
  `games/*/patches/cull*.c`, `docs/platform.md`, `docs/testing.md` switch
  table.
- **Done when**: the Wings64-X video scenarios pass per pack in xemu and on
  red (each mode photographed; Auto -> Off at once; set 720p then press
  nothing -> 480i after 15 s and the ini reads 480i); gold shows
  "720p -> 480i" and boots 480i; the Buttons sub-page captures on a Duke and
  a Controller S; two burn-ins per pack clean.

### M7: the tool, packaging, release
- **Goal**: `n64rx` as shipped: single-file binaries for Windows, macOS
  (universal) and Linux from CI, the SDK image tagged, `deploy`/`xiso`/`log`,
  the README in Melee-X's voice (three steps, the ROM requirement, what is
  supported, controls, the panel, troubleshooting), `LICENSE.md` (MIT for the
  port; N64Recomp MIT; Ares ISC for the VU reference; libultraship MIT
  tables; no GPL code; the Melee-X note), `docs/` complete.
- **Done when**: a person who has never seen the repo follows the README on
  each OS from a fresh machine to a mission on the console (the maintainer or
  a tester per OS), and the time from download to XBE is recorded per OS in
  `docs/handoff.md`.

### M8 (stretch): Expansion Pak games and the recompiled audio path
- **Goal**: Majora's Mask: 8 MB RDRAM at 480, `naudio` + `njpgdsp` through
  RSPRecomp and the vector unit, Flash saves, the budget of §3.10 class C
  measured; DK64 as the second data point.
- **Done when**: Clock Town playable at the game's rate or the deficit named
  with `[PERF]`; `[MEMB]` under 60 MB at 480 for an hour.

### M9 (by M3's numbers only): the recompiler fork and PGO
- **Goal**: if T says so, `patches/recomp/` (or a fork under TechProGabe):
  per-function register caching in locals, a mixed-width mode for games with
  a few 64-bit functions, a per-section output option upstream might take;
  PGO per pack with Melee-X's machinery; order files.
- **Done when**: the A/B table of M3 re-run with the gain recorded and every
  change behind a switch.

---

## 7. Risks and hard problems (ranked)

| # | risk | likelihood | impact | mitigation |
|---|---|---|---|---|
| R1 | **Translation overhead**: CPU-bound N64 games do not hold their rate on the P3 | medium | high (the approach's ceiling) | M3 measures before any second game; gpr 32; plain `RECOMP_FUNC`; ThinLTO/PGO; the fork's register caching; the ladder favours 20-30 fps titles first |
| R2 | **Memory**: recompiled code size, 8 MB games, ROM caches at 720p | medium | high for class B/C | on-demand XBE sections; page cache with pins; 480 first for class C; `[MEMB]` gates every pack's 720p |
| R3 | **Memory-model corners**: KSEG1 CPU access, direct ROM reads, TLB, self-modifying code, `osViGetCurrentFramebuffer` reads of VI registers | medium | medium (per game) | the fault policy of §3.2.1 (patch, mask, report), `recomp_scan64.py`'s KSEG1 report, upstream's stub list per game as knowledge, `XRC_MEM_MASK` |
| R4 | **HDD latency** for streamed ROMs (per-frame small DMAs) | medium | high for class B/C | pins, prefetch, the PI thread; the resident path where the ROM fits; measured in M5 |
| R5 | **Microcode coverage**: F3DEX vs F3DEX2 opcode tables, S2DEX, custom ucodes, VI modes | high | medium | registry with a hard stop on unknowns; libultraship tables as reference; per-pack family list; VI-mode content rect |
| R6 | **Legal**: GPL contamination from the runtime or the recomp projects; symbol files from unlicensed decomps; the XBE is game code | certain to need care | the maintainer's call | no code from N64ModernRuntime/Zelda64Recomp/community recomps (read, don't copy, named in `LICENSE.md`); packs hold facts, not bytes; syms policy per §8 Q7; no XBE ever published; CI without ROMs; `rom_leak_check.py` |
| R7 | **Thread semantics**: games that depend on exact libultra preemption, message ordering or `osYieldThread` spins | medium | medium | the shim's strict levels and queue reserve; the cooperative fallback; the stall watch in test builds |
| R8 | **Build time and compile memory** for 17k-function games on a laptop; ThinLTO over tens of thousands of functions | medium | medium (ease of use) | the cache, `--fast`, per-section files, progress and estimates in `n64rx`; MM is M8 |
| R9 | **Float determinism**: x87 doubles, rounding-mode changes (`set_cop1_cs`), denormals (the VR4300 flushes with FS set) | low | medium | `-mfpmath=sse` floats, `027f` control word, MXCSR+CW set together, `XRC_FTZ` knob; `test_recomp_h.py`'s rounding cases |
| R10 | **Docker on Windows** needs WSL2 and admin; Apple Silicon users on an amd64 image would be slow | medium | high (ease of use) | native engine via `doctor`; multi-arch image; the README's three steps tested per OS in M7 |
| R11 | **Upstream drift**: N64Recomp changes its output or header | low | low | the pin; `test_recomp_h.py` against the pinned header; move the pin deliberately |
| R12 | **Scope overlap** with Snap64-X and Wings64-X | certain | low | §8 Q2; the A/B makes Wings64-X the reference, not a competitor |
| R13 | **xemu hides hardware behaviour** (PFIFO timing, AC97, HDD latency, `XeLoadSection`) | high | medium | console at every milestone; `[PI]`, `[OVL]`, `[PERF]` read from the console only |
| R14 | **Upgraded CPUs, 128 MB consoles, patched `rdtsc`** | medium | high for those users | ACPI time, vblank pacing, `xhw_code_repair.c`, `xhw_ramlock.c` (all in M0) |

Hard problems named as such, so nobody is surprised by them:

| # | problem | plan |
|---|---|---|
| H1 | the register width and the P3's seven GPRs | gpr 32 by scan; M3's numbers; the fork if needed |
| H2 | libultra structs in RDRAM with host state beside them | side tables keyed by vram; the shim's logic unchanged |
| H3 | overlays as demand-loaded XBE sections through the kernel | one preload bit per section; `XeLoadSection`; the preloaded fallback |
| H4 | a ROM too big for RAM behind a game that expects microsecond DMA | page cache, pins, prefetch, the PI thread, console numbers |
| H5 | the RSP vector unit on a CPU with MMX only | scalar C + MMX asm, Ares's scalar as oracle, HLE first wherever the ABI is stock |
| H6 | three operating systems and "really easy" | Docker image + native toolchains + `doctor` + per-OS binaries + the tri-OS CI + M7's fresh-machine test |

---

## 8. Open questions for the maintainer

1. **G1**: Super Smash Bros. (the default), Star Fox 64, Mario Kart 64, or
   another? The choice fixes which interpreter table and audio path M4
   builds first.
2. **Pokémon Snap**: stay with the decomp plan (Snap64-X) or move here as a
   pack? The two plans share the F3DEX2 interpreter and the readback work;
   they differ in speed (native wins), in the menu (game-style needs the
   decomp or a MIPS-C patch) and in distribution (identical: neither ships a
   binary).
3. **Distribution**: confirm that no XBE is ever published and the product
   is the tool plus the packs. (Zelda64Recomp ships binaries containing
   recompiled code; this plan does not follow it.)
4. **The tool's shape**: CLI with per-OS single-file binaries (the plan), or
   also a small GUI (drop a ROM, watch the log)? And the name `n64rx`.
   *Answered (maintainer, 2026-10-18): both; the app is the way in for
   users (`docs/app.md`), `n64rx` stays the CLI.*
5. **Title IDs** `4E52nnnn` ("NR", one per pack) and original icons per pack:
   fine, or another scheme?
6. **Licence** of the port code: MIT, Melee-X-derived files relicensed by
   their author (Wings64-X's position); no GPL code. Confirm.
7. **Symbol files from unlicensed decomps** (OoT, MM, Smash if its decomp
   has no licence): (a) generate on the user's machine from a pinned decomp
   checkout (slow, needs the decomp's tools), (b) fetch a public symbol repo
   at build time (Zelda64RecompSyms has no licence file), (c) commit them as
   facts about the ROM. The plan's default is (b) where a public repo exists
   and (a) otherwise; nothing is committed from an unlicensed source.
8. **The recompiler fork**: patches under `patches/recomp/` applied to the
   submodule (the plan's default) or a fork repo `TechProGabe/N64Recomp`?
   Upstream's CONTRIBUTING should be read before anything is sent back.
9. **Test hardware**: red and gold as for the siblings; is a 128 MB or a
   CPU-upgraded console available for R14; is there a DVD-R user to test the
   disc path?
10. **Controller Pak**: emulate a 32 KB pak per port by default (ghost data in
    MK64, some saves) or only where a pack asks?

Decided on 2026-10-16 and no longer open: a 128 MB console uses only 64 MB,
for stability, with no opt-in for the upper half; every build behaves the
same on 64 MB and 128 MB consoles and on stock and upgraded CPUs (§3.13); the tool runs
on Windows, macOS and Linux with one command (§5.1); up to 720p and widescreen for every game, 1080i
not offered (§5.4); the BACK menu is the port's settings UI in every game,
game-agnostic, and a game-style menu a pack adds later is an addition, not a
replacement (§5.2); the loading screen is built on the siblings' splash with
the four ways back to a picture (§5.5); `L + R + BACK + BLACK` returns to the
dashboard and `L + R + BACK + WHITE` restarts in every game and every state,
and no pack can disable them (§3.9).

---

## 9. Seed prompt for the executing agent

Paste this to start the build, and again after every context reset.

> You are executing N64Recomp-X: N64 games on the original Xbox by static
> recompilation. The user's ROM goes through N64Recomp (MIT, pinned at
> `ffb39cd`, a submodule under `tools/recomp/`), the generated C is compiled
> with nxdk `58427c07` and LLVM 21.1.8 for the Pentium III, and it links with
> our own MIT runtime on the Wings64-X platform layer. The repo is
> `TechProGabe/N64Recomp-X`; work on `dev`, `main` is for releases. Read
> `CLAUDE.md`, then `docs/PLAN.md` in full: it is binding on the approach
> (§1), the architecture (§3), the build, packs and tests (§4), the feature
> specs (§5), the milestone order with its "done when" checks (§6) and the
> open questions (§8), whose defaults apply unless the maintainer has
> answered. Then read `docs/research.md`. Clone `TechProGabe/Wings64-X`,
> `TechProGabe/Melee-X`, `TechProGabe/Snap64-X` and `TechProGabe/OpenCrossing-Xbox`
> next to this repo and read the files `docs/PLAN.md`'s preamble lists; copy
> from them rather than rewriting; `xbox/src/hw` comes over as it is, and
> `xhw_splash.c` is the base of the loading screen (§5.5), which keeps its
> wordmark, font, palette, safe-video line and behaviours and adds the
> animated background, the game card, the stage progress, the video line,
> the bridge frame and the BACK + START reset. Read N64Recomp's
> `include/recomp.h` before writing ours.
>
> Work milestone by milestone starting at M0. Each milestone ends with a
> `default.xbe` that boots, the host tests green on Linux, macOS and Windows
> (CI), a `docs/handoff.md` entry (what was verified, how, on xemu or on the
> console) and a commit on `dev`; ask before pushing to `main`. Within a
> milestone, finish the "done when" list before moving on; if an item cannot
> be met, say which and why rather than narrowing it silently. M3 is a
> measurement, not a feature: do not start M4 without its table.
>
> Hard rules: never commit ROM material or anything derived from it: the
> ROM, decompressed images, recompiled C (`RecompiledFuncs`, `gen/`,
> `rsp/*.c`), built XBEs, save images, screenshots of game frames;
> `tools/xbox/rom_leak_check.py` must pass. Game packs hold addresses, names,
> sizes and hashes, never bytes; symbol files from unlicensed decomps are not
> committed (PLAN §8 Q7). Copy no code from N64ModernRuntime, Zelda64Recomp
> or any community recomp (all GPL-3.0): read them for behaviour, name them
> in `LICENSE.md`, write our own. Never edit `tools/recomp/` in place; changes
> go to `patches/recomp/` with a `.md` each. Every port-side departure from
> the game is marked `DIVERGES: <reason>`. Every optimisation and risky
> change has a kill switch and the default is the good build. The game
> triple is Melee-X's (`i686-pc-windows-gnu -mno-ms-bitfields -march=pentium3
> -msse -mfpmath=sse -ffp-contract=off -fno-strict-aliasing -fwrapv`); the
> platform code uses nxdk's triple; the two share only plain-scalar structs.
> Releases build with `XBOX_LTO=1` through `thinlto_link.py`. 64 MB is
> enforced. Time comes from the vblank and the ACPI timer, never an assumed
> CPU clock. Nothing runs on a ROM that has not passed the SHA-1 check.
> The tool (`n64rx`) must work on Windows, macOS and Linux with one command
> and be really easy to use: every error message says what to do next, the
> Docker engine gives identical results everywhere, `doctor` installs or
> explains the native toolchain, and the README's three steps are tested on a
> fresh machine per OS before a release. Up to 720p and widescreen are goals
> for every pack (§5.4) and the port's settings are the game-agnostic BACK
> menu drawn by the platform over the frozen game (§5.2): both live in the
> platform layer and the interpreter, never in per-game code, with only the
> culling fix per game; 1080i is not offered. `L + R + BACK + BLACK`
> (dashboard) and `L + R + BACK + WHITE` (restart) work on any port in every
> state in every pack, read from the raw pad state before any mapping at the
> game's reads, from the VI thread and from the watchdog; no pack may
> disable them (§3.9). Port code is MIT.
>
> xemu is for crashes and pictures only; timing, audio, memory headroom,
> HDD latency and GPU ordering are judged on a real Xbox, and you say when a
> result is emulator-only. Report what you verified and how, not what you
> intended. When a decision is the maintainer's (§8), state the default you
> are applying and carry on; stop only where proceeding under any assumption
> would waste the milestone.
