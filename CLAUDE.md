# CLAUDE.md

N64Recomp-X puts Nintendo 64 games on the original Xbox (2001) by static
recompilation: the user's own ROM goes through N64Recomp (MIT, pinned), the
generated C is compiled for the Pentium III with nxdk and LLVM 21, and it
links with this project's own MIT runtime on the Melee-X / Wings64-X
platform layer (NV2A back end, AC97, USB pads, the splash, the console
tooling). Target: retail Xbox, stock 64 MB. The user-facing tool, `n64rx`,
runs on Windows, macOS and Linux with one command.

**The end goal (maintainer, 2026-10-16): someone gives `n64rx` a ROM and
gets a working XBE, with no decomp and no game pack.** `n64rx` analyses the
ROM itself (`docs/analyzer.md`, PLAN.md §3.14); packs only add fixes on top.
The decomps are the answer keys that make the ROM-only path stable
(PLAN.md §6 "V"): read, compared against, never shipped.

**Status (`docs/compat.md` for each game's tier, `docs/handoff.md` for the
record): M1 done; M2 mostly done. Three tiers: Gold fully playable on a
real Xbox with no major issues, Silver playable there with issues,
Experimental everything else (only the console promotes; the meaning as of
2026-10-22). From the ROM
alone on the console (red, 480i): Gold (the maintainer's): Super Mario 64,
Pilotwings 64, Super Smash Bros., Diddy Kong Racing. Silver: Mario Kart 64, GoldenEye,
Banjo-Kazooie, Ocarina of Time, Castlevania: Legacy of Darkness. Experimental: Mario Party (its board: fix staged), Pokémon Snap (its
tutorial's photo check: A/B staged), Majora's Mask (boot crash: fix staged), Jet Force Gemini, Kirby 64
(title right on red; its HUD), Mario Tennis and the rest; the BACK menu's Controls page. The loading screen is the deep-space title card (approved on red,
`vsm13`: 6-9 ms a frame; hold BACK for the settings before the game, L + R
for 480i).
In xemu: 720p on the working
games, GoldenEye's Dam right (textures, sky, Bond's gun, the pause watch: its cosf fell into sinf), Pokémon Snap through its Beach course (the camera lock is the first run's tutorial: three photos with a Pokémon in focus; the photo check now finds one and the tutorial prompts), Kirby 64 (E) to its tutorial, Mario Tennis's matches, Castlevania: Legacy of Darkness to Foggy Lake, All-Star Baseball 2000 / 2001 / '99 in a game, Ocarina and Majora's relocated overlays recompiled, Jet Force Gemini to its menus, Perfect Dark past its boot, Paper Mario past its
copy-protection boot to its effects, Diddy Kong Racing whole (Rare's DKR microcode), Star Fox 64 past its scene change (with the run token), Wave Race 64 racing (its own Fast3D variant read), Acclaim's Turok-engine games (Turok 1, 2 and 3, Rage Wars, Shadow Man, South Park) and both Ken Griffey games presenting frames from the ROM alone with the dry boot. The library sweep (s16, the dry boot and the shipped learnt facts: all 246 build, 192
present frames in 120 s) (`tools/xbox/sweep.py`) ranks what blocks the rest
(`docs/analyzer.md`). CI is manual-only (free minutes). The desktop app (`app/`, `docs/app.md`)
is the way in for users: portable, a prebuilt toolchain download, Windows
verified.** `docs/PLAN.md` is binding on
the approach (§1), the architecture (§3), the build, game packs and tests
(§4), the feature specs (§5), the milestone order and exit checks (§6).
`docs/research.md` holds the evidence. Read both, then `docs/handoff.md` and
`docs/decisions.md`, before writing code.

## Hard rules

- **Never commit ROM material or anything derived from it**: no
  `.z64/.n64/.v64`, no decompressed image, no recompiled C (`RecompiledFuncs/`,
  `gen/`, `rsp/*.c`), no built `default.xbe` (it contains the game's code),
  no save images, no screenshots of game frames. `tools/xbox/rom_leak_check.py`
  enforces it in CI. The user supplies their own ROM at build time; the
  console checks its SHA-1 before anything runs.
- **Game packs hold facts, not bytes**: addresses, names, sizes, hashes,
  knobs, patches as MIPS C. Symbol files from unlicensed decomps are not
  committed (`docs/PLAN.md` §8 Q7). The analyser's rules hold behaviour and
  libultra's struct shapes, never a game's addresses or a fingerprint of
  any ROM; what it derives stays in the user's build folder.
- **No code from GPL recomp projects.** N64ModernRuntime, Zelda64Recomp and
  the community recomps are read for behaviour and named in `LICENSE.md`;
  the runtime is written here. Port code is MIT.
- **Never edit `tools/recomp/` (the N64Recomp submodule) in place.** Changes
  go to `patches/recomp/` with a `.md` each; move the pin deliberately.
- **Every port-side departure from the game is marked `DIVERGES: <reason>`.**
- **Every optimisation and risky change has a kill switch; the default is
  the good build.**
- **Two triples**: generated code, runtime and interpreter use Melee-X's game
  triple (`i686-pc-windows-gnu -mno-ms-bitfields -march=pentium3 -msse
  -mfpmath=sse -ffp-contract=off -fno-strict-aliasing -fwrapv`); `xbox/src/hw`
  uses nxdk's; they share only plain-scalar structs (`xhw.h`, `xgfx.h`,
  `xrom.h`). Never `-ftrivial-auto-var-init` or `-ffast-math` on game code.
  Releases build with `XBOX_LTO=1`.
- **Stock 64 MB on every console** (`xhw_ramlock.c` holds the upper half of
  a 128 MB one; the budget is 64 MB for every pack). **Time comes from the
  vblank and the ACPI timer**, never the TSC or an assumed CPU clock, so an
  upgraded or overclocked CPU changes nothing but headroom (`docs/PLAN.md`
  §3.13). Each handoff entry records the `[MEM]` and `[CLOCK]` lines.
- **The loading screen is the deep-space title card** (maintainer,
  2026-10-18; `docs/PLAN.md` §5.5's note, `docs/decisions.md`): built on
  Melee-X's `xhw_splash.c` thread and holds, drawn by the CPU in the
  dashboard's 640x480 mode before pbkit so it shows on any TV; the game's
  title and ROM facts, never the app's logo as if it were the game's;
  "TechProGabe Presents" as its label; a frame costs a few ms (red: 6-9 ms)
  and the boot thread always gets the CPU it needs. The four ways back to a
  picture (L + R, L + R + START, the init fallback, the 15 s probation) are
  never removed; BACK held there opens the BACK menu before the game.
- **The tool is cross-platform and easy**: `n64rx` must work on Windows,
  macOS and Linux; the Docker engine gives identical results everywhere;
  `doctor` explains or installs the native toolchain; every error says what
  to do next; the README's three steps are tested on a fresh machine per OS
  before a release.
- **Up to 720p and widescreen for every game** (`docs/PLAN.md` §5.4): the
  video rules, the 16:9 mapping (generic hor+, the 4:3 box) and the modes
  live in the platform layer and the interpreter; only the culling fix is per
  game. 1080i is not offered.
- **BACK opens the port's settings menu in every game** (`docs/PLAN.md`
  §5.2): drawn by the platform over the frozen game, game-agnostic, never
  dependent on a per-game patch; a game-style menu a pack adds later is an
  addition, not a replacement.
- **`L + R + BACK + BLACK` returns to the dashboard and `L + R + BACK +
  WHITE` restarts, in every game, in every state** (`docs/PLAN.md` §3.9):
  detected on the raw pad state before any N64 mapping, at the game's reads,
  from the VI thread and from the watchdog; a dirty save is flushed first;
  no pack can disable it.
- **xemu is not hardware.** Timing, audio, memory pressure, HDD latency and
  GPU ordering are judged on a real Xbox; say when a result is emulator-only.
- **Branches:** work on `dev`; `main` is for releases. Subagents do not run
  git; the main thread commits, and only when asked.

## Doc map

| file | contents |
|---|---|
| `docs/PLAN.md` | the implementation plan: approach, architecture, the generated-code contract on the P3, runtime, overlays and code on demand, ROM sources, graphics and audio HLE, memory budget, build and game packs, `n64rx`, the game ladder, milestones, risks, open questions, seed prompt |
| `docs/research.md` | the evidence: N64Recomp and its header, the GPL runtime's surface (read only), Zelda64Recomp's structure, the Xbox section-loading and memory facts, what each sibling gives, candidate games with "verify" marks |
| `docs/architecture.md` | what exists: the tree, the boot, the runtime's pieces, the generated code's world, the BACK menu, the exit combos |
| `docs/renderer.md` | the graphics side as built: the microcode registry, the GBI families in the interpreter, what each game draws, gaps |
| `docs/analyzer.md` | the ROM-only path as built: the analyser's steps, what the runtime adds (interpreter, safety net, PIF, TLB), the learning loop, how well it does per game (decomps) and across the library (the sweep), gaps |
| `docs/toolchain.md` | the two triples, setup per OS, Docker, N64Recomp, LTO, the app's prebuilt toolchain |
| `docs/app.md` | the desktop app (`app/`): how it drives `n64rx` (`info --json`, `--events json`), where it keeps the source, builds and toolchain |
| `docs/testing.md` | host tests, the xemu runner, scripted input, the decomp check and the library sweep, the switch table |
| `docs/decisions.md` | decisions made while building and why (incl. the plan's departures) |
| `docs/compat.md` | each game's tier (Gold fully playable, no major issues; Silver playable, with issues; Experimental the rest), the rules that move a game, the console run behind each |
| `docs/roadmap.md` | the plan to the top 100 and 25 Gold: the seven cross-game buckets ranked, the phases, the 25 candidates and the gap for each, the top-100 appendix with the record and the DB facts, questions for the maintainer |
| `docs/review-2026-10.md` | the code review of `dev` at `ba03cd1`: host-test state, the generated code's aliasing cost, the PC tool's security and robustness, the analyser, runtime, libultra layer, renderer, platform, licences, docs debt, test coverage; each item CONFIRMED or PLAUSIBLE with file:line |
| `docs/handoff.md` | where things stand (games x xemu x console), then per milestone and per piece of work: verified how, not verified, gaps |
| `docs/look/` | the logo (`logo/`: mark, mono, lockups; `tools/xbox/render_logo.js` renders the icons); the loading screen's baseline shots (xemu, STILL); the BACK menu card's, Paks page's and Controls page's host previews |
| `games/README.md` | how a game pack is made |
| `LICENSE.md` | MIT; what came from where; what was only read |

Still to create per `docs/PLAN.md` §3.1 and §6: `platform.md`, `perf.md` (M3). Settled fact → the matching `docs/` file; add or remove a doc
→ update this map.
