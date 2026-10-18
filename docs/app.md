# The app

`app/` is the desktop app: drop an N64 ROM on it, see what the game is,
its tier (Gold, Silver, Experimental: `docs/compat.md`) and its known
problems, press Build, get the folder for the Xbox, and optionally send it
to the console by FTP or make an ISO. Electron, plain JavaScript, in the
port's galaxy look (the loading screen's nebula, the BACK menu's card).
Why an app and why this shape: `docs/decisions.md` (2026-10-18).

It is a shell over `n64rx`; every step runs in Python, the same code the
CLI runs. `app/README.md` says how to run and package it; the toolchain it
downloads is in `docs/toolchain.md`.

## How the app talks to n64rx

`python tools/n64rx/n64rx.py info <rom> --json` prints one object:

```json
{"rom": {"path", "title", "code", "region", "revision": "1.0", "size", "order", "sha1", "cic"},
 "pack": {"id", "title"} | null,
 "compat": {"tier", "meaning", "name": "... | null", "issues": ["..."], "other_version": "... | null"},
 "estimate": "about 5 minutes"}
```

`compat` is `compat.report()`: `name` is the `docs/compat.md` row's name
(null for a ROM no row lists), `issues` the known problems in a player's
words (empty for Gold and for an unlisted ROM), `other_version` set when a
row of the same game in another region or revision is Gold or Silver.

`--events json` before a command (`build`, `deploy`, `xiso`, `info`) makes
every line on stdout one JSON object:

| `ev` | fields | when |
|---|---|---|
| `step` | `n`, `of` (5), `text` | a pipeline step starts (`pipeline.Progress`) |
| `note` | `text` | a fact about the step |
| `done` | `secs` | the step ended |
| `log` | `text` | any other line (deploy's files, extract-xiso's output, a traceback) |
| `result` | `folder`, `status` (build); `file`, `status` (build `--iso`); `dest` (deploy); `file` (xiso) | it worked |
| `error` | `message`, `hint` | an `N64rxError` (exit 2) or an internal error (exit 3) |

`build --iso FILE` (the app's "Disc image" choice) builds the folder in
`<cache>/iso-staging`, makes the ISO with extract-xiso as a sixth step
(`of` is 6), sends one `result` with the disc image's `file`, and removes
the staging folder.

Lines that are not JSON (a tool's own output) are kept as log by the app.
`tools/xbox/test_n64rx.py` checks the build's events and `info --json`.

## Where things are

- **Dev mode** (`npm start` in `app/`): the checkout is the source, the
  system Python runs n64rx, and n64rx's own engine choice (native or
  Docker) builds; a toolchain bundle is used if one is unpacked.
- **Packaged**: the app carries the source n64rx needs (no tests, no docs,
  nothing from any ROM) and copies it to `<cache>/src/<version>/` on first
  run; builds go to `<cache>/builds` (`N64RX_BUILD_ROOT`). The cache is
  n64rx's: `%LOCALAPPDATA%\n64rx` on Windows, `~/.n64rx` elsewhere.
- **Toolchain**: `<cache>/toolchain/<key>/`, its `manifest.json` giving
  the Python to run and the environment (`MSYS2_ROOT`, `NXDK_DIR`, `LLVM`,
  `N64RX_RECOMP`, PATH entries). `N64RX_TOOLCHAIN=system` skips it;
  `N64RX_TOOLCHAIN_URL` (`file://` too) and `N64RX_TOOLCHAIN_SHA256`
  point the app at another archive.

Nothing leaves the machine but what the user sends to their own Xbox
(Send to Xbox: FTP over the local network, unencrypted, the app says so
under the fields); the toolchain's download from GitHub only comes in.
The home screen's footer says the same in one line.

## How long a build takes

Measured 2026-10-18 on the maintainer's PC (Ryzen 7 5700X3D, 8 cores /
16 threads, 32 GB, NVMe), native engine, release build (ThinLTO), an empty
build folder (the runtime compiled too), the recompiler already built:

| ROM | 1 read + analyse | 2 recompile | 3 compile + link | 4-5 XBE, folder | total |
|---|---|---|---|---|---|
| Super Mario 64 (8 MB) | 16.4 s | 4.2 s | 17.0 s (81 files) | 0.2 s | 38 s |
| Ocarina of Time 1.2 (32 MB, Yaz0) | 22.8 s | 2.2 s | 15.4 s (43 files) | 0.4 s | 41 s |
| Super Mario 64 again (cache) | 13.3 s | 0.1 s | 2.9 s | 0.2 s | 17 s |

Step 1 is Python on one core (the analyser), step 3 is clang on every
core. A mid-range machine (4-6 cores, a few years old) is about 1.5-2x on
step 1 and 2-3x on step 3: roughly 1.5 to 2 minutes; an old dual-core
laptop on a hard disk 3 to 5. `n64rx info` says "about 1 to 3 minutes".
The app's first run adds the toolchain's one-time download.
