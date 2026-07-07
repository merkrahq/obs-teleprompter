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

> Windows / macOS toolchains and packaging run in CI (see **CI** below). The
> CMake and source here are already cross-platform; only the per-OS install
> destinations, packaging generators, and signing differ.

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

## Packaging (installers)

CPack produces the per-OS installer from a completed build. Run it from the
build dir; the generators are chosen automatically per OS in `CMakeLists.txt`:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
cd build && cpack
```

- **Linux** → `obs-teleprompter-*-Linux.deb` + `.tar.gz`. The `.deb` drops the
  module into the multiarch OBS plugins dir
  (`/usr/lib/<arch>/obs-plugins/obs-teleprompter.so`) and the locale into
  `/usr/share/obs/obs-plugins/obs-teleprompter/locale/` — the exact paths a
  stock Debian/Ubuntu OBS scans — so the dock auto-appears after
  `sudo dpkg -i`. `shlibdeps` pins the libobs/Qt6 runtime deps. **Verified
  end-to-end on this box:** installed the `.deb` system-wide, launched headless
  OBS 30, and confirmed the log shows the module loaded + `dock ... registered`
  with no errors.
- **Windows** → `NSIS` `.exe` + `.zip` (CI-only).
- **macOS** → `productbuild` `.pkg` + `.tar.gz` (CI-only).

Installers are **unsigned** for now (decision `ship-unsigned-installers-first`):
the Windows `.exe` trips SmartScreen and the macOS `.pkg` trips Gatekeeper until
signing certs exist — the README documents the one-time override.

## CI (all three OSes)

`.github/workflows/build.yml` builds + packages on `ubuntu-latest`,
`windows-latest`, and `macos-latest`, uploads the installers as workflow
artifacts on every push/PR, and publishes them to a **GitHub Release** on a
`v*` tag (decision `installers-via-github-ci`).

- **Linux** builds straight from the apt toolchain above — the same, locally
  verified path.
- **Windows/macOS** need the OBS SDK (the `OBS::libobs` /
  `OBS::obs-frontend-api` CMake config packages), which the GitHub runners don't
  ship. The workflow checks out the matching `obsproject/obs-studio` source and
  configures/installs its `libobs` + `obs-frontend-api` targets into a local
  prefix, then points our build at it via `CMAKE_PREFIX_PATH`. That OBS
  configure step depends on the **OBS deps bundle** (`obs-deps`) for its own
  third-party libraries; if a runner lacks it the step emits a `::warning::` and
  the SDK build must be completed with the obs-deps setup (the standard
  `obs-plugintemplate` CI pattern) before the Windows/macOS artifacts build
  clean. Only the Linux artifact is proven locally on this Linux-only dev box;
  the Windows/macOS legs are validated when the runners first execute.

To cut a release: `git tag v0.1.0 && git push origin v0.1.0` → the `release`
job gathers all three OSes' artifacts into one GitHub Release.
