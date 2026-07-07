# OBS Docked Teleprompter

A free, native **OBS Studio plugin** that adds a teleprompter dock right inside OBS.
Install it once, launch OBS, and the **Teleprompter** dock is already there — no Custom
Browser Dock setup, no `file://` URL, no WebSocket host/port/password to configure.

Paste a script, press **Start** — a countdown runs, OBS starts recording automatically, and
the script scrolls smoothly. Press **Stop** and it halts both the scroll and the recording
together. Everything is local; nothing is sent anywhere.

---

## Features

- 🎬 **Countdown → auto-record → auto-scroll.** Start runs a configurable countdown (0/3/5/10s),
  then OBS starts recording and the script begins scrolling the instant recording is confirmed.
- 🔌 **In-process OBS control.** Recording is driven directly through the OBS frontend API — no
  WebSocket, no connection to configure. It detects when OBS is already recording and reuses that
  session instead of double-starting, and it never starts scrolling on an unconfirmed recording.
- ✍️ **Paste-and-go script editor** with automatic local save/restore. Handles long scripts.
- 🎛️ **Live adjustable** font size, scroll speed, and line height. Smooth (sub-pixel) scrolling.
- ⏱️ Estimated reading time and word count.
- 🌙 Dark, high-contrast theme with an optional **center guide line**.
- ⌨️ Keyboard shortcuts: **Ctrl+Enter** = Start, **Space** = Pause/Resume, **Esc** = Stop.
- 💾 Everything persists locally across OBS restarts (script, font, speed, line height, countdown,
  layout) — in the plugin's config dir, no cloud, no accounts.

## Requirements

- **OBS Studio 28 or newer** (Qt6 build). That's it — the teleprompter is a native dock once
  installed.

## Install

Download the installer for your OS from the [**Releases**](https://github.com/merkrahq/obs-teleprompter/releases)
page and run it. After installing, (re)launch OBS Studio — the **Teleprompter** dock appears
automatically under the **Docks** menu.

| OS | File | Install |
|---|---|---|
| **Linux** | `obs-teleprompter-*-Linux.deb` | `sudo dpkg -i obs-teleprompter-*-Linux.deb` (or `sudo apt install ./obs-teleprompter-*-Linux.deb` to pull deps). A `.tar.gz` is also provided — extract it over `/usr`. |
| **Windows** | `obs-teleprompter-*-win64.exe` | Run the installer. |
| **macOS** | `obs-teleprompter-*-macOS.pkg` | Open the `.pkg` and follow the installer. |

### ⚠️ Unsigned installers — one-time OS override

These installers are currently **unsigned** (code-signing certificates aren't held yet), so your
OS may warn you the first time:

- **Windows / SmartScreen:** if you see "Windows protected your PC," click **More info → Run
  anyway**.
- **macOS / Gatekeeper:** if the `.pkg` is blocked, **right-click it → Open** (instead of
  double-clicking), or clear the quarantine flag:
  `xattr -dr com.apple.quarantine obs-teleprompter-*-macOS.pkg`.

Linux `.deb`/`.tar.gz` need no such override. Signing/notarization will be added once certs are
in place; until then these steps are expected and safe for artifacts you downloaded from the
official Releases page.

## Use

1. Open the **Teleprompter** dock (Docks → Teleprompter if it isn't already visible).
2. Open **✎ Script** and paste your script.
3. Adjust font size / scroll speed / line height / countdown to taste.
4. Press **▶ Start** (or Ctrl+Enter). Countdown → OBS records → the script scrolls.
5. **Space** pauses/resumes; **⏹ Stop** (or Esc) stops scrolling *and* recording.

If OBS is already recording when you press Start, the teleprompter reuses that recording rather
than starting a second one.

## Building from source

See [`BUILD.md`](./BUILD.md). In short (Linux):

```sh
sudo apt-get install -y cmake ninja-build build-essential \
  obs-studio libobs-dev qt6-base-dev qt6-base-private-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
cd build && cpack               # → obs-teleprompter-*-Linux.deb + .tar.gz
```

The per-OS installers are produced in CI by
[`.github/workflows/build.yml`](./.github/workflows/build.yml) (Linux `.deb`/`.tar.gz`, Windows
NSIS `.exe`, macOS `.pkg`) and published to Releases on a version tag.

## Privacy

Fully local. No accounts, no telemetry, no cloud. The plugin controls OBS in-process (no network
connection at all) and stores its settings in OBS's local plugin config directory.

## Reference / fallback: the single-file browser dock

The repo also contains [`index.html`](./index.html) — the original single-file browser-dock
version that controls OBS over **OBS WebSocket v5**. The native plugin above is the primary,
recommended way to use the teleprompter; `index.html` is retained as a zero-install reference and
fallback (e.g. for an OBS build without the plugin). It works as a **Docks → Custom Browser
Docks…** entry pointed at the local file.

## License

[MIT](./LICENSE) — free and open source.
