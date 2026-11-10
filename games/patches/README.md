# Per-ROM fixes

Each file here belongs to one ROM. `n64rx` finds it by the ROM's game code and the first 12 hex digits of its SHA-1
(`<code>.<sha1[:12]>.c`, `tools/n64rx/n64rx_lib/romonly.py`), so the names can't change. This table says which game
each file is for. `N64RX_PATCHES=0` builds without any of them.

| File | Game | What it does |
|---|---|---|
| `NBYE.697c1e895fc8.c` | A Bug's Life (US) | `XRC_PATCH(0x8007AD60)`: the music library's MusFxBankInitialize in C |
| `NTHE.982ad2e1e44c.c` | Toy Story 2 (US) | `XRC_PATCH(0x8008E080)`: the same music-library routine |
| `NDOE.cf806ff26036.c` | Donkey Kong 64 (US 1.0) | `XRC_PATCH(0x80601D24)`: its audio thread's loop |
| `NFUE.4cbadd3c4e07.c` | Conker's Bad Fur Day (US) | `XRC_PATCH(0x1501878C)`: its main loop's step, with one check before it |
| `NW4E.3566d9b57232.c` | WWF No Mercy (USA, v1.0) | graphics task dones not held (`xrc_game_rcp_sender_us`): 4-wrestler matches at 30 fps; 16:9 as the 4:3 picture between bars (`xrc_game_horplus`) |
| `NEDE.f0ab0cd10a96.c` | WWF No Mercy Plus V3 (ROM hack) | the hack's VI-manager hook (`XRC_PATCH(0x800430C0)`, `xrc_game_vi`); its hooked 4x4 matrix product in C (`XRC_PATCH(0x80001E08)`); the two No Mercy settings above |
| `NWXE.442d417a52ed.c` | WWF WrestleMania 2000 (USA) | keeps its match overlay (`n64rx: ovl_jr`; without it a match crashed); the two No Mercy settings |
| `NW2E.e1711a251139.c` | WCW/nWo Revenge (USA) | the two No Mercy settings |
| `NVPJ.f9e9fa2ed819.c` | Virtual Pro-Wrestling 64 (Japan) | the two No Mercy settings |
| `NA2J.967e5239ca44.c` | Virtual Pro-Wrestling 2 (Japan, an English translation's image) | the two No Mercy settings |

## What a file can hold

- `XRC_PATCH(0xVVVVVVVV) { ... }`: a function of the ROM written in C, built in place of the recompiled or interpreted one.
- The runtime's per-ROM hooks, weak in the runtime and defined here when the ROM needs another value:
  - `void xrc_game_vi(unsigned retrace)`: called at every retrace (`rt/ultra/vi.c`).
  - `int xrc_game_rcp_sender_us(void)`: how long a graphics task's done is held, in microseconds (`rt/ultra/sp.c`, `XRC_RCP_SENDER_US`); -1 for the default.
  - `int xrc_game_horplus(void)`: 1 when the game culls for widescreen, so 16:9 draws the 3D across the full width (`gfx/f3d_interp.c`); otherwise the 4:3 picture between bars, unless the Widescreen setting is Force.
- Markers the analyser reads from the file:
  - `n64rx: ovl_jr`: keep an overlay whose only bad walks are a few `jr` with no table (`analyze/segments.py`, `N64RX_OVL_JR`).

A file that only sets hooks or markers, with no `XRC_PATCH`, is built too.
