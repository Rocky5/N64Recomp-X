# CLAUDE.md

N64Recomp-X puts Nintendo 64 games on the original Xbox (2001) by static
recompilation: the user's own ROM goes through N64Recomp (MIT, pinned), the
generated C is compiled for the Pentium III with nxdk and LLVM 21, and it
links with this project's own MIT runtime on the Melee-X / Wings64-X
platform layer (NV2A back end, AC97, USB pads, the splash, the console
tooling). Target: retail Xbox, stock 64 MB. The user-facing tool, `n64rx`,
runs on Windows, macOS and Linux with one command.

**Status: planning.** `docs/PLAN.md` is binding on the approach (§1), the
architecture (§3), the build, game packs and tests (§4), the feature specs
(§5), the milestone order and exit checks (§6). `docs/research.md` holds the
evidence. Read both before writing code.

## Hard rules

- **Never commit ROM material or anything derived from it**: no
  `.z64/.n64/.v64`, no decompressed image, no recompiled C (`RecompiledFuncs/`,
  `gen/`, `rsp/*.c`), no built `default.xbe` (it contains the game's code),
  no save images, no screenshots of game frames. `tools/xbox/rom_leak_check.py`
  enforces it in CI. The user supplies their own ROM at build time; the
  console checks its SHA-1 before anything runs.
- **Game packs hold facts, not bytes**: addresses, names, sizes, hashes,
  knobs, patches as MIPS C. Symbol files from unlicensed decomps are not
  committed (`docs/PLAN.md` §8 Q7).
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
- **Stock 64 MB** (`xhw_ramlock.c`). **Time comes from the vblank and the
  ACPI timer**, never an assumed CPU clock. **The splash is Melee-X's
  `xhw_splash.c` as is.**
- **The tool is cross-platform and easy**: `n64rx` must work on Windows,
  macOS and Linux; the Docker engine gives identical results everywhere;
  `doctor` explains or installs the native toolchain; every error says what
  to do next; the README's three steps are tested on a fresh machine per OS
  before a release.
- **xemu is not hardware.** Timing, audio, memory pressure, HDD latency and
  GPU ordering are judged on a real Xbox; say when a result is emulator-only.
- **Branches:** work on `dev`; `main` is for releases. Subagents do not run
  git; the main thread commits, and only when asked.

## Doc map

| file | contents |
|---|---|
| `docs/PLAN.md` | the implementation plan: approach, architecture, the generated-code contract on the P3, runtime, overlays and code on demand, ROM sources, graphics and audio HLE, memory budget, build and game packs, `n64rx`, the game ladder, milestones, risks, open questions, seed prompt |
| `docs/research.md` | the evidence: N64Recomp and its header, the GPL runtime's surface (read only), Zelda64Recomp's structure, the Xbox section-loading and memory facts, what each sibling gives, candidate games with "verify" marks |

Settled fact → the matching `docs/` file (to be created per `docs/PLAN.md`
§3.1 and §6 M0: `architecture.md`, `platform.md`, `renderer.md`,
`toolchain.md`, `testing.md`, `decisions.md`, `perf.md`, `handoff.md`,
`games/README.md`). Add or remove a doc → update this map.
