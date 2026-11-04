# Game packs

**Packs are optional.** A ROM with no pack is analysed from the ROM alone
(`docs/analyzer.md`, PLAN.md 3.14): `n64rx` finds the functions, segments
and libultra itself and writes a generated pack into `build-xbox/<id>/pack/`
(never committed). A pack in this folder adds what analysis cannot know:
fixes (instruction patches, stubs), the culling patch for widescreen, the
title and icon, the save type when the game's probe is not enough, and
symbol names for the report. `n64rx build <rom>` hashes the ROM and picks
the pack whose `game.toml` lists that SHA-1 (`--analyze` analyses anyway
and puts the pack's patches on top, mapped by address); `n64rx games`
lists the packs.

```
games/<id>/
  game.toml        the facts (below)
  <id>.syms.toml   sections and functions: N64Recomp's symbol format
  recomp.toml      the recompiler's config: @ROM@ and @OUT@ are filled in by n64rx
  overlays.txt     relocatable section names (M5; none for most class-A games)
  patches/         MIPS C patches + patches.toml (when the game needs them)
  icon.png         original art for the dashboard and the loading screen (never game art)
  README.md        what works, known issues, the knobs
```

`games/test` is the smallest real example.

## game.toml

```toml
[game]
id = "pw64"                    # the folder; the build folder is build-xbox/<id>
title = "Pilotwings 64"        # the loading screen, the XBE, the output folder
code = "NPWE"                  # the header's game code (0x3B): a wrong-region ROM gets a clear message
title_id = 0x4E520001          # "NR" + the pack's index: the save folder E:\UDATA\4E520001
sha1 = ["..."]                 # of the normalised (z64) image; a Yaz0 game also lists the decompressed one
rom_size = 8388608

[rom]
compressed = "none"            # none | yaz0-oot | yaz0-mm (M5)
resident = "auto"              # auto: in RAM up to 16 MB, the page cache over | always | never (the page cache)
pin = [[0x123400, 0x8000]]     # [offset, length] ranges the page cache keeps held (tables read every frame); at most half its cap

[cpu]
entrypoint = 0x80000400
boot_size = 0x100000
rdram_mb = 4                   # 8 for Expansion Pak games
gpr = "auto"                   # auto: tools/xbox/recomp_scan64.py decides | 32 | 64

[rsp]
gfx = ["F3D"]                  # the microcode families the registry must know
audio = "hle"                  # hle | recomp

[save]
type = "eep4k"                 # eep4k | eep16k | sram | flash | none

[video]
widescreen = "auto"            # auto | off (the pack found the detection wrong)
cull_patch = false

[status]
stage = "planned"              # planned | boots | playable | complete
```

## recomp.toml

N64Recomp's own format (`tools/recomp/src/config.cpp`); n64rx writes it to
`build-xbox/<id>/cfg/` with `rom_file_path = "@ROM@"` and
`output_func_path = "@OUT@"` replaced and `symbols_file_path` made absolute.
Stubs, ignored functions and single-instruction patches go here, each with a
comment saying why (PLAN.md 3.2.1: KSEG1 sites, RCP timeouts).

## Symbols

From an MIT decomp: build its ELF once and run `N64Recomp --dump-context`
on it, commit the TOML. From a decomp without a licence: not committed; the
pack says where n64rx fetches or generates it (PLAN.md §8 Q7). Every
function needs a name, a vram and a size; the entrypoint function must start
at ROM 0x1000.

## Making one

A pack starts from the analysis: build the ROM with no pack, read
`build-xbox/<id>/analysis.txt`, and add only what the game needs.

1. `n64rx build` on the ROM: with no pack it analyses it and names the game and its SHA-1.
2. Make `games/<id>/` with `game.toml` (that SHA-1), the symbol file and
   `recomp.toml`; copy `games/test` for the shape.
3. Build; the first boot's log names an unknown microcode by its hash
   (`[GFX]`) and any function a pointer reaches that the table doesn't know.
4. Write the README: what works, what doesn't, how it was checked.

## Learnt facts (`games/learned/`)

`games/learned/<code>.<sha1[:12]>.txt` holds what the project's console
learning rounds found for one ROM (`tools/xbox/learned_facts.py <rom>
<learn files...>`): the `func`, `dma`, `kuseg`, `rdram 8` and `reloc` lines
of the console's learn files, addresses and sizes only, never a captured
code record. A ROM-only build of the ROM with that SHA-1 (the image as
supplied, before a Yaz0 rebuild) reads it ahead of any `--learn`
(`N64RX_LEARNED=0`: not read), so a user's first build starts where the
rounds left off for code a ROM DMA put there: Ocarina of Time's first build
goes from 3,302 functions in 3 segments to 5,328 in 57 (its relocated
overlays recompiled). Code the CPU made (Mario Tennis's scenes) needs its
bytes, which the dry boot (`N64RX_DRYBOOT`, on by default) or a console round supplies.

## Routes (`games/routes/`)

`games/routes/<code>.<sha1[:12]>.txt` is an input script for one ROM's dry boot: button
presses in `docs/testing.md`'s autopad format and an optional `# frames N` line, no code
and no addresses. It takes the training past the menus into play, so the game unpacks
what a plain boot never reaches (`N64RX_DRYBOOT_ROUTES=0`: not read). None ships yet:
check a new one with a full build in xemu, Banjo-Kazooie's crashed (`docs/handoff.md`).

## Patches (`games/patches/`)

`games/patches/<code>.<sha1[:12]>.c` holds functions of one ROM written in
C (MIT, written here; each departure from the game marked `DIVERGES:`). A
ROM-only build of the ROM with that SHA-1 leaves each `XRC_PATCH(0x<vram>)`
function out of the recompile (or the interpreter) and builds this one under
the name the recompiled code calls; the function lookup finds it too
(`romonly.py`, `pipeline.py write_patch_c`, `gen/xrc_patch.c`;
`N64RX_PATCHES=0`: not used). A patch works on the context as generated
code does: the game's other functions are called through `LOOKUP_FUNC`
with the arguments in `ctx`, libultra through the runtime's `*_recomp`. It
holds addresses and C, never a byte of the game. Donkey Kong 64's audio loop
is the first (`NDOE.cf806ff26036.c`).
