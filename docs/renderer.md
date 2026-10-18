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
| `RSP Gfx[Safe] S2DEX ... 2.05` (a tag in place of `ucode`: Ocarina of Time's second microcode, loaded by `G_LOAD_UCODE`) | S2DEX2 | - |
| nothing found | Fast3D (the build before the registry) | 16 |

The version text is the microcode's behaviour, not a game's: one rule
serves every ROM linking the same library, nothing per ROM is kept.
`gfx_ucode_parse` is pure and host-tested; `gfx_ucode_at` reads the data
segment from RDRAM once per address, remembers it (8 slots) and logs
`[GFX] ucode <text> (data <addr>): <family>, <n> vertices[, old][, lines]`
the first time. A `G_LOAD_UCODE` in a list (`RDPHALF_1` = the new data
segment) goes through the same lookup and switches the family for the rest
of that task. Kill switches: `XGFX_UCODE_F3DEX`, `XGFX_UCODE_F3DEX2`,
`XGFX_UCODE_S2DEX` (0: that family read as Fast3D). Before the tag was
read, Ocarina of Time's S2DEX2 lists ran as Fast3D after its first
`G_LOAD_UCODE` in Kokiri Forest (garbage matrices, the scheduler's task
never ending). `G_RDPSETOTHERMODE` (0xEF, gDPSetOtherMode: both othermode
words at once) is handled in every family; Ocarina of Time sets its render
modes with it (tens of thousands a minute, all dropped before).

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

A LOADBLOCK with dxt 0 loads data kept in TMEM order (the RDP reads odd
rows swapped): `texconv_deswizzle` swaps the 32-bit halves of each 64-bit
word on odd rows, and for RGBA32 (split across TMEM's two banks, an odd row
reads texel i from i ^ 2) `texconv_deswizzle32` swaps the 8-byte halves of
each 16-byte group (`XGFX_DESWIZZLE32`, 2026-10-17: Smash Bros.'s
character portraits and names were striped with the 16-bit rule; the
maintainer's "blurry textures" on the console).

Rare's Fast3D (GoldenEye, Perfect Dark: `RSP SW Version: 2.0G`) has
`G_TRI4` at 0xB1, unused in Nintendo's Fast3D: four triangles of 4-bit
indices, x and y nibbles in w1, z in w0, a triangle of three 0s skipped
(`XGFX_TRI4`, host test `tri4`); without it GoldenEye drew one model
triangle in ten.

RDP rules added 2026-10-17 (each with its switch in `docs/testing.md`):
- the screen's size is the width of the buffer the game swaps to (its
  `osViSwapBuffer` addresses) and 3/4 of it, or the VI mode's rows when
  they differ (640x240); 320x240 maps as before (`XGFX_HIRES`;
  GoldenEye's 440x330 menus, `[GFX] screen WxH`);
- a 1/2-cycle texrect covers every row and column a subscanline of it
  touches, so `(y << 2)` to `((y + 1) << 2) - 1` fills row y whole when
  upscaled (`XGFX_TEXRECT_SNAP`; GoldenEye's line-by-line backgrounds);
- a fill into a 32-bit colour image takes its colour as RGBA8888
  (`XGFX_FILL32`);
- a LOADBLOCK into a 32-bit tile under a 16-bit image counts 32-bit
  texels (`XGFX_LOAD32`; Mario Party's title logo).

Widescreen (generic hor+, the 4:3 box), the screen-fade and BACK-menu held
frame, the title fit and the scissor rules are below the GBI: the same for
every family.

## What each game draws (xemu only; xemu is not hardware)

| game | microcode | from the ROM alone |
|---|---|---|
| Pilotwings 64 | Fast3D (`RSP SW Version: 2.0D`) | as before the registry; the BACK menu over the held frame |
| Mario Kart 64 | F3DEX 0.95 (F3D_OLD) | on the console (red): races at 29.9 fps, display lists 2-3 ms a frame; in xemu: the title, game/player/map select, a Luigi Raceway race driven with A held (HUD, karts, track, items, the course map), `scenarios/mk64_race`; no texture misses, no unhandled commands. At 720p 16:9 (`scenarios/mk64_hd`, Widescreen Auto) the race is hor+ (the world to the edges, the HUD in the 4:3 box), the title in the box |
| Super Mario 64 | Fast3D | the head screen and the castle grounds (`--learn`, the TLB); the head's shading is off |
| Paper Mario | F3DEX2 | not reached: the analyser now passes its boot; the build stops at overlays sharing a vram |
| Super Smash Bros. | F3DEX (fifo 2.0) | the intro, the title and the character select at about 50 fps (no learning rounds) |
| Blast Corps | Fast3D (2.0D) | the Rare logo, the attract demo at 50-56 fps with its text (the font's glyphs hashed whole), the title, the profile screen (its portrait frame empty) |
| Banjo-Kazooie | F3DEX 1.21 | its intro: Banjo and Kazooie, textured, about 20 fps (`--learn`, captured code); the sky and a tree draw black. A tile inside a larger LOADBLOCK takes the latest load that covers its TMEM address (`XGFX_TMEM_WITHIN`: Banjo's faces) |
| GoldenEye | Fast3D 2.0G with Rare's G_TRI4 | Nintendo/Rare logos, the gun barrel, the 440x330 file select and mission menus, the Dam in-game; the pause watch's model missing, the sky purple, one block's texture garbled (open) |
| Mario Party | F3DEX | the intro's 3D scenes, the title (its 32-bit logo strips: `XGFX_LOAD32`), Mushroom Village; on red its frames whole since present-on-swap |
| Ocarina of Time 1.2 | F3DZEX2 (+ S2DEX2 `[Safe]`, JPEG task) | the title, file select, name entry, Kokiri Forest; on red its textures wrong and two moons on the title (open) |

## Framebuffer effects (`xbox/src/gfx/fbfx.{h,c}`, f3d_interp.c)

Frames are drawn on the NV2A and never land in RDRAM. Mario Kart 64's big
screens (Luigi Raceway, Wario Stadium: `copy_framebuffer` in the decomp's
render_courses.c / skybox_and_splitscreen.c) copy 64x32 tiles of the frame
before with the CPU into segment 5 textures; Pokemon Snap's photos read
the frame too. The maintainer's "frame buffer screen on the Mario map" is
Luigi Raceway's (the Mario GP's first course). What is built
(`-DXGFX_FB_READBACK=1`; off by default, see below):

- end of frame: one draw copies the picture, N64 size, into a quarter of a
  640x480 readback stage in cached memory (`xgfx_target_stage`,
  `xgfx_tex_linear_back`); the colour image (G_SETCIMG width and size, the
  height from the frame's lowest scissor/fill row, at most 3/4 of the
  width) is a slot (fbfx.c: 4, least recently drawn goes);
- start of next frame: the slot's whole pages are looked at through the
  x86 accessed bits (`xhw_watch_pages`, xhw_alias.c; armed every frame); a
  touched frame is hot for 60 frames and converted into RDRAM (RGBA5551 or
  RGBA8888, word-swapped) every frame (`xgfx_stage_read`: wait for the GPU,
  `wbinvd`, read);
- a texture load from a frame not converted yet converts it first;
- `[GFX] framebuffer readback` (every 600 frames) and `[NV2A] ... stage
  reads` count it.

Tried and dropped: guarding the pages (not present) and converting on the
fault. The probe (a page read from the boot thread) reached the crash
guard's handler, but the game's touch of a guarded frame page was a
bugcheck 0xA (IRQL 0xFF, kernel EIP 0x80014F10) before any handler ran
(xemu, the real kernel).

Verified in xemu only: Mario Kart 64 in the race scenario touches its
frames (53 frames a 600), readback runs; `-DXGFX_FB_DUMP=60` dumped the
RDRAM frame (the N64 logo intro) correct at 320x240. The big screen itself
was not in any scripted shot. Gaps: Pilotwings 64 touches its frames every
~2 s (or the A bit is set by something else) so it stays hot, and a
readback cost 20-85 ms in xemu (GPU wait included): flight dropped to ~14
fps, hence off by default. Next: convert only the rows of the touched pages
(the A bits say which), and wait for the GPU once per frame (before
xgfx_frame_begin). Render to texture in the same frame (Zelda's pause,
Banjo's jigsaw) is not done: an off-screen colour image still draws on the
screen.

## Gaps

- Framebuffer effects (a frame used as a texture or read by the CPU): the
  frame never lands in RDRAM, so Mario Kart 64's map screen shows stale
  memory (seen on the console); Pokémon Snap's photos need the same (agent
  `fbfx`).
- The BACK menu's card is a 1 MB texture made every frame it shows; when the
  pool has no room the texture cache is dropped (`-DXGFX_HOLD_EVICT`,
  `docs/decisions.md`). Keeping one card texture for the menu's life would
  be cheaper.

- Mario Kart 64: a white translucent puff covers the driver's back in every
  race shot: the kart's smoke particles (the decomp draws them with
  `G_RM_ZB_CLD_SURF` and `G_CC_MODULATEIDECALA`, i.e. texel alpha over the
  frame, depth-tested, no depth write) in front of the sprite; maybe as on
  the hardware, maybe too opaque: to compare with a console.

- S2DEX's own commands are not drawn (no game in the ladder needs them yet
  from its ROM alone).
- F3DEX2's forced matrix (`G_MV_MATRIX` + `G_MW_FORCEMTX`), `G_MV_POINT`,
  G_LINE3D, `G_DMA_IO`, `G_SPECIAL_*`: skipped (counted).
- F3DEX2 is verified on the host only (no F3DEX2 game runs from its ROM
  yet: Paper Mario and Pokémon Snap stop at overlays sharing a vram, Dr.
  Mario 64 at a KSEG1 register read, Smash Bros. in the analyser).
- Lighting: F3DEX2's lookat (`G_MV_LIGHT` offsets 0 and 24) is not kept;
  texgen uses the eye-space normal as for Fast3D.
