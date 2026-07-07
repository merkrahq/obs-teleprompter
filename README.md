# OBS Docked Teleprompter

A free, single-file teleprompter that lives **inside OBS Studio** as a custom browser dock.
Paste a script, press **Start** — a countdown runs, OBS starts recording automatically, and the
script scrolls smoothly. Press **Stop** and it halts both the scroll and the recording together.

No install, no build step, no accounts, no cloud. One HTML file. Everything is stored locally in
your browser.

---

## Features

- 🎬 **Countdown → auto-record → auto-scroll.** Start runs a configurable countdown (0/3/5/10s),
  then tells OBS to start recording and begins scrolling the instant recording is confirmed.
- 🔌 **Native OBS control** via OBS WebSocket v5 — connection status, and it detects if OBS is
  already recording (and reuses that session instead of double-starting).
- ✍️ **Paste-and-go script editor** with automatic local save/restore. Handles long scripts.
- 🎛️ **Live adjustable** font size, scroll speed, and line height. Smooth (sub-pixel) scrolling.
- ⏱️ Estimated reading time and word count.
- 🌙 Dark, high-contrast theme with an optional **center guide line**.
- 🎚️ Optional **dock opacity** control (dim the dock).
- ⌨️ Keyboard shortcuts: **Ctrl+Enter** = Start, **Space** = Pause/Resume, **Esc** = Stop.
- 💾 Everything persists locally (script, font, speed, line height, countdown, OBS host/port/password).

## Requirements

- **OBS Studio 28 or newer** (ships with OBS WebSocket v5 built in).
- OBS WebSocket enabled: **Tools → WebSocket Server Settings → Enable WebSocket server**
  (note the port — default `4455` — and password if you set one).

## Install (as an OBS Custom Browser Dock)

1. Download `index.html` from this repo (or clone it).
2. In OBS: **Docks → Custom Browser Docks…**
3. Give it a name (e.g. `Teleprompter`) and set the **URL** to the local file, e.g.
   `file:///home/you/obs-teleprompter/index.html` (Windows: `file:///C:/path/to/index.html`).
4. Click **Apply**. The teleprompter appears as a dock — drag it into the OBS dock grid next to
   your other docks, or float it. It behaves like any other OBS dock.

> Tip: if `file://` gives you trouble, serve the folder from any static web server
> (`python3 -m http.server`) and point the dock at `http://localhost:8000/index.html`.

## Use

1. Open the **⚙ Settings** panel and enter your OBS host/port/password, then **Connect**
   (or tick **Auto** to connect on load). The status bar turns green when connected.
2. Open **✎ Script**, paste your script.
3. Adjust font size / scroll speed / line height / countdown to taste.
4. Press **▶ Start** (or Ctrl+Enter). Countdown → OBS records → the script scrolls.
5. **Space** pauses/resumes; **⏹ Stop** (or Esc) stops scrolling *and* recording.

## Notes on the "transparent floating dock"

You can dim the dock with the **Dock opacity** slider. True *see-through-to-desktop* window
transparency is not something a browser dock can control — OBS renders docks on an opaque widget —
so this dims the dock's content rather than making the window itself transparent.

## Privacy

Fully local. No accounts, no telemetry, no network calls except the OBS WebSocket connection you
configure. Settings live only in your browser's `localStorage`.

## License

[MIT](./LICENSE) — free and open source.
