# The renderer as built (PLAN.md §3.7)

A graphics task is one frame: `osSpTaskStartGo` (`xbox/src/rt/ultra/sp.c`)
hands it to `xrc_gfx_task` (`xbox/src/rt/gfx.c`), which names the task's
microcode through the registry and runs its display list through the
interpreter (`xbox/src/gfx/f3d_interp.c`) onto the NV2A back end
(`xgfx.h`, `xbox/src/hw/nv2a*.c`); the frame is presented at the list's
end. The CPU does the RSP's work (the list walk, the vertex buffer, the
matrix stack, lighting, texgen, fog, clipping, culling), the NV2A the
RDP's (the combiner, blending, depth, alpha test, fog, two texture units).

## The microcode registry (`xbox/src/gfx/ucode.{h,c}`)

A task names its microcode by address only. Which GBI its lists speak is
read from the microcode's data segment (`TK_UCODE_DATA`): every SGI and
Nintendo graphics microcode carries its version text there.

| text in the data segment | family | vertex buffer |
|---|---|---|
| `RSP SW Version: 2.0D, 04-01-96` (any) | Fast3D | 16 |
| `RSP Gfx ucode F3DEX ... 0.9x` | F3DEX, F3D_OLD flag (Mario Kart 64's 0.95) | 32 |
| `RSP Gfx ucode F3DEX/F3DLX/F3DLP/L3DEX ... 1.xx` | F3DEX | 32; `.Rej` 64; F3DLP 80 |
| `RSP Gfx ucode F3DEX.NoN/F3DZEX/... 2.xx` | F3DEX2 | 32; `.Rej` 64 |
| `RSP Gfx ucode S2DEX ... 1.xx` / `2.xx` | S2DEX / S2DEX2 | - |
| nothing found | Fast3D (the build before the registry) | 16 |

The version text is the microcode's behaviour, not a game's: one rule
serves every ROM linking the same library, nothing per ROM is kept.
`gfx_ucode_parse` is pure and host-tested; `gfx_ucode_at` reads the data
segment from RDRAM once per address, remembers it (8 slots) and logs
`[GFX] ucode <text> (data <addr>): <family>, <n> vertices[, old][, lines]`
the first time. A `G_LOAD_UCODE` in a list (`RDPHALF_1` = the new data
segment) goes through the same lookup and switches the family for the rest
of that task. Kill switches: `XGFX_UCODE_F3DEX`, `XGFX_UCODE_F3DEX2`,
`XGFX_UCODE_S2DEX` (0: that family read as Fast3D).

## One interpreter for every family

`run_dl`'s switch is Fast3D's. For any other family `ex_cmd` sees each
command first and either does it (what Fast3D has no equal for) or
rewrites it in place as the Fast3D command it equals and leaves it to the
switch, which keeps the display-list stack, the shadow volumes, the
widescreen tags and every RDP command. The state stays in Fast3D's terms
(the geometry mode in Fast3D's bits, F3DEX2's own word kept beside it).
The GBI facts are in `xbox/src/gfx/f3dex_gbi.h` (from the decomps' gbi.h:
mk64 for F3DEX with and without F3D_OLD, papermario for F3DEX2; read, not
copied).

| | F3DEX (1.x, 0.9x) | F3DEX2 |
|---|---|---|
| numbering | Fast3D's, plus TRI2 `B1`, BRANCH_Z `B0`, LOAD_UCODE `AF`, MODIFYVTX `B2` (F3D_OLD: RDPHALF_CONT), QUAD `B5` (F3D_OLD only) | renumbered: VTX `01` .. QUAD `07`, `D7`..`DF`, othermodes `E2`/`E3`, halves `E1`/`F1` |
| G_VTX | `v0*2 << 16 \| n << 10` | `n << 12 \| (v0+n) << 1` |
| triangles | indices x2; TRI2 and quads as two triangles; the flag rotates the indices (flat shade from the first) | the same, TRI1's indices in w0 |
| G_MTX | Fast3D's | params with PUSH inverted, PROJECTION 4: converted |
| G_POPMTX | one | w1 / 64 matrices |
| G_GEOMETRYMODE | SET/CLEAR, Fast3D's bits | one command (`~clear`, set), CULL and SHADING_SMOOTH bits moved: converted |
| G_MOVEWORD | Fast3D's | `index << 16 \| offset`; NUMLIGHT `n*24`, LIGHTCOL `(n-1)*24` |
| G_MOVEMEM | Fast3D's | `idx`, `ofs/8`, `(len-1)/8`; VIEWPORT 8, LIGHT 10 at `24*(n+1)` |
| G_TEXTURE | Fast3D's | `on` in bits 1..7 |
| othermodes | Fast3D's | `(32-sft-len) << 8 \| len-1`: converted |
| CULLDL | `vstart*2`, `vend*2` (F3D_OLD: Fast3D's x40) | the same as F3DEX |

CULLDL ends the list when every vertex in the range is beyond the same
plane (the outcodes, which carry hor+'s wider extent); BRANCH_Z branches
when the vertex's screen z (`z/w * vscale.z + vtrans.z`, 16.16) is at most
the value; MODIFYVTX sets RGBA, ST (already scaled), the screen XY (back
through the viewport at the vertex's w) or the screen Z. F3DLX/F3DLP and
the .NoN/.Rej variants differ from their parent in clipping and precision
only: the same commands. L3DEX (lines) draws nothing.

S2DEX: its own commands (G_BG_1CYC, G_BG_COPY, G_OBJ_RECTANGLE, G_OBJ_SPRITE,
G_OBJ_MOVEMEM, G_OBJ_LOADTXTR ..., G_SELECT_DL, G_OBJ_RENDERMODE) are
skipped (counted in the 600-frame `[GFX] unhandled op` line) so they never
reach Fast3D's switch, where their numbers mean MTX, VTX, MOVEMEM; its
shared commands (DL, ENDDL, the segment table, the RDP, the othermodes) run
as in its parent. Not drawn yet: backgrounds and objects of S2DEX games stay
black.

Textures: G_LOADTLUT keeps TMEM's upper half (256 entries, one a 64-bit
word) and CI4/CI8 tiles convert through it (`texconv_ci_to_argb8`, RGBA16
or IA16 by the othermode's TLUT type, CI4 at the tile's palette); the
palette is part of the texture cache key.

Widescreen (generic hor+, the 4:3 box), the screen-fade and BACK-menu held
frame, the title fit and the scissor rules are below the GBI: the same for
every family.

## What each game draws (xemu only; xemu is not hardware)

| game | microcode | from the ROM alone |
|---|---|---|
| Pilotwings 64 | Fast3D (`RSP SW Version: 2.0D`) | as before the registry; the BACK menu over the held frame |
| Mario Kart 64 | F3DEX 0.95 (F3D_OLD) | the title, game/player/map select, a Luigi Raceway race driven with A held (HUD, karts, track, items, the course map), `scenarios/mk64_race`; no texture misses, no unhandled commands. At 720p 16:9 (`scenarios/mk64_hd`, Widescreen Auto) the race is hor+ (the world to the edges, the HUD in the 4:3 box), the title in the box |
| Paper Mario | F3DEX2 | not reached: the analyser stops at its boot segment (`n64rx: This ROM could not be analysed`) |

## Gaps

- Mario Kart 64: a white translucent puff covers the driver's back in every
  race shot: the kart's smoke particles (the decomp draws them with
  `G_RM_ZB_CLD_SURF` and `G_CC_MODULATEIDECALA`, i.e. texel alpha over the
  frame, depth-tested, no depth write) in front of the sprite; maybe as on
  the hardware, maybe too opaque: to compare with a console.

- S2DEX's own commands are not drawn (no game in the ladder needs them yet
  from its ROM alone).
- F3DEX2's forced matrix (`G_MV_MATRIX` + `G_MW_FORCEMTX`), `G_MV_POINT`,
  G_LINE3D, `G_DMA_IO`, `G_SPECIAL_*`: skipped (counted).
- F3DEX2 is verified on the host only (no F3DEX2 game builds from its ROM
  yet: Paper Mario, Smash Bros. and Pokemon Snap stop in the analyser, Dr.
  Mario 64 at the link).
- Lighting: F3DEX2's lookat (`G_MV_LIGHT` offsets 0 and 24) is not kept;
  texgen uses the eye-space normal as for Fast3D.
