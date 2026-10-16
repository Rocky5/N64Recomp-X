# N64Recomp-X

Nintendo 64 games running natively on an original Xbox, by static
recompilation.

Your own ROM goes through [N64Recomp](https://github.com/N64Recomp/N64Recomp),
which turns the game's code into C; that C is compiled for the Xbox's
Pentium III and linked with this project's runtime, which replaces the N64's
operating system, draws the game's display lists on the Xbox's own GPU and
plays its audio through the AC97. Not an emulator. The same platform layer
runs [Melee-X](https://github.com/TechProGabe/Melee-X),
[OpenCrossing-Xbox](https://github.com/TechProGabe/OpenCrossing-Xbox) and
[Wings64-X](https://github.com/TechProGabe/Wings64-X).

**Status: milestone M0.** The tool, the toolchain, the platform layer and
the loading screen exist; no game runs yet (that is M1, Pilotwings 64). A
ROM-free test program already goes the whole way: `n64rx build` recompiles
it, builds the XBE, and the Xbox (xemu so far) runs the recompiled code and
reports its checks. The plan is [`docs/PLAN.md`](docs/PLAN.md); what was
verified, and how, is [`docs/handoff.md`](docs/handoff.md).

## Three steps (once games are supported)

1. Install Docker Desktop, or run `n64rx doctor --install` for a native
   toolchain (Windows, macOS, Linux). `n64rx doctor` says what is there and
   what is missing.
2. `n64rx build "Your Game (USA).z64"` (or drop the ROM onto `n64rx.cmd` on
   Windows).
3. Copy the folder it makes (`out/<game>/`) to your Xbox, for example to
   `E:\Games\`, and start `default.xbe`.

Nothing leaves your machine: the ROM is read where it is, the build is
local, and the tool uploads nothing.

From a checkout today:

```sh
git clone --recursive https://github.com/TechProGabe/N64Recomp-X && cd N64Recomp-X
./n64rx doctor                                  # n64rx.cmd doctor on Windows
python tools/xbox/make_test_rom.py build-host/test/test.z64
./n64rx build build-host/test/test.z64          # -> out/N64Recomp-X test/
```

## On the console

| | |
|---|---|
| hold BACK while it loads | safe video: this boot runs 480i 4:3, and it is saved |
| hold BACK + START | safe video and the default settings |
| BACK | the port's settings menu (from M2) |
| L + R + BACK + BLACK | back to the dashboard, anywhere |
| L + R + BACK + WHITE | restart, anywhere |

Every game gets the same port: the "TechProGabe Presents..." loading screen,
output up to 720p, widescreen, the BACK menu and the exit combos. Nothing
from any game ships with this project, no built game is ever published, and
nothing derived from a ROM is committed here ([`LICENSE.md`](LICENSE.md)).
