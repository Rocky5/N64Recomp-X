# Research: what exists for N64 recompilation on the Xbox, and what this takes

Surveyed 2026-10-16. Method: shallow clones of `N64Recomp/N64Recomp` (HEAD
`ffb39cd`, 2026-05-26), `N64Recomp/N64ModernRuntime` (HEAD `cdf5abb`,
2026-08-30), `Zelda64Recomp/Zelda64Recomp` (HEAD `b65c482`, 2026-09-25),
`Zelda64Recomp/Zelda64RecompSyms` (HEAD `5815f86`, 2025-09-06) and
`XboxDev/nxdk` (HEAD `14d5ee9`, 2026-05-20), read on that day; the sibling
checkouts (Melee-X `abdfd0f`, OpenCrossing-Xbox `e6be908`, Wings64-X
`83f1258`, wings64-dc `0c384bd`, Snap64-X `43e3803`); web searches for the
community recomps. `gcsmith/Pilotwings64Recomp`, which search engines list,
returned 404 and the author's profile no longer shows it; xboxdevwiki and
deepwiki were unreachable from the research environment. `docs/PLAN.md` §2
is the summary; this file keeps the evidence. Facts marked **verify** come
from search results or memory, not from a file read here.

## 1. N64Recomp (the recompiler)

- https://github.com/N64Recomp/N64Recomp. MIT, "Copyright (c) 2024 Wiseguy"
  (`LICENSE`). C++20, CMake ≥ 3.20, submodules `lib/{ELFIO,fmt,rabbitizer,
  sljit,tomlplusplus}`. Programs in the tree: `N64Recomp` (`src/`, 6,878
  lines), `RSPRecomp` (`RSPRecomp/src/rsp_recomp.cpp`), `RecompModTool`,
  `RecompModMerger`, `OfflineModRecomp`, `LiveRecomp` (sljit, x86-64 and
  ARM64 only: PR #114). `.github/workflows/validate.yml` builds it on CI.
- **README** (read in full): one C function per MIPS function, instructions
  translated one by one (`addiu $r4, $r4, 0x20` → `ctx->r4 = ADD32(ctx->r4,
  0X20);`), `jal` → a call, tail calls detected, delay slots duplicated,
  `jr` with a jump table → a `switch`; `jalr $25` → `LOOKUP_FUNC(ctx->r25)(rdram,
  ctx);`; relocatable overlays get `RELOC_HI16(section, offset)` /
  `RELOC_LO16` on `lui`/`addiu`/loads/stores; tested on IDO, mips gcc 2.7.2
  and modern clang output; "support for relocations for TLB mapping is
  coming"; single-file output mode for patches (linker interposition order);
  RSP microcode recompilation ("no support for recompiling RSP overlays").
  The README still says every function goes into its own file and that a
  symbol format without an ELF is planned; both are outdated: see the config
  keys below.
- **Configuration** (`src/config.cpp`, keys it reads): `[input]`
  `entrypoint` (optional: `has_entrypoint`), `elf_path` **or**
  `symbols_file_path` + `rom_file_path`, `output_func_path`,
  `relocatable_sections_path`, `single_file_output`, `use_absolute_symbols`,
  `manual_funcs`, `unpaired_lo16_warnings`, `recomp_include` (default
  `#include "recomp.h"`), `functions_per_output_file` (default **50**; files
  `funcs_N.c`), `trace_mode` (adds `trace.h`), `func_reference_syms_file`
  and `data_reference_syms_files` (ELF mode only, for patches),
  `allow_exports`, `strict_patch_mode`, `use_mdebug` + `mdebug_file_mappings`
  (IDO debug symbols), `output_binary_path`; `[patches]` `stubs`, `ignored`,
  `renamed`, `[[patches.instruction]]` (`func`, `vram`, `value`),
  `[[patches.func]]`, `[[patches.hook]]`. `--dump-context` writes `dump.toml`
  and `data_dump.toml` (`src/main.cpp:161, 311, 422`).
- **Output files** (`src/main.cpp`): `funcs.h` (prototypes), `funcs_N.c`
  (or one file per function when `functions_per_output_file` is 1, or the
  ELF's stem `.c` in single-file mode), `lookup.cpp` (only with an
  entrypoint: `get_entrypoint_address()`, `get_rom_name()`),
  `recomp_overlays.inl` (a `section_table[]` of `SectionTableEntry` plus
  per-section `FuncEntry`/`RelocEntry` arrays; the header it includes is
  `librecomp/sections.h`, whose struct definitions are: `FuncEntry {func,
  offset, rom_size}`, `RelocEntry {offset, target_section_offset,
  target_section, type}`, `SectionTableEntry {rom_addr, ram_addr, size,
  funcs, num_funcs, relocs, num_relocs, index}`, `FunctionExport`,
  `ManualPatchSymbol`; that header is in the GPL repo, 60 lines of struct
  declarations whose layout the generated code dictates, so ours is
  rewritten to the same field names).
- **Function classes** (`src/symbol_lists.cpp`): `reimplemented_funcs` (the
  libultra surface: OS init, AI, VI, DP, SP, controllers, EEPROM, motor, PFS,
  PI, threads, queues, timers, time, cache, Flash, voice, `__osDisableInt`
  and friends; renamed `<name>_recomp` by `main.cpp:456-463` and not
  recompiled), `ignored_funcs` (not recompiled, nothing emitted: e.g.
  `__createSpeedParam`, `__osAiDeviceBusy`, `osViFade`,
  `osViGetCurrentField`, `osViGetCurrentLine`, `osViGetCurrentMode`,
  `osViGetStatus`, `osViRepeatLine`, the exception and dispatch internals),
  `renamed_funcs` (libc/math recompiled under another symbol so the host's
  libc is never called: `sincosf sinf cosf asinf acosf atanf atan2f tanf
  sqrt sqrtf memcpy memset memmove memcmp strcmp strcat strcpy strchr
  strlen strtok sprintf vsprintf bcopy bzero bcmp malloc free calloc realloc
  rand srand ldiv lldiv setjmp longjmp __divdi3 __udivdi3 __moddi3 __umoddi3
  __muldi3 __ll_* __ull_* __f_to_ll ... ` and the inflate/gzip family).
- **64-bit instructions the generator handles** (`src/operations.cpp`):
  `daddu dadd dsubu dsub daddi daddiu` (Add64/Sub64), `dsll dsll32 dsrl
  dsrl32 dsra dsra32 dsllv dsrlv dsrav` (Sll64/Srl64/Sra64 with `ToU64`/
  `ToS64`), `dmult dmultu ddiv ddivu` (`src/recompilation.cpp:464-470`),
  `ld sd ldl ldr sdl sdr` (LD/SD/LDL... through `load_doubleword`,
  `do_ldl` etc.), `dmtc1 dmfc1` (`FsU64`), `cvt.*.l`, `trunc.l.*`,
  `round.l.*`. Everything 32-bit sign-extends through `S32`/`ADD32`.
- **Names the generator emits** (grep of `cgenerator.cpp`/`operations.cpp`):
  `S32 U32 S64 SIGNED ADD32 SUB32 MEM_W/H/B/HU/BU LD SD do_lwl/lwr/swl/swr/
  ldl/ldr/sdl/sdr DMULT DMULTU DDIV DDIVU MUL_S MUL_D DIV_S DIV_D CVT_S_W
  CVT_D_W CVT_D_L CVT_S_L CVT_D_S CVT_S_D CVT_W_S CVT_W_D CVT_L_S CVT_L_D
  TRUNC_W_S TRUNC_W_D TRUNC_L_S TRUNC_L_D NAN_CHECK CHECK_FR sqrtf sqrt fabsf
  fabs lroundf lround llroundf llround set_cop1_cs cop0_status_write
  do_break pause_self recomp_syscall_handler recomp_trigger_event
  LOOKUP_FUNC TRACE_RETURN`. Signature:
  `RECOMP_FUNC void {name}(uint8_t* rdram, recomp_context* ctx)`
  (`cgenerator.cpp:398`). A branch whose target is itself emits
  `pause_self(rdram);` (`recompilation.cpp:493-495`).
- **`include/recomp.h`** (475 lines; the contract):
  - `RECOMP_FUNC`: MSVC `__declspec(noinline)`; clang `extern inline
    __attribute__((weak,noinline))` ("weak forces clang to not perform any
    IPO as the symbol can be interposed"); GCC `noipa, optimize
    ("rounding-math")`. `SET_FENV_ACCESS()` pragma.
  - `DMULT`/`DMULTU` via `__int128` or MSVC `_mul128`, else `#error
    "128-bit integer type not found"` (i686 clang defines no
    `__SIZEOF_INT128__`). `DDIV`/`DDIVU` with the `INT64_MIN / -1` case.
  - `typedef uint64_t gpr;` `SIGNED`, `ADD32`, `SUB32`.
  - `MEM_W(offset, reg) (*(int32_t*)(rdram + ((((reg) + (offset))) -
    0xFFFFFFFF80000000)))`; `MEM_H` with `^ 2`, `MEM_B` with `^ 3`, `MEM_HU`,
    `MEM_BU`; `SD` as two 32-bit stores (high word at the lower address);
    `load_doubleword`; `do_lwl/lwr/swl/swr/ldl/ldr/sdl/sdr`. So RDRAM is
    kept as native 32-bit words with the bytes of each word reversed, and
    nothing masks or handles KSEG1 (`0xA0000000`) addresses.
  - Rounding: `get_cop1_cs`/`set_cop1_cs` through `fegetround`/`fesetround`;
    `CVT_W_S` = `lrintf`, `CVT_L_S` = `llrintf`, `CVT_W_D` = `lrint`,
    `CVT_L_D` = `llrint`; `TRUNC_*` are C casts; `DEFAULT_ROUNDING_MODE 0`.
  - `NAN_CHECK(val) assert(val == val)`; `fpr` union (`d`, `fl`/`fh`,
    `u32l`/`u32h`, `u64`); `recomp_context { gpr r0..r31; fpr f0..f31;
    uint64_t hi, lo; uint32_t* f_odd; uint32_t status_reg; uint8_t
    mips3_float_mode; }`; `CHECK_FR` asserts even float registers unless
    `mips3_float_mode`.
  - Runtime-provided: `cop0_status_write/read`, `switch_error(func, vram,
    jtbl)`, `do_break(vram)`, `recomp_func_t`, `recomp_func_ext_t`,
    `get_function(int32_t vram)` behind `LOOKUP_FUNC`, `extern int32_t*
    section_addresses`, `LO16`/`HI16` (the `+ ((x >> 15) & 1)` carry),
    `RELOC_HI16/LO16(section_index, offset)`, `recomp_syscall_handler(rdram,
    ctx, vram)`, `pause_self(rdram)`.
- **RSPRecomp** (`RSPRecomp/src/rsp_recomp.cpp`, 1,200+ lines): config keys
  `text_offset`, `text_size`, `text_address`, `rom_file_path`,
  `output_file_path`, `output_function_name`, `extra_indirect_branch_targets`
  (from Zelda64Recomp's `aspMain.us.rev1.toml`: text at ROM `0xC40FF0`, size
  `0x1000`, address `0x04001000`; `njpgdspMain.us.rev1.toml`: `0xC438B0`,
  `0xAF0`, `0x04001080`); `rsp_mem_mask 0x1FFF`; the output includes
  `librecomp/rsp.hpp` and `librecomp/rsp_vu_impl.hpp` (line 1179-1180), i.e.
  the vector unit lives in the runtime repo.
- **Symbol file format** (Zelda64RecompSyms `mm.us.rev1.syms.toml`, "#
  Autogenerated from an ELF via N64Recomp"): `[[section]]` with `name`,
  `rom`, `vram`, `size`, `functions = [{ name, vram, size }, ...]`, and
  `relocs = [...]` for relocatable sections (first at line 4118). MM: 81,965
  lines, **17,765 functions**; `datasyms.toml` 142,209 lines; `overlays.us.rev1.txt`
  lists **617** relocatable section names (`..ovl_title`, ...). The README
  says the files were generated from `zeldaret/mm` commit `385c45a`. **No
  LICENSE file in the repo.** MidwayRecomp's docs (search) describe writing
  the same TOML by hand for a flat binary; TheNewT64risRecomp generates it
  from a splat project with a `gen_syms.py` after `splat split` on an
  all-assembly build (search).

## 2. N64ModernRuntime (read for behaviour; GPL-3.0, no code taken)

- https://github.com/N64Recomp/N64ModernRuntime. `COPYING` = GNU GPL v3.
  Two libraries: `ultramodern/` (`audio.cpp, error_handling.cpp, events.cpp,
  extensions.cpp, input.cpp, mesgqueue.cpp, misc_ultra.cpp, port_main.c,
  renderer_context.cpp, rsp.cpp, scheduling.cpp, task_win32.cpp,
  threadqueue.cpp, threads.cpp, timer.cpp, ultrainit.cpp`) and `librecomp/`
  (`ai.cpp, config*.cpp, cont.cpp, dp.cpp, eep.cpp, euc-jp.cpp,
  extensions.cpp, files.cpp, flash.cpp, heap.cpp, math_routines.cpp,
  mod_*.cpp, mods.cpp, overlays.cpp, pak.cpp, patcher.cpp, pi.cpp, print.cpp,
  recomp.cpp, rsp.cpp, sp.cpp, ultra_stubs.cpp, ultra_translation.cpp,
  vi.cpp`).
- **Entry points** (`<name>_recomp(uint8_t*, recomp_context*)`, 133 of them):
  `osAiGetLength osAiGetStatus osAiSetFrequency osAiSetNextBuffer
  osCartRomInit osContGetQuery osContGetReadData osContInit osContReset
  osContSetCh osContStartQuery osContStartReadData osCreateMesgQueue
  osCreatePiManager osCreateThread osCreateViManager osDestroyThread
  osDpGetStatus osDpSetNextBuffer osDpSetStatus osDriveRomInit
  osEPiRawStartDma osEPiReadIo osEPiStartDma osEepromLongRead
  osEepromLongWrite osEepromProbe osEepromRead osEepromWrite
  osExQueueDisplaylistEvent osFlash* (12) osGetCount osGetMemSize
  osGetThreadId osGetThreadPri osGetTime osInitialize osInvalDCache
  osInvalICache osJamMesg osMotorInit osMotorStart osMotorStop osPfs* (10)
  osPiGetStatus osPiRawStartDma osPiStartDma osRecvMesg osSendMesg osSetCount
  osSetEventMesg osSetIntMask osSetThreadPri osSetTime osSetTimer
  osSpTaskLoad osSpTaskStartGo osSpTaskYield osSpTaskYielded osStartThread
  osStopThread osStopTimer osUnmapTLBAll osViBlack osViGetCurrentFramebuffer
  osViGetNextFramebuffer osViRepeatLine osViSetEvent osViSetMode
  osViSetSpecialFeatures osViSetXScale osViSetYScale osViSwapBuffer
  osVirtualToPhysical osVoice* (9) osWritebackDCache osWritebackDCacheAll
  osYieldThread __osDisableInt __osRestoreInt __osSetFpcCsr __osSpSetPc
  __osPiGetAccess __osPiRelAccess __osMotorAccess __osRdbSend
  __osInitialize_common/_isv/_kmc/_msp __checkHardware_* isPrintfInit
  is_proutSyncPrintf setjmp longjmp __divdi3 __udivdi3 __umoddi3 __f_to_ll
  __ll_div __ll_lshift __ll_mul __ll_to_f __ull_div __ull_rem __ull_rshift
  __ull_to_d __ull_to_f`.
- **Calling convention** (`ultra_translation.cpp`): e.g.
  `osCreateThread(rdram, (int32_t)ctx->r4, (OSId)ctx->r5, (int32_t)ctx->r6,
  (int32_t)ctx->r7, (int32_t)MEM_W(0x10, ctx->r29), (OSPri)MEM_W(0x14,
  ctx->r29))`; results in `ctx->r2`; `osYieldThread_recomp` is
  `assert(false)`.
- **Threads** (`threads.cpp`, `scheduling.cpp`, `ultramodern.hpp`): every
  N64 thread is a host `std::thread` with a `running`/`initialized`
  semaphore pair (`UltraThreadContext`); a `running_queue` of `OSThread`
  pointers in RDRAM; `check_running_queue` swaps to a higher-priority thread;
  `pause_self` loops on `wait_for_external_message` + `check_running_queue`;
  `yield_self`, `yield_self_1ms`; `set_native_thread_priority` is a no-op on
  Windows (commented out) and unimplemented on Linux. Thread-local
  `is_game_thread`, `thread_self` (a `PTR(OSThread)`).
- **Structs in RDRAM**: `thread_queue_insert(RDRAM_ARG PTR(PTR(OSThread)) ...)`,
  `TO_PTR(OSThread, t)->priority`: libultra objects are at N64 addresses and
  read through the RDRAM, which is why game code's inline reads of
  `validCount` and friends keep working.
- **Events** (`events.cpp`): a VI thread (`vi_thread_func`, honours
  `retrace_count`), a task thread and a gfx thread (`task_thread_func`,
  `gfx_thread_func`; SP/DP completion posted from them: tasks are
  asynchronous upstream), `osSetEventMesg` for SP/DP/AI/SI,
  `enqueue_external_message` with per-source requeue rules
  (`MessageQueueControl`).
- **RSP** (`librecomp/rsp.cpp`, `rsp.hpp`): `uint8_t dmem[0x1000]`;
  `run_task` copies the `OSTask` to DMEM `0xFC0`, DMAs `ucode_data` to DMEM 0
  (`0xF80 - 1` bytes), calls `ucode_func(rdram, task->t.ucode)`, checks
  `RspExitReason` (`Broke, ImemOverrun, UnhandledJumpTarget, Unsupported,
  SwapOverlay, UnhandledResumeTarget`); `RspContext` has 31 GPRs, DMA
  registers, a `RSP` vector-unit object, resume state; `RSP_MEM_*` macros
  with `^ 3` on a big-endian DMEM image; reciprocal and inverse-square-root
  tables of 512 entries. `rsp_vu.hpp`: "modified from the Ares N64 emulator
  core", ISC notice, `ARCHITECTURE_SUPPORTS_SSE4_1` on x86-64 (`nmmintrin.h`)
  and ARM64 (`sse2neon`), otherwise `Accuracy::RSP::SISD = true` (a scalar
  path exists).
- **Overlays** (`overlays.cpp`): `std::unordered_map<int32_t, recomp_func_t*>
  func_map`; `section_addresses` array; `load_overlays(rom, ram_addr, size)`
  finds code sections whose ROM range the DMA covers (`code_sections_by_rom`)
  and `load_overlay` enters their functions at `ram + offset` and sets
  `section_addresses[section.index] = ram`; `unload_overlays(ram_addr,
  size)`; patch sections loaded at fixed addresses; exports and events for
  mods.
- **PI and saves** (`pi.cpp`, `recomp.cpp`): the ROM is a `std::vector` in
  memory; `do_rom_read` copies byte by byte through `MEM_B`; PI DMA decodes
  by physical address (`rom_base` vs `sram_base`), SRAM/Flash reads and
  writes go to a save file through a saving thread with a file-swap
  handshake; `SaveType` per game; the ROM's XXH3 hash is checked and the
  byte order detected (`check_rom_start`, `byteswap_data`); `init` copies
  `0x100000` bytes from ROM `0x1000` to the entrypoint and writes `osMemSize`
  (`0x80000318`) = 8 MB; `run_thread_function(rdram, addr, sp, arg)` builds
  a context per thread.

## 3. Zelda64Recomp (the flagship; GPL-3.0, read for structure)

- https://github.com/Zelda64Recomp/Zelda64Recomp. `us.rev1.toml`:
  `entrypoint = 0x80080000`, `output_func_path = "RecompiledFuncs"`,
  `relocatable_sections_path = "overlays.us.rev1.txt"`, `symbols_file_path =
  "Zelda64RecompSyms/mm.us.rev1.syms.toml"` (the `elf_path` line is commented
  out), `rom_file_path = "mm.us.rev1.rom_uncompressed.z64"`; `[patches]
  stubs = ["RcpUtils_PrintRegisterStatus", "RcpUtils_Reset", "CIC6105_Init"]`
  ("an unnecessary function that accesses kseg1 addresses"), `ignored =
  ["D_80186028"]`; `[[patches.instruction]]`: `AudioHeap_WritebackDCache`
  `lui $at, 0x2000 -> 0x0000` ("Write audio dlists to kseg0 (cached, 0x80...)
  addresses instead of kseg1 (uncached, 0xA0...) ones. This saves the
  recompiler from needing to handle checking for kseg1 addresses on every
  memory operation"), two `jal osSetTimer -> nop` ("Disable RCP timeout
  detection"), a minimap `slti` fix.
- `BUILDING.md`: decompress the ROM first (`decompress_baserom.py` or
  `z64decompress`), run `N64Recomp us.rev1.toml`, `RSPRecomp aspMain...` and
  `njpgdspMain...`, then CMake; "In the game itself, you should be using a
  standard ROM, not the decompressed one" (the runtime decompresses:
  `src/game/rom_decompression.cpp`, Yaz0 over the DMA table). Windows builds
  with VS 2022 + clang; Linux with clang/lld.
- `CMakeLists.txt`: `RecompiledFuncs` is a static library of
  `RecompiledFuncs/*.c` compiled with `-fno-strict-aliasing
  -Wno-unused-variable -Wno-implicit-function-declaration`; `PatchesLib`
  likewise; `patches_bin.c` embeds `patches/patches.bin`.
- **Patches** (`patches/Makefile`): `clang -target mips -mips2 -mabi=32 -O2
  -G0 -mno-abicalls -mno-odd-spreg -mno-check-zero-division
  -fomit-frame-pointer -ffast-math -fno-unsafe-math-optimizations
  -fno-builtin-memset`, `-nostdinc -D_LANGUAGE_C -DMIPS` with the decomp's
  headers, `ld.lld -nostdlib -T patches.ld -T syms.ld --emit-relocs
  --unresolved-symbols=ignore-all`; `patches.toml`: `elf_path =
  "patches/patches.elf"`, `single_file_output = true`, `use_absolute_symbols
  = true`, `func_reference_syms_file`, `data_reference_syms_files` (the two
  syms files plus `patches/custom_syms.toml`), `output_binary_path`,
  `allow_exports`, `strict_patch_mode`. 60+ patch sources (`autosaving.c`,
  `camera_patches.c`, `culling.c`, `*_transform_tagging.c` for RT64
  interpolation, `input.c`, `options.c`, `ui_patches.c`, ...).
- `README.md` requirements: D3D12 SM6 / Vulkan 1.2 / Metal argument buffers
  tier 2; SSE4.1 on x86-64; "Changing framerate has no effect on gameplay"
  (RT64 interpolation); any aspect ratio.
- `src/`: `game/` (`config.cpp controls.cpp debug.cpp input.cpp
  quicksaving.cpp recomp_actor_api.cpp recomp_api.cpp recomp_data_api.cpp
  rom_decompression.cpp scene_table.cpp`), `main/main.cpp`
  (`recomp_entrypoint`), `ui/` (RmlUi: the menu is the port's, not the
  game's). `recomp_api.cpp` shows the host-hook style: `extern "C" void
  recomp_get_target_framerate(uint8_t* rdram, recomp_context* ctx)` etc.,
  called from MIPS-C patches by name.

## 4. The Xbox facts that matter here

- **Section loading** (nxdk `lib/xboxkrnl/xboxkrnl.h:1596-1611`):
  `XBAPI NTSTATUS NTAPI XeLoadSection(IN PXBE_SECTION_HEADER Section)` ("loads
  the supplied section into memory or increases its reference count if it's
  already loaded"), `XeUnloadSection(IN OUT PXBE_SECTION_HEADER Section)`
  ("decrements the reference count ... unloads it if the reference count
  reaches zero"); `XeImageFileName`. Section headers are reachable from the
  XBE header in memory (base `0x10000`).
- **cxbe** (`tools/cxbe/Xbe.cpp:360-400`): one XBE section per PE section,
  `bWritable` from `IMAGE_SCN_MEM_WRITE`, `bExecutable` from
  `IMAGE_SCN_MEM_EXECUTE`/`CNT_CODE`, and **`bPreload = true` for every
  section** (line 383); `dwInitFlags.bLimit64MB = true` (line 68). Clearing
  the preload bit on chosen sections is a post-link header edit (the siblings'
  `xbe_title_image.py` already edits the header for the title ID and icon).
- **Kernel calls** used by the siblings and needed here: `PsCreateSystemThreadEx`,
  `KeSetPriorityThread`/`KeSetBasePriorityThread` (priorities 0-31),
  `NtAllocateVirtualMemory` (fixed-base reservations: Melee-X reserves MEM1
  at VA `0x10000000` and ARAM at `0x12000000`), `MmAllocateContiguousMemoryEx`
  (Wings64-X's `xhw_rdram_alloc`: "contiguous, cached, bit 31 set", i.e. a
  `0x80000000 + physical` address), `MmMapIoSpace`.
- **Melee-X lazy memory** (`xbox/include/xhw.h:95-120`, `docs/platform.md`):
  `xhw_alloc_at(va, bytes)`, `xhw_reserve_lazy(va, bytes)` (committed 64 KB
  at a time by the crash guard on first touch, zero-filled), `xhw_commit`
  (before kernel writes and raised-IRQL touches), `xhw_lazy_fault`,
  `xhw_lazy_set_fill` (a fill callback for disc-backed chunks),
  `xhw_lazy_decommit`, `xhw_mem_free_kb`; `xhw_crash.c:133` "first touch of a
  demand-committed chunk (MEM1, ARAM): commit and retry".
- **nxdk libc**: `lib/xboxrt/c_runtime/_aulldiv.s` provides the MSVC-named
  64-bit helpers; Wings64-X links 64-bit arithmetic under the game triple,
  so the mingw-named helpers resolve somewhere in its link (check in M1
  before relying on it); nxdk's `lrintf` is a stub that asserts (Wings64-X
  `docs/handoff.md`, M3 entry); pdclib's `printf` has no `%f` (Melee-X).
- **Video**: nxdk's mode table has 480i/480p/720p/1080i for the HDTV pack
  (Snap64-X `docs/research.md`); Wings64-X ships 1080i off by default
  (`XHW_VIDEO_1080I 0`: console run 15, "works, frame rate cost too high").
- **Memory layout** (from Melee-X and Wings64-X docs; xboxdevwiki was
  unreachable): the XBE loads at `0x10000`; user reservations at
  `0x10000000+` work; physical RAM is identity-mapped at `0x80000000`
  (cached) for 64 MB; the GPU masks addresses with `0x03FFFFFF`; 128 MB
  consoles are held to 64 by `xhw_ramlock.c`.

## 5. The siblings: what is taken, from where

| area | file(s) | state |
|---|---|---|
| platform layer | Wings64-X `xbox/src/hw/` (25 C files, 7802 lines): `xhw_main.c` (boot flow, cards, exits), `xhw_splash.c` (unchanged from Melee-X), `xhw_video.c` (modes, retrace, fallback), `xhw_sys.c` (log, files, threads, events, mutexes), `xhw_pad.c` (SDL2, drain), `xhw_audio.c` (polled AC97, APU under xemu), `xhw_clock.c` (ACPI), `xhw_ramlock.c`, `xhw_code_repair.c`, `xhw_crash.c` (SEH, commit-on-fault), `xhw_watchdog.c`, `xhw_prof.c`, `xhw_perf.c`, `xhw_fbdump.c`, `xhw_autopad.c`, `xhw_overlay.c` (CPU text), `xhw_rom.c` + `xhw_sha1.c` + `xrom.h` (find, order, header, SHA-1), `xhw_string.c`, `xhw_debug.c`, `nv2a.c` (1,550), `nv2a_cc.c` (438), `nv2a_fault.c`, `asp_hle.c` (914, MMX), `xgfx_selftest.c` | running on hardware (console runs 2-15 in its handoff) |
| splash | Wings64-X `xbox/src/hw/xhw_splash.c` (166 lines; Melee-X's, from OpenCrossing `xbox_splash.c`): `XVideoSetMode(640, 480, 32)` then CPU writes into `XVideoGetFB()`; unscii-16 (`hal/font_unscii_16.h`, public domain, shared with `xhw_overlay.c` as `xhw_font16`) at zoom 3; `bg_at(y)` from `rgb(14, 22, 52)` to `rgb(6, 10, 28)`; `SPLASH_TEXT "TechProGabe Presents..."` at `TITLE_Y` 192; fade-in 17 steps x 30 ms (grey 40 → 255); bar 400x8 at (120, 272), `rgb(120, 200, 110)` on `rgb(30, 40, 60)`; "Hold BACK for 480i (safe video)" at y 416 in `rgb(110, 120, 140)`; hold `XHW_SPLASH_MS` 2000, `any_button()` (buttons or a trigger past 128 on any port) skips; `Sleep`, never `XVideoWaitForVBlank` ("pb_init() then fails with -4"); `xhw_splash_release()` forgets the framebuffer (`XVideoSetMode` frees it); `-DXHW_NO_SPLASH`, `-DXHW_SPLASH_DUMP` (`xhw_fbdump` of the frame). OpenCrossing `docs/architecture.md` "Safe video": BACK held as the splash ends runs the boot at 480i and saves Output 480i; controllers started before the splash; without a splash the boot waits 1.5 s for them | unchanged from Melee-X in Wings64-X; the base of PLAN §5.5 |
| libultra shim | Wings64-X `xbox/src/game/ultra/os_shim.c` (642 lines): threads as kernel threads at levels 2-14 (`level_of`), one recursive lock for libultra's interrupt mask, queues as rings with per-thread wake events (`sleep_on`/`wake_all`), event table, timer thread, ACPI-based `osGetCount`/`osGetTime` (46.875 MHz), VI thread (retrace, swap latch, `WX_VI_RESERVE` queue reserve, `fps_mode` swap interval, pad watch), `osSpTaskStartGo` synchronous (`f3d_run` or `wx_audio_task`, then SP/DP), virtual AI DAC (`osAiGetLength` = playing buffer; underrun counting), PI DMA from the ROM image, controllers, EEPROM; `MAX_THREADS 16`, `THREAD_STACK 128 KB` | running |
| interpreter | Wings64-X `xbox/src/gfx/f3d_interp.c` (1,532 lines: Fast3D, 16-slot cache, `XGFX_*` switches), `texconv.c`; Snap64-X `docs/PLAN.md` §3.4 (F3DEX2: 32 slots, `G_TRI2`, `G_LOAD_UCODE`, lines, readbacks) | F3D running; F3DEX2 designed |
| build | Wings64-X `tools/xbox/compile_game.py` (290), `thinlto_link.py` (138), `xbox/CMakeLists.txt` (104), `tools/xbox/{msys,docker}/build.sh`, `setup.sh`, `patch_pbkit.py`, `xbe_title_image.py`, `package_release.py`, `rom_leak_check.py`, `console.py`, `xemu_run.sh`, `fbdump_to_png.py`, `sym.py` | in use |
| rules | Wings64-X `CLAUDE.md` (never commit ROM material; never edit the external tree in place; `DIVERGES`; kill switches; splash as is; 64 MB enforced; ACPI time; SHA-1 gate; xemu is not hardware), Snap64-X `CLAUDE.md` (`-fno-strict-aliasing -fwrapv`; never `-ftrivial-auto-var-init` tree-wide; branches `dev`/`main`) | adopted |

## 6. Candidate games (facts to re-check when a pack is made)

ROM sizes and RAM needs are from memory of the retail releases unless noted;
microcode families from the N64 development record (**verify** each against
the ROM with the ucode registry's first run); decomp states from the October
2026 searches.

| game (code) | ROM | RDRAM | gfx ucode | audio | save | symbol source | notes |
|---|---|---|---|---|---|---|---|
| Pilotwings 64 (`NPWE`) | 8 MB | 4 MB | Fast3D (`G_TRI1` only; Wings64-X) | stock `aspMain`, 22,050 Hz | EEPROM 4k | `n64decomp/pw64` @ `1a8558f`, MIT, 100 % C: the decomp's IDO build gives the ELF | the A/B; everything else exists |
| Super Smash Bros. (`NALE`) | 16 MB | 4 MB | F3DEX family (**verify** 1 vs 2) | **verify** (HAL's engine over a libultra microcode) | EEPROM 4k | `ssb-decomp-re` (VetriTheRetri, last push 2026-04-26; "100.00 %" on decomp.dev per an Oct 2026 article) — **licence to verify** | four players; a recomp (SrBananaMan64, Boopie) announced 2026-10 "coming soon" with Smash Remix support (TimeExtension) |
| Star Fox 64 (`NFXE`) | 12 MB | 4 MB | F3DEX (**verify**) | custom engine over `aspMain`-family microcode (**verify**) | EEPROM 4k | `sonicdcer/sf64` decomp (basis of Starship) — **licence/status to verify** | Starfox64Recomp v1.0.3 (Feb 2026) exists (search) |
| Mario Kart 64 (`NKTE`) | 12 MB | 4 MB | F3DEX (**verify**) | SM64-style engine over `aspMain` (**verify**) | EEPROM 4k (+ Controller Pak ghosts) | `n64decomp/mk64` (basis of SpaghettiKart) | four players; MarioKart64Recomp v0.9.2 (Feb 2026) per search |
| Pokémon Snap (`NPFE`) | 16 MB | 4 MB (8 only in kiosk mode) | F3DEX2 + L3DEX2 | stock alSyn/`aspMain`, 32 kHz | Flash 1 Mbit | `ethteck/pokemonsnap` @ `df8815e`, no licence (Snap64-X research) | Snap64-X plans the decomp route; Snap64Recomp (GPL) is the recomp precedent |
| Ocarina of Time (US 1.0/1.2, `CZLE`) | 32 MB compressed (Yaz0); decompressed image larger (**verify** size) | 4 MB | F3DZEX2 | custom (`naudio`-class microcode) | SRAM 32 KB | `zeldaret/oot`, complete, no licence (**verify**); upstream syms format | ~470 overlays (**verify**); OoT recomps exist (`oot-recompiled` fork, search) |
| Majora's Mask (`NZSE`) | 32 MB compressed; decompressed image used by the recomp | **8 MB** | F3DZEX2 | `naudio` + `njpgdspMain` (two RSP microcodes recompiled) | Flash 1 Mbit | Zelda64RecompSyms (17,765 functions, 617 sections; no licence file) from `zeldaret/mm` `385c45a` | the flagship; class C |
| Donkey Kong 64 (`NDOE`) | 32 MB | **8 MB** | F3DEX2 (**verify**) | custom (**verify**) | EEPROM 16k | decomp in progress; "DK64 Rekongpiled" v1.0.2 (Sep 2026) per search | class C |
| Banjo-Kazooie (`NBKE`) | 12 MB | 4 MB | F3DEX with Rare changes (**verify**) | Rare's own microcode (**verify**) | EEPROM 4k | `n64decomp/banjo-kazooie` (80 % in an older article; **verify**) | BanjoRecomp playable Jan 2026 (search); needs the `recomp` audio path |
| Kirby 64 (`NK4E`) | 32 MB | 4 MB | F3DEX2 (**verify**) | **verify** | EEPROM 16k (**verify**) | decomp exists (**verify** status) | class B |
| Paper Mario (`NMQE`) | 40 MB | 4 MB | F3DEX2 | custom | EEPROM 16k (**verify**) | `pmret/papermario`, complete (**verify** licence) | class B; 40 MB ROM streams |
| Goemon's Great Adventure / Mystical Ninja | — | — | — | — | — | Goemon64Recomp v0.2.0-dev (Jan 2026) | early |

What makes a game *easy* here: 4 MB RDRAM, ROM ≤ 16 MB, a stock microcode
family the interpreter has a table for, the stock `aspMain` audio ABI, no
TLB, no direct ROM reads, a symbol source with a licence. PW64 has all of
these; Smash and Mario Kart have most; the Zeldas have none of the first two
and bring the big-ROM machinery for it.

## 7. Numbers used in the plan and where they come from

| number | value | source |
|---|---|---|
| functions in MM | 17,765 | `grep -c 'name ='` on `mm.us.rev1.syms.toml` |
| relocatable sections in MM | 617 | `wc -l overlays.us.rev1.txt` |
| libultra entry points upstream | 133 | grep of `_recomp(uint8_t` in the runtime |
| `recomp.h` | 475 lines | `wc -l` |
| default functions per output file | 50 | `src/config.cpp:443` |
| boot copy upstream | ROM `0x1000` → entrypoint, `0x100000` bytes | `librecomp/src/recomp.cpp:502` |
| `osMemSize` address | `0x80000318` | `recomp.cpp:518` |
| Wings64-X shim | 642 lines; 16 threads; 128 KB stacks; levels 2-14 | `os_shim.c` |
| Wings64-X interpreter / audio HLE / back end | 1,532 / 914 / 1,550 lines | `wc -l` |
| Melee-X lazy chunk | 64 KB | `xhw.h` `XHW_LAZY_CHUNK` |
| VR4300 counter | 46.875 MHz | `os_shim.c` (and libultra) |
| x86 code expansion | 4-6x the MIPS text | **estimate**, to be measured in M1 (`[RECOMP]` code size) |
| translation factor T | unknown | measured in M3 (§3.11) |

## 8. Unverified, by design of this survey

- Whether N64Recomp's output compiles unchanged under clang 21 for
  `i686-pc-windows-gnu` apart from the header (`__int128`, `fenv`,
  `lrintf`): expected yes; M1 confirms.
- Exact decompressed sizes of the Zelda ROMs and the pad-to-power-of-two
  behaviour of the decompressors.
- The microcode families and audio engines marked **verify** in §6.
- The licence of the Smash, Star Fox and Paper Mario decomps and of
  Zelda64RecompSyms (no LICENSE file seen).
- `gcsmith/Pilotwings64Recomp`'s existence (404 on 2026-10-16).
- Real-hardware frame rates of any recompiled N64 game on any weak CPU; no
  recomp-vs-decomp benchmark was found anywhere (searches returned only
  qualitative claims).
- How `__udivdi3` and friends resolve under the game triple in Wings64-X's
  link (it links; the provider was not traced).
