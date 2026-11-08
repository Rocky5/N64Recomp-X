**Experimental.** N64 games running natively on the original Xbox, built from your own ROM. Tiers and known problems per game: [`docs/compat.md`](https://github.com/TechProGabe/N64Recomp-X/blob/main/docs/compat.md).

## Download

| your computer | file |
|---|---|
| Windows (64-bit) | `N64Recomp-X-<version>-win-x64.zip` |
| macOS (Apple silicon) | `N64Recomp-X-<version>-mac-arm64.dmg` or `.zip` |
| Linux (x86-64) | `N64Recomp-X-<version>-x86_64.AppImage` |

The build tools are inside, so there's nothing else to install and building needs no network. Intel Macs aren't supported.

## Three steps

1. Unzip it to a folder you own (not Program Files, not inside the zip) and open the app. On Linux, make the AppImage executable first: `chmod +x N64Recomp-X-*.AppImage`.
2. Drop your ROM on the window, pick **Xbox folder (XBE)** or **Disc image (ISO)**, then press **Build**.
3. Copy the folder to your Xbox (`E:\Games\`) and start `default.xbe`, use **Send to Xbox**, or burn the ISO.

The app isn't signed yet:
- **Windows**: SmartScreen shows a warning. Click **More info**, then **Run anyway**.
- **macOS**: move the app out of Downloads, right-click it, choose **Open**, then **Open** again.

No game is included. Nothing from any ROM ships here, and the app uploads nothing except what you send to your own Xbox. Check your download against `SHA256SUMS.txt`.
