# N64Recomp-X, the app

One window over `n64rx` (`tools/n64rx`): drop your own N64 ROM, see whether the
game is Gold, Silver or Experimental and its known problems, choose **Xbox folder
(XBE)** or **Disc image (ISO)**, press Build and say where it goes (the folder picker,
or a save dialog for `<game>.iso`; asked every time, the last place remembered), and
get the folder for your Xbox (`default.xbe` + `default.tbn`, which can be sent
straight to the console by FTP) or a disc image (`n64rx build --iso`). Plain JavaScript on Electron, no
framework, no bundler. The app never does a build step itself: every step is
`n64rx`, run with an argument array (no shell) and read through its
`--events json` lines.

Standalone: no installer. Download, click, run. Nothing registers itself (no
shortcuts, no registry, no auto-start), and everything it writes goes into one
folder next to it, **`N64Recomp-X Data`**: delete that folder to remove
everything.

## First launch

The builds are not signed yet, so each system asks once:

- **Windows**: SmartScreen says "Windows protected your PC": click **More info**,
  then **Run anyway**.
- **macOS**: right-click (or Control-click) `N64Recomp-X.app`, choose **Open**,
  then **Open** again. (Or System Settings > Privacy & Security > **Open Anyway**.)
  Move the app out of Downloads first (to Applications or any folder you own):
  run from Downloads, macOS starts it from a read-only copy and the data folder
  falls back to `~/.n64rx`.
- **Linux**: make the AppImage executable: right-click > Properties >
  Permissions > **Allow executing file as program**, or
  `chmod +x N64Recomp-X-*.AppImage`.

The first time, the app downloads its build tools (about 125 MB on Windows,
unpacked to ~650 MB) into the data folder.

## Run it from the checkout

```
cd app
npm install
npm start                 # the real app against this checkout
npm run fake -- Gold      # the UI with a stand-in n64rx (Gold, Silver, Experimental, Unknown)
npm test                  # node --test: toolchain, JSON lines, paths, the spawn side
npm run test:smoke        # Playwright for Electron: ROM -> badge -> Build -> Done (fake), and the real `n64rx info`
npm run dist              # standalone builds for this OS into app/dist/
```

`npm start`, the tests and `dist` first run `scripts/make-icon.js`, which copies
the project mark (`docs/look/logo/mark.svg`, never edited here) into
`renderer/img/`, records where its stars are for the intro (`renderer/mark-stars.js`)
and renders `build/icon.png` / `.ico` / `.icns` with `@resvg/resvg-js`. All of
them are generated and gitignored.

`node scripts/shots.js <dir> <test ROM>` screenshots every screen with the fake
(the ROM must be one from `tools/xbox/make_test_rom.py`, never a game).

## What `npm run dist` makes

| OS | files in `app/dist/` |
|---|---|
| Windows x64 | `N64Recomp-X.exe` (one portable file) and `N64Recomp-X-<v>-win-x64.zip` (the unpacked folder: starts faster) |
| macOS arm64, x64 | `N64Recomp-X-<v>-mac-<arch>.zip` (the .app) and a `.dmg` |
| Linux x64 | `N64Recomp-X-<v>-x86_64.AppImage` |

electron-builder packs only what one OS can build (macOS needs a Mac). The app
carries a source set of the repo (`scripts/stage-src.js`: `git ls-files` of
`xbox/`, `games/`, `patches/`, `tools/n64rx/`, `tools/xbox/*.py|*.sh`,
`tools/xbox/docker|msys/`, `VERSION`, `LICENSE.md`; no tests, docs, build or
output folders, no ROM extensions) as `resources/src`, about 2 MB. Every staged
file then goes through `tools/xbox/rom_leak_check.py`'s own `scan()` (N64 header
words, generated code, images, ROM extensions); one problem and nothing is staged,
so `npm run dist` stops. N64Recomp's source is not shipped: the
toolchain bundle has it built.

## How the app finds things

| | dev (`npm start`) | packaged |
|---|---|---|
| data folder | the user cache (`%LOCALAPPDATA%\n64rx`, `~/.n64rx`) | `N64Recomp-X Data` next to the exe / the `.app` / the `.AppImage`; the user cache if that is not writable (Program Files, a read-only folder, macOS App Translocation), said in Data and storage |
| source tree (`ROOT`) | this checkout | `resources/src` copied to `<data>/src/<version>/` on first run |
| Python | `N64RX_PYTHON`, else `py -3` / `python` (Windows), `python3` / `python` | the toolchain bundle's |
| toolchain | whatever `n64rx` finds (`engines.pick('auto')`: native or Docker), or a bundle if one is installed | `<data>/toolchain/<key>/`, downloaded on first run |
| builds | `build-xbox/` in the checkout | `<data>/builds` (`N64RX_BUILD_ROOT`) |
| settings, logs, Electron's own files | Electron's userData | `<data>/electron/` |

A toolchain bundle (`tools/xbox/pack_toolchain.py`) has `manifest.json` at its
root: `python`, `env` (MSYS2_ROOT, NXDK_DIR, LLVM, N64RX_RECOMP, relative to the
bundle) and `path` (prepended to PATH), all resolved to absolute paths, plus
`recompiler`, passed as `N64RX_RECOMPILER_KEY` so `n64rx` takes the bundle's
N64Recomp as built for this source. n64rx also gets `N64RX_HOME=<data>`.

Which archive: `toolchain.json`, `{"<platform>-<arch>": {url, sha256, size, key}}`
(the `.json` that `pack_toolchain.py` writes next to its archive has this shape).
The download checks free space first, resumes a stopped download (a `.part`
file and an HTTP Range request), refuses and deletes an archive whose SHA-256
does not match, unpacks `.tar.zst` / `.tar.gz` / `.zip` (Node's zlib, `tar`,
`yauzl`; `.tar.xz` through the system's `tar`) into a scratch folder and renames
it into place.

## Environment

| variable | effect |
|---|---|
| `N64RX_TOOLCHAIN=system` | never use a bundle: the machine's Python and toolchain |
| `N64RX_TOOLCHAIN_URL` (+ `_SHA256`, `_SIZE`, `_KEY`) | download this archive instead of `toolchain.json`'s; `file://` works, for testing a local `pack_toolchain.py` archive |
| `N64RX_PYTHON` | the Python to run `n64rx` with (system mode) |
| `N64RX_HOME` | the data folder (also what `n64rx` uses for its cache) |
| `N64RX_ROOT` | the source tree to run (instead of the checkout or the copy) |
| `N64RX_FAKE=1` | the stand-in `test/fake-n64rx.js` (`N64RX_FAKE_TIER`, `_FAIL`, `_STEP_MS`, `_HOLD`, `_OTHER`); `N64RX_FAKE_TOOLCHAIN=missing` shows the first-run card |
| `N64RX_USER_DATA` | Electron's userData (the tests use a fresh one) |
| `N64RX_TEST_PICK_OUT`, `N64RX_TEST_PICK_ISO` | dev builds only: answer the folder picker / ISO save dialog with this path (or `cancel`), for the tests |
| `N64RX_DEVTOOLS=1` | allow DevTools in a packaged build |

Test a packaged build against a local toolchain:

```
python tools/xbox/pack_toolchain.py            # dist/toolchain/<archive> + <archive>.json
set N64RX_TOOLCHAIN_URL=file:///C:/path/to/n64rx-toolchain-....tar.zst
set N64RX_TOOLCHAIN_SHA256=<sha256 from the .json>
app\dist\win-unpacked\N64Recomp-X.exe
```

## Releasing

Fill `toolchain.json` from each OS's `pack_toolchain.py` `.json` (the archives
uploaded to a release the public can reach), then `npm run dist` per OS. Updates:
`electron-updater` is wired but stays off until `build.publish` is set in
`package.json` (e.g. `{"provider": "github", "owner": "TechProGabe", "repo":
"N64Recomp-X"}`); a portable exe cannot update itself, the zip/AppImage/mac zip can.

## Security

`contextIsolation`, no `nodeIntegration`, a sandboxed renderer, a strict CSP (no
inline script or style, no network), pages served from an `app://` scheme,
navigation and new windows blocked, every permission request refused. The
preload exposes a dozen calls; the main process checks every argument (paths
exist and are absolute, the Xbox address is an IP or host name, the Xbox folder
is a plain path). The FTP password goes to `n64rx deploy` by environment
(`N64RX_FTP_PASS`), never on its command line or in a log, and is stored only
encrypted by the OS (Electron `safeStorage`); where the OS offers no encryption
it is kept for the session only. Settings are checked on load and written whole
(temp file + rename). One instance runs at a time (a second launch focuses the
first), and one job at a time (a build, a send, an ISO or the tools download).

## Files

```
main.js              window, IPC, settings, the data folder, jobs
preload.js           the renderer's API
src/paths.js         data folder, source tree, Python, bundle manifest, how n64rx is run
src/n64rx.js         spawn, JSON lines -> events, cancel (the whole process tree)
src/jsonlines.js     the line parser
src/toolchain.js     download, resume, verify, unpack, install
src/validate.js      IPC argument checks
src/jobs.js          one job at a time, claimed before any await
src/settings.js      checked, atomic settings; the password via safeStorage
renderer/            index.html, style.css, galaxy.js (the sky), ui.js (the screens), icons.js, fonts/unscii-16.woff
scripts/             stage-src.js (with the leak check), make-icon.js, shots.js, run-fake.js
test/                node --test files, fake-n64rx.js, smoke.e2e.js
toolchain.json       the archive per OS
```

unscii-16 is by viznut, public domain (http://viznut.fi/unscii/), the same
font as the loading screen.
