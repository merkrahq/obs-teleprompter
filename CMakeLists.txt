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
  # Windows: install into OBS's NESTED plugin drop-in layout, the same tree the
  # portable zip stages —
  #     <plugin-root>/bin/64bit/obs-teleprompter.dll
  #     <plugin-root>/data/locale/en-US.ini
  # OBS scans exactly TWO Windows plugin locations (00003.a, GO-009):
  #   1. %ProgramData%\obs-studio\plugins  — NESTED (<name>\bin\64bit + <name>\
  #      data\locale). Modern PREFERRED manual-install path; scanned regardless of
  #      where OBS itself is installed. This is our PRIMARY target.
  #   2. <obs-install>\obs-plugins\64bit   — FLAT (<name>.dll directly + data in
  #      <obs>\data\obs-plugins\<name>). Legacy ("will stop working in a future
  #      version" per OBS docs) + needs admin in Program Files. Registry-discovered
  #      FALLBACK only, when ProgramData isn't writable.
  # NOTE (GO-009): %APPDATA%\obs-studio\plugins (Roaming) is NOT scanned by OBS —
  # v0.1.1/v0.1.2 targeted it and the dock never loaded there even with clean
  # backslashes (GO-008). The %ProgramData% target below replaces it.
  #
  # These DESTINATIONs are RELATIVE and produce the NESTED tree; the NSIS
  # PREINSTALL block below sets the absolute $INSTDIR to %ProgramData%\obs-studio\
  # plugins\obs-teleprompter (nested — correct as-is). The flat registry fallback
  # relocates the two files itself (see CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS).
  #
  # GO-005: relative dests must never re-inherit NSIS's default $PROGRAMFILES
  # prefix — the PREINSTALL block sets an ABSOLUTE $INSTDIR before FULL_INSTALL.
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
# Escape CPACK_* values correctly when CPack re-serializes them into
# CPackConfig.cmake (which cpack then re-parses). Without this, a value like the
# Windows install root "$APPDATA\obs-studio\plugins" is written verbatim and its
# `\o`/`\p` become invalid CMake escapes on the re-parse ("Syntax error in cmake
# code") — the reason we previously had to use forward slashes (which then leaked
# into the installed path as ugly MIXED slashes and OBS failed to scan it).
# CPACK_VERBATIM_VARIABLES makes CPack escape the backslashes so a single clean
# backslash survives both parse cycles into the generated .nsi. This is a no-op
# for the Linux/macOS values (no backslashes), so those legs are unaffected.
set(CPACK_VERBATIM_VARIABLES TRUE)
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

  # ── Install target: %ProgramData% first, registry-discovered OBS tree fallback
  #    (00003.a; operator decision 2026-07-07). ─────────────────────────────────
  # The static InstallDir CPack bakes from CPACK_NSIS_INSTALL_ROOT \
  # CPACK_PACKAGE_INSTALL_DIRECTORY is only a DEFAULT placeholder — the PREINSTALL
  # block below OVERRIDES $INSTDIR at install time (InstallDir is evaluated before
  # the Section's SetShellVarContext, so it can't itself resolve %ProgramData%;
  # NSIS-forum-confirmed). We still set the default to the %ProgramData% path (via
  # SetShellVarContext all → $APPDATA = C:\ProgramData) so a fresh checkout without
  # the PREINSTALL logic degrades to the right place, and so the CI assert has a
  # concrete backslash string to check. BACKSLASHES + CPACK_VERBATIM_VARIABLES
  # keep GO-008 fixed (forward slashes leak as a mixed-slash path OBS won't scan).
  set(CPACK_NSIS_INSTALL_ROOT "$APPDATA\\obs-studio\\plugins")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}")
  set(CPACK_NSIS_DEFINES "RequestExecutionLevel user")

  # PREINSTALL runs at the top of the install Section, BEFORE @CPACK_NSIS_FULL_INSTALL@
  # copies the nested payload (bin\64bit + data\locale) into $INSTDIR. We use it to
  # pick the real target:
  #   1. %ProgramData%\obs-studio\plugins\obs-teleprompter  (NESTED — FULL_INSTALL's
  #      layout is correct as-is). Chosen when writable (the common case: the
  #      obs-studio subdir already exists once OBS has run, so a standard user can
  #      write it — no admin/UAC).
  #   2. else registry-discover <obs> (HKLM uninstall key InstallLocation, native +
  #      WOW6432Node + 32-bit view) and if <obs>\obs-plugins\64bit exists + is
  #      writable, stash it in $R0. FULL_INSTALL still drops the nested tree into the
  #      ProgramData staging $INSTDIR; the POST block (EXTRA_INSTALL_COMMANDS) then
  #      copies the two files FLAT into the OBS tree (that legacy dir wants
  #      <name>.dll directly + data under <obs>\data\obs-plugins\<name>).
  # Writability is TESTED (FileOpen w), never assumed — admin is never required and
  # UAC is never prompted (RequestExecutionLevel user). GO-005: $INSTDIR is ABSOLUTE,
  # so nothing re-inherits NSIS's default $PROGRAMFILES prefix.
  # NSIS-escaping recipe (verified via a CPackConfig round-trip probe): use LITERAL
  # newlines between commands, `\"` for quotes, `\\` for path backslashes, and a
  # plain `$` for NSIS vars ($INSTDIR/$APPDATA/$0/$9/$R0). Under
  # CPACK_VERBATIM_VARIABLES the `$` serializes to `\$` in CPackConfig.cmake and
  # unescapes back to a clean `$` when cpack re-parses + substitutes into the .nsi.
  # (The `$\r$\n` NSIS newline construct does NOT survive the CMake layer — CMake
  # eats the `\r`/`\n`; literal newlines are the correct carrier.)
  set(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS
"SetShellVarContext all
  StrCpy $INSTDIR \"$APPDATA\\obs-studio\\plugins\\obs-teleprompter\"
  StrCpy $R0 \"\"
  CreateDirectory \"$APPDATA\\obs-studio\\plugins\"
  ClearErrors
  FileOpen $9 \"$APPDATA\\obs-studio\\plugins\\obsprompt_wtest.tmp\" w
  IfErrors obsprompt_try_reg
  FileClose $9
  Delete \"$APPDATA\\obs-studio\\plugins\\obsprompt_wtest.tmp\"
  Goto obsprompt_target_done
  obsprompt_try_reg:
  SetRegView 64
  ReadRegStr $0 HKLM \"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OBS Studio\" \"InstallLocation\"
  StrCmp $0 \"\" 0 obsprompt_have_obs
  ReadRegStr $0 HKLM \"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OBS Studio\" \"InstallLocation\"
  StrCmp $0 \"\" 0 obsprompt_have_obs
  SetRegView 32
  ReadRegStr $0 HKLM \"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OBS Studio\" \"InstallLocation\"
  StrCmp $0 \"\" obsprompt_target_done 0
  obsprompt_have_obs:
  IfFileExists \"$0\\obs-plugins\\64bit\\*.*\" 0 obsprompt_target_done
  ClearErrors
  FileOpen $9 \"$0\\obs-plugins\\64bit\\obsprompt_wtest.tmp\" w
  IfErrors obsprompt_target_done
  FileClose $9
  Delete \"$0\\obs-plugins\\64bit\\obsprompt_wtest.tmp\"
  StrCpy $R0 $0
  StrCpy $INSTDIR \"$TEMP\\obs-teleprompter-stage\"
  obsprompt_target_done:
  SetOutPath \"$INSTDIR\"")

  # POST-install: $R0 is non-empty ONLY when ProgramData was NOT writable but a
  # writable OBS install tree WAS discovered (the rare legacy path). In that case
  # PREINSTALL staged the nested payload into $TEMP\obs-teleprompter-stage; copy the
  # two files FLAT into the OBS tree (that legacy dir wants <name>.dll directly +
  # data under <obs>\data\obs-plugins\<name>), then wipe the temp stage so no cruft
  # (or a stray temp uninstaller) is left behind. When ProgramData WAS writable, $R0
  # is empty and this is a no-op — the nested ProgramData install stands.
  set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
"StrCmp $R0 \"\" obsprompt_post_done 0
  CreateDirectory \"$R0\\obs-plugins\\64bit\"
  CopyFiles /SILENT \"$INSTDIR\\bin\\64bit\\obs-teleprompter.dll\" \"$R0\\obs-plugins\\64bit\"
  CreateDirectory \"$R0\\data\\obs-plugins\\obs-teleprompter\\locale\"
  CopyFiles /SILENT \"$INSTDIR\\data\\locale\\en-US.ini\" \"$R0\\data\\obs-plugins\\obs-teleprompter\\locale\"
  RMDir /r \"$TEMP\\obs-teleprompter-stage\"
  obsprompt_post_done:")
  # Uninstaller runs in the all-users context so it resolves the same %ProgramData%
  # $APPDATA paths the installer wrote to.
  set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "SetShellVarContext all")
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
