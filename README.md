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

**Status: planning.** There is no code yet. The implementation plan is
[`docs/PLAN.md`](docs/PLAN.md); the evidence behind it is
[`docs/research.md`](docs/research.md).

How it is meant to work when it exists: install Docker (or let the tool set
up a native toolchain), run `n64rx build "<your rom>.z64"` on Windows, macOS
or Linux, and copy the folder it produces to your Xbox. Nothing from any game
ships with this project, no built game is ever published, and nothing derived
from a ROM is ever committed here.
