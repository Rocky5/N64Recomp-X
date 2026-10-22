# Game status

Every game N64Recomp-X builds has one of three tiers. This file is where
a game's tier lives; `README.md`, `CLAUDE.md` and `docs/handoff.md` point
here, and `n64rx` will say the tier when it builds a game.

## The tiers

| tier | means | needs |
|---|---|---|
| **Gold** | fully working | played on a real Xbox (stock 64 MB, 480i or better) from boot through normal play: picture, sound, saves, controls and the game's own frame rate right; no known bug that a player would notice |
| **Silver** | playable | played on a real Xbox through normal play; known bugs, none game-breaking (nothing that stops progress, crashes, loses a save or makes the game unplayable) |
| **Experimental** | everything else | any game not yet played on a console, any game seen only in xemu, and any game with a game-breaking bug |

Rules:

- **Only the console promotes.** xemu is not hardware (`CLAUDE.md`): an
  xemu result, however good, leaves a game Experimental. The console run
  is named by its build (`vsm6`) and its logs are pulled.
- **A fix is not a promotion.** A fix staged for a game-breaking bug
  keeps the game where it is until the console run with the fix says so.
- **Any console regression demotes** at once (Gold to Silver for a
  noticeable bug, to Experimental for a game-breaking one), and the bug
  goes in "keeps it from the next tier".
- One ROM per row: the tier is the build from that ROM, named by the
  header's game code and revision; another version of the game is its own
  row and starts Experimental. `n64rx` reads its table
  (`tools/n64rx/n64rx_lib/compat.py`) by the same two: every row here,
  with "keeps it from the next tier" put in a player's words as the
  known problems the app shows when that ROM is loaded (a Gold row has
  none). `tools/xbox/test_compat.py` checks it against this file, so a
  row that changes here changes there in the same commit.
- Built from the ROM alone (`n64rx build`, no pack) unless the row says
  otherwise.

## The games (2026-10-22)

| tier | game | ROM (code, revision) | console run | keeps it from the next tier |
|---|---|---|---|---|
| **Gold** | Super Mario 64 (US) | `NSME` 1.0 | `vsm6`, `vsm7`: works perfectly, 29.9 fps; Gold by the maintainer (2026-10-18); `vsm18` (set 19, 2026-10-22): ok | - |
| **Gold** | Pilotwings 64 (US; its pack or the ROM alone) | `NPWE` 1.0 | `vnrx1`: plays, sound, EEPROM saves, the BACK menu, the combos, 53-59 fps; Gold by the maintainer (2026-10-18); `vpw2` (set 19, 2026-10-22): ok | - |
| **Gold** | Super Smash Bros. (US) | `NALE` 1.0 | `vssb6`: perfect at 59.9 fps; `vssb7`: one boot froze in the audio boot (fixed since); `vssb8`: two clean boots; Gold by the maintainer (2026-10-18); `vssb15` (set 19, 2026-10-22): ok | - |
| **Gold** | Diddy Kong Racing (US, v1.1) | `NDYE` 1.1 | `vdkr3` (set 19): works perfectly; Gold by the maintainer (2026-10-22) | - |
| **Silver** | Mario Kart 64 (US) | `NKTE` 1.0 | `vmk2`..`vmk10`: races at 29.9 fps, sound, Controller Pak | the map screen's TV (a framebuffer read) wrong; drift boosts in question |
| **Silver** | GoldenEye 007 (US) | `NGEE` 1.0 | `vge5`: works very well, 59.5 fps in menus; `vge10`: fine, dips to about 40 fps in intense scenes; `vge13` (set 19): Silver by the maintainer (2026-10-22) | texture flicker on the road markers (`vge13`); frame-rate dips in intense scenes (to ~40 fps) |
| **Silver** | Banjo-Kazooie (US) | `NBKE` 1.0 | `vbk12`, `vbk13`: plays at ~30 fps, the jiggy transition and the textures right; `vbk20`, `vbk21a/b/c` (set 19): Silver by the maintainer; `vbk22` (2026-10-22): the START screen's background right (the Z-image copy, 527e2c1) | a black-fog-like texture error in the opening intro, and texture errors on its jigsaw pieces there; shadows (Banjo's, enemies') now and then orange (not the fog: `vbk23` logged no draw losing it) |
| **Silver** | Ocarina of Time 1.2 (US) | `CZLE` 1.2 | `vzel13`, `vzel14`: plays at its own 20 fps, steadier since the learn writer | hitches at scene loads (its relocated overlays interpreted the first time), small audio skips there, texture pop-in, a blocky title |
| **Silver** | Castlevania - Legacy of Darkness (US) | `ND4E` 1.0 | `vcv2`: ran ~70 s, then crashed (fixed); `vcv5` (set 19): Silver by the maintainer (2026-10-22) | texture issues (`vcv5`, open) |
| Experimental | Mario Party (US) | `CLBE` 1.0 | `vmp11`: stable to a board; `vmp15`: froze at the board's start, textures shuffled on the board | the freeze at the board's start (game-breaking: its ended processes' contexts were never freed; fixed, `vmp17` staged), textures shuffled on the board (open) |
| Experimental | Pokémon Snap (US) | `NPFE` 1.0 | `vsnap5`: runs well on the Beach; `vsnap13`, `vsnap14`, `vsnap15a/b` (2026-10-22): the camera does not move (15b, the vertex output cache and word reads off: the same) | the camera does not move (13 photos taken, never a "Press Z" prompt or a red lock-on; the tutorial theory does not fit), the photos cannot be viewed or picked at the end, grass seen through the mountains |
| Experimental | Majora's Mask (US) | `NZSE` 1.0 | `vmm9`, `vmm14`: crash after the Nintendo logo | the crash (a page table left by the game run before it, inside its RDRAM; fixed, `vmm16` staged) |
| Experimental | Jet Force Gemini (US) | `NJFE` 1.0 | `vjfg1`: Rare logo, a broken menu, slow, silent | Rare's own GBI (its 0x07 now read: the menus draw in xemu since 2026-10-20) and audio microcode |
| Experimental | Kirby 64 - The Crystal Shards (US) | `NK4E` 1.0 | `vk4`: runs; major texture failure; `vk7` (set 19): texture issues at the menu | the textures (open: not seen in xemu; Banjo-Kazooie's `vbk20` shows the same) |
| Experimental | Kirby 64 - The Crystal Shards (E) | `NK4P` 1.0 | not run on a console | first run (`vk3` built, not deployed: the US ROM is tested instead); in xemu: intro, the tutorial rooms |
| Experimental | Mario Tennis (US) | `NM8E` 1.0 | `vmt2`: a white screen, frozen at boot; `vmt7`: froze in the BACK menu (fixed); `vmt8` (2026-10-22): works, the BACK menu's Paks page no longer freezes | a line at the bottom of the screen now and then (in play and the exposition), the right side of the screen cut off, the ball's textures wrong |
| Experimental | All-Star Baseball 2000 (US) | `NBEE` 1.0 | not run on a console | first run; in xemu (the ROM alone + 26 learning rounds): legal screens, title, Main Setup, Controllers, the line-up, a game in the Metrodome (batting, pitches, ~22 fps) |
| Experimental | All-Star Baseball 2001 (US) | `NASE` 1.0 | not run on a console | first run; in xemu (the ROM alone + 14 learning rounds): menus and a game (batting, ~22 fps) |
| Experimental | All-Star Baseball '99 (US) | `NBSE` 1.0 | not run on a console | first run; in xemu (the ROM alone + 6 learning rounds): legal screen, Team Select, Controllers, a game (batting) |
| Experimental | South Park: Chef's Luv Shack (US) | `NCYE` 1.0 | not run on a console | first run; in xemu (the ROM alone + 9 learning rounds): the title, Who's Playing, the menus; its logo draws wrong (the title's green background and red block, the letters) |
| Experimental | every other game | - | - | `docs/analyzer.md` ranks what stops the library (the sweep) |

## How a row changes

After a console run: the maintainer's report and the pulled logs go in
`docs/handoff.md` (as now); the row here moves only by the rules above,
in the same change, with the run named. `CLAUDE.md`'s status line and
`README.md` list the Gold and Silver games by name.
