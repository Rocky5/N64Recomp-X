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
resident = "auto"              # auto | always | never (the page cache, M5)

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
