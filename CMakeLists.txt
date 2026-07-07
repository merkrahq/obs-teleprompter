cmake_minimum_required(VERSION 3.16...3.30)

# ── OBS Docked Teleprompter — native OBS plugin ──────────────────────────────
# Builds a libobs plugin that registers a Qt6 teleprompter dock via the OBS
# frontend API and drives recording in-process. The install rules + CPack config
# below produce per-OS installers (002.D) so the dock auto-appears after install:
#   Linux   → .deb / .tar.gz  (buildable + verifiable on the dev box)
#   Windows → NSIS installer  (CI-only; no local Windows toolchain)
#   macOS   → .pkg / .tar.gz  (CI-only; no local macOS toolchain)
#
# Modeled on the official obs-plugintemplate pattern, simplified to a single
# self-contained CMakeLists. The source is already cross-platform; only the
# per-OS install destinations and packaging differ.

project(obs-teleprompter VERSION 0.1.0)

option(ENABLE_FRONTEND_API "Use the OBS frontend API (required for the dock)" ON)
option(ENABLE_QT "Use Qt functionality (required for the dock)" ON)

add_library(${PROJECT_NAME} MODULE)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  PLUGIN_VERSION="${PROJECT_VERSION}")

target_sources(
  ${PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/teleprompter-dock.cpp
    src/teleprompter-dock.hpp)

# ── libobs (always required) ─────────────────────────────────────────────────
find_package(libobs REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs)

# macOS: OBS's imported libobs framework drags in the legacy `AGL` framework
# through its config, but AGL was removed from modern macOS SDKs
# (`ld: framework 'AGL' not found`). We don't use any AGL symbols. Rather than
# chase the exact CMake property that injects it (it arrives via the framework's
# libobsConfig, in a form that's awkward to filter), we stage a tiny empty `AGL`
# stub framework in the build tree and add it to the linker's framework search
# path, so `-framework AGL` resolves to a no-op. This keeps the fix local and
# robust across OBS SDK versions.
if(APPLE)
  set(_agl_stub_dir "${CMAKE_BINARY_DIR}/stub-frameworks")
  set(_agl_fw "${_agl_stub_dir}/AGL.framework")
  file(MAKE_DIRECTORY "${_agl_fw}")
  # An empty Mach-O dylib named `AGL` is enough for the linker to find the
  # framework; we reference no symbols from it.
  if(NOT EXISTS "${_agl_fw}/AGL")
    execute_process(
      COMMAND ${CMAKE_CXX_COMPILER} -x c -dynamiclib -o "${_agl_fw}/AGL"
              -install_name "@rpath/AGL.framework/AGL" -
      INPUT_FILE /dev/null
      RESULT_VARIABLE _agl_rc)
    if(NOT _agl_rc EQUAL 0)
      # Fallback: empty file — the framework dir presence alone often satisfies
      # the search; if not, the build log will show it and we adjust.
      file(TOUCH "${_agl_fw}/AGL")
    endif()
  endif()
  target_link_options(${PROJECT_NAME} PRIVATE "-F${_agl_stub_dir}")
endif()

# ── OBS frontend API (dock registration, recording control) ──────────────────
if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
  target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_FRONTEND_API)
endif()

# ── Qt6 (dock widget) ────────────────────────────────────────────────────────
if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
  target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_definitions(${PROJECT_NAME} PRIVATE ENABLE_QT)
  set_target_properties(
    ${PROJECT_NAME}
    PROPERTIES AUTOMOC ON
               AUTOUIC ON
               AUTORCC ON)
  # Silence the "Qt requires C++17" note and match OBS's C++ level.
  target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

# On macOS the module is packaged INSIDE a `.plugin` bundle, whose Mach-O in
# Contents/MacOS/ must be named after the bundle (obs-teleprompter, no .so) for
# OBS to load it. Drop the `.so` suffix on macOS only — Linux keeps `.so` and
# Windows keeps `.dll`, which is what OBS scans for on those platforms.
if(APPLE)
  set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX "")
endif()

# ── Per-OS install layout ────────────────────────────────────────────────────
# Each OS has its own OBS plugin directory convention. Installing into these
# paths is what makes the dock auto-appear on next OBS launch — no Custom Browser
# Dock, no manual setup. The locale .ini must sit beside the module in the
# OS-appropriate data dir or OBS_MODULE_USE_DEFAULT_LOCALE can't load it.
include(GNUInstallDirs)

if(APPLE)
  # macOS: OBS loads .plugin bundles from the OBS plugins dir. CPack (below) and
  # the CI step assemble the bundle layout; here we install the raw module +
  # data so the bundle can be staged. Bundle name matches the module.
  set(_obs_plugin_root "${CMAKE_INSTALL_PREFIX}")
  # Assign the install to an explicit COMPONENT. CPack's productbuild generator
  # only writes <pkg-ref>/<choice> entries — and thus a non-empty
  # <choices-outline> — for NAMED components. With the default "Unspecified"
  # component, productbuild builds the component .pkg (it is ~31 KB, non-empty)
  # but emits an EMPTY <choices-outline/>, so the final product archive
  # references nothing and installs NOTHING (the empty-.pkg bug). Naming the
  # component + listing it in CPACK_COMPONENTS_ALL (below) fixes the outline.
  install(TARGETS ${PROJECT_NAME}
          LIBRARY DESTINATION "${PROJECT_NAME}.plugin/Contents/MacOS"
          COMPONENT plugin)
  install(DIRECTORY data/locale
          DESTINATION "${PROJECT_NAME}.plugin/Contents/Resources"
          COMPONENT plugin)
elseif(WIN32)
  # Windows: install into OBS's PER-USER plugin drop-in layout, the same tree the
  # portable zip stages —
  #     <plugin-root>/bin/64bit/obs-teleprompter.dll
  #     <plugin-root>/data/locale/en-US.ini
  # The NSIS block below roots <plugin-root> at %APPDATA%\obs-studio\plugins\
  # obs-teleprompter (per-user, no admin) via CPACK_NSIS_INSTALL_ROOT +
  # CPACK_PACKAGE_INSTALL_DIRECTORY, so the final paths are exactly where OBS
  # scans for user plugins. (These DESTINATIONs are RELATIVE; the CPack install
  # root supplies the absolute base.)
  #
  # GO-005: the previous relative dests `obs-plugins/64bit` + `data/obs-plugins/
  # <name>` inherited NSIS's default $PROGRAMFILES prefix, landing everything
  # under `C:\Program Files\OBS Docked Teleprompter\obs-plugins\64bit\...` — a
  # path OBS never scans, so the dock silently never loaded. Matching the
  # portable `bin/64bit`+`data` layout fixes both the .exe and the .zip.
  install(TARGETS ${PROJECT_NAME}
          RUNTIME DESTINATION "bin/64bit"
          LIBRARY DESTINATION "bin/64bit")
  install(DIRECTORY data/locale
          DESTINATION "data")
else()
  # Linux: system OBS loads flat .so modules from the *multiarch* libdir's
  # obs-plugins/ subdir and reads data from <datadir>/obs/obs-plugins/<name>/.
  # This matches the stock OBS layout on Debian/Ubuntu (verified: OBS scans
  # /usr/lib/x86_64-linux-gnu/obs-plugins and /usr/share/obs/obs-plugins/%module%).
  #
  # IMPORTANT: under CPack's /usr install prefix, GNUInstallDirs collapses
  # CMAKE_INSTALL_LIBDIR to a bare "lib", which lands the module in
  # /usr/lib/obs-plugins — a path OBS does NOT scan on multiarch distros, so the
  # dock would silently never load. We therefore pin the module dir to the
  # multiarch tuple (CMAKE_LIBRARY_ARCHITECTURE, e.g. x86_64-linux-gnu) so the
  # .deb drops the .so exactly where OBS looks.
  if(NOT DEFINED OBS_PLUGIN_DESTINATION)
    if(CMAKE_LIBRARY_ARCHITECTURE)
      set(OBS_PLUGIN_DESTINATION "lib/${CMAKE_LIBRARY_ARCHITECTURE}/obs-plugins")
    else()
      set(OBS_PLUGIN_DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
    endif()
  endif()
  if(NOT DEFINED OBS_DATA_DESTINATION)
    set(OBS_DATA_DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${PROJECT_NAME}")
  endif()
  install(TARGETS ${PROJECT_NAME}
          LIBRARY DESTINATION "${OBS_PLUGIN_DESTINATION}")
  install(DIRECTORY data/locale
          DESTINATION "${OBS_DATA_DESTINATION}")
endif()

# ── Packaging (CPack) ────────────────────────────────────────────────────────
# `cpack -G <gen>` from the build dir produces the installer for the current OS.
# CI (.github/workflows/build.yml) runs the matching generator per runner and
# uploads the artifact to the GitHub Release. Artifacts are UNSIGNED (decision
# `ship-unsigned-installers-first`); the README documents the SmartScreen /
# Gatekeeper override until signing certs exist.
set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VENDOR "merkrahq")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "OBS Docked Teleprompter — native teleprompter dock with in-process recording")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/merkrahq/obs-teleprompter")
set(CPACK_PACKAGE_CONTACT "admin@merkra.com")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  # CPack's macOS productbuild + Windows NSIS generators reject a license file
  # whose extension isn't one of .txt/.rtf/.html ("CPack Error: Bad file
  # extension specified"). Our LICENSE has no extension, so stage a .txt copy in
  # the build tree and point CPack at that.
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_BINARY_DIR}/LICENSE.txt"
    COPYONLY)
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")
endif()

# ── Deterministic, version-free artifact names ───────────────────────────────
# The README links to https://github.com/merkrahq/obs-teleprompter/releases/latest/download/<name>,
# whose redirect always resolves to the newest release's asset. For those links
# to be stable across version bumps, the artifact filenames must NOT embed the
# version. We therefore pin CPACK_PACKAGE_FILE_NAME per OS/arch. (The DEB
# generator otherwise re-derives its own <pkg>_<ver>_<arch>.deb name, so it is
# pinned explicitly too via CPACK_DEBIAN_FILE_NAME.)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(_arch "arm64")
else()
  set(_arch "x86_64")
endif()

if(APPLE)
  set(CPACK_GENERATOR "productbuild;TGZ")
  # macOS is shipped as a universal-ish single bundle; arch omitted from the name.
  set(CPACK_PACKAGE_FILE_NAME "obs-teleprompter-macos")
  # Install the .plugin bundle into the OBS plugin dir, not the filesystem root
  # (GO-005/D-002 — same relative-dest defect class as Windows). The install()
  # rules stage `<name>.plugin/Contents/...` relative to this prefix, so the
  # bundle lands at <prefix>/obs-teleprompter.plugin/. UNVERIFIED (no Mac in the
  # lab): the operator must install the CI .pkg and confirm the dock appears.
  #
  # Per-user install (~/Library, no admin) via productbuild's user domain. If the
  # user-domain install proves unreliable on the operator's macOS, the fallback
  # is a system install at /Library/Application Support/obs-studio/plugins (set
  # CPACK_PACKAGING_INSTALL_PREFIX to that absolute path and drop the domain vars
  # — that one needs admin but is the most broadly-scanned location).
  set(CPACK_PACKAGING_INSTALL_PREFIX "/Library/Application Support/obs-studio/plugins")
  set(CPACK_PRODUCTBUILD_DOMAINS ON)
  set(CPACK_PRODUCTBUILD_DOMAINS_USER ON)
  set(CPACK_PRODUCTBUILD_DOMAINS_ROOT OFF)
  # Register the named component so productbuild writes a real <choices-outline>
  # referencing the (already non-empty) component .pkg. Without this the product
  # archive's outline is empty and the .pkg installs nothing (see the install()
  # COMPONENT note above). One component, packaged into one product archive.
  set(CPACK_COMPONENTS_ALL plugin)
  set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
  set(CPACK_COMPONENT_PLUGIN_DISPLAY_NAME "OBS Teleprompter plugin")
elseif(WIN32)
  set(CPACK_GENERATOR "NSIS;ZIP")
  set(CPACK_PACKAGE_FILE_NAME "obs-teleprompter-windows-${_arch}")
  set(CPACK_NSIS_PACKAGE_NAME "OBS Docked Teleprompter")
  set(CPACK_NSIS_DISPLAY_NAME "OBS Docked Teleprompter")
  set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
  # Install into the PER-USER OBS plugin dir, not $PROGRAMFILES (GO-005/D-002).
  # Final path assembled by NSIS is:
  #   CPACK_NSIS_INSTALL_ROOT \ CPACK_PACKAGE_INSTALL_DIRECTORY \ <dest>
  #   = $APPDATA\obs-studio\plugins \ obs-teleprompter \ bin\64bit\obs-teleprompter.dll
  # ($APPDATA is an NSIS builtin → C:\Users\<user>\AppData\Roaming, exactly where
  # OBS scans for user plugins.) The ZIP generator uses the same
  # CPACK_PACKAGE_INSTALL_DIRECTORY, so the .zip contains a ready
  # `obs-teleprompter/bin/64bit`+`data` drop-in tree too.
  # Use FORWARD slashes (NSIS accepts them): with backslashes, CPack
  # re-serializes the value into build/CPackConfig.cmake as "$APPDATA\obs-studio
  # \plugins", where `\o`/`\p` are invalid CMake escapes and fatal the re-parse
  # ("Syntax error in cmake code"). Forward slashes avoid the escaping entirely.
  set(CPACK_NSIS_INSTALL_ROOT "$APPDATA/obs-studio/plugins")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}")
  # Per-user install: run unelevated (no UAC), resolve $APPDATA/shortcuts for the
  # current user, and write the uninstall entry under HKCU (SetShellVarContext
  # current makes CPack's default uninstall registry writes land in the per-user
  # hive instead of HKLM, which would need admin).
  set(CPACK_NSIS_DEFINES "RequestExecutionLevel user")
  set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "SetShellVarContext current")
  set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "SetShellVarContext current")
else()
  set(CPACK_GENERATOR "DEB;TGZ")
  set(CPACK_PACKAGE_FILE_NAME "obs-teleprompter-linux-${_arch}")
  # Install DEB payloads under the absolute system prefix so the .so/data land in
  # the real OBS dirs when the user runs `dpkg -i` (CPack DEB honors the install
  # DESTINATIONs relative to CPACK_PACKAGING_INSTALL_PREFIX).
  set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "merkrahq <admin@merkra.com>")
  set(CPACK_DEBIAN_PACKAGE_SECTION "video")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
  # OBS + Qt6 are runtime deps; shlibdeps fills in the exact libobs/Qt versions.
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "obs-studio (>= 31)")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  # Pin the .deb filename too (the DEB generator ignores CPACK_PACKAGE_FILE_NAME).
  set(CPACK_DEBIAN_FILE_NAME "obs-teleprompter-linux-${_arch}.deb")
endif()

include(CPack)

# ── Portable "drag-into-OBS" plugin folder ───────────────────────────────────
# Separate from the CPack system installers above: this stages the plugin in
# OBS's per-user drop-in layout —
#     obs-teleprompter/bin/64bit/obs-teleprompter.{so,dll}
#     obs-teleprompter/data/locale/en-US.ini
# — which a user can unzip and drop straight into their OBS plugins dir with no
# installer and no admin rights:
#     Linux   → ~/.config/obs-studio/plugins/
#     Windows → %APPDATA%\obs-studio\plugins\
#     macOS   → ~/Library/Application Support/obs-studio/plugins/
# The `portable` target builds this tree under build/portable/; CI zips it into
# obs-teleprompter-<os>-portable.zip and attaches it to the Release beside the
# installers. `README.md` links to it as the drag-in option.
set(_portable_root "${CMAKE_BINARY_DIR}/portable/${PROJECT_NAME}")
add_custom_target(
  portable
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/portable"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_portable_root}/bin/64bit"
  COMMAND ${CMAKE_COMMAND} -E copy
          "$<TARGET_FILE:${PROJECT_NAME}>" "${_portable_root}/bin/64bit/"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/data" "${_portable_root}/data"
  DEPENDS ${PROJECT_NAME}
  COMMENT "Staging portable drag-into-OBS plugin folder → build/portable/${PROJECT_NAME}/"
  VERBATIM)
