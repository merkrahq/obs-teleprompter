# Building the OBS Docked Teleprompter plugin

Native OBS plugin (C++/Qt6, CMake + libobs + the OBS frontend API). This is the
current primary artifact; the single-file `index.html` is retained only as the
reference/fallback implementation (see `GOAL.md`).

## Prerequisites (Linux / Ubuntu 24.04)

```sh
sudo apt-get install -y cmake ninja-build build-essential pkg-config \
  obs-studio libobs-dev qt6-base-dev qt6-base-private-dev
```

This provides: OBS Studio 30.x, `libobs-dev` (with the `libobs` and
`obs-frontend-api` CMake config packages under
`/usr/lib/x86_64-linux-gnu/cmake/`), Qt6, CMake ≥ 3.16, and Ninja.

> Windows / macOS toolchains and packaging are phase **002.D**. The CMake and
> source here are already cross-platform; only packaging/signing differs per OS.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
# → build/obs-teleprompter.so
```

## Install for local testing (no root)

Drop the module into the per-user OBS plugin path:

```sh
DEST=~/.config/obs-studio/plugins/obs-teleprompter
mkdir -p "$DEST/bin/64bit" "$DEST/data/locale"
cp build/obs-teleprompter.so "$DEST/bin/64bit/"
cp data/locale/en-US.ini      "$DEST/data/locale/"
```

For a system-wide install (root), the module goes flat into the OBS plugins
libdir, e.g. `/usr/lib/x86_64-linux-gnu/obs-plugins/obs-teleprompter.so`
(`cmake --install build` uses `OBS_PLUGIN_DESTINATION`, default
`<libdir>/obs-plugins`).

## Verify it loads (headless smoke check)

On a machine without a display, OBS can be exercised under Xvfb; check the log
for a clean module load and dock registration:

```sh
timeout 45 xvfb-run -a -s "-screen 0 1920x1080x24 +extension GLX +render" \
  env LIBGL_ALWAYS_SOFTWARE=1 QT_QPA_PLATFORM=xcb obs --disable-updater
grep -i teleprompter "$(ls -t ~/.config/obs-studio/logs/*.txt | head -1)"
```

Expected log lines:

```
Loading module: obs-teleprompter.so
[obs-teleprompter] loaded (version 0.1.0)
[obs-teleprompter] dock 'obs_teleprompter_dock' registered
```

On a real desktop OBS, the **Teleprompter** dock then appears automatically
under **Docks** in the OBS menu bar — no Custom Browser Dock, no URL, no
WebSocket configuration.
