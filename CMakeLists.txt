# Host OS services (memory, threads, faults, files): POSIX or Win32.
if(WIN32)
    set(SSA_PLATFORM_SOURCES platform/platform_win32.cpp)
else()
    set(SSA_PLATFORM_SOURCES platform/platform_posix.cpp)
endif()

# The figure and collection library (host/figures without the portraits, host/collection), shared with Skylauncher
# (docs/runtime/host/collection_api.md). The catalogue (resources/collection/catalogue.json) is built in.
set(SSA_CATALOGUE ${CMAKE_SOURCE_DIR}/resources/collection/catalogue.json)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/embedded_catalogue.cpp
    COMMAND ${CMAKE_COMMAND} -DNAME=g_embeddedCatalogue -DINPUT=${SSA_CATALOGUE}
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/embedded_catalogue.cpp -P ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake ${SSA_CATALOGUE}
    COMMENT "Embedding the toy catalogue")
set(SSA_COLLECTION_SOURCES
    host/figures/figure_catalog.cpp
    host/figures/figure_crypto.cpp
    host/figures/figure_data.cpp
    host/figures/figure_tag.cpp
    host/collection/catalogue.cpp
    host/collection/collection.cpp
    host/collection/datadir.cpp
    host/collection/game_data.cpp
    host/collection/icons.cpp
    host/collection/json.cpp
    host/collection/save_backup.cpp
    host/collection/search.cpp
    host/collection/upgrades.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/embedded_catalogue.cpp)
# Each game's pictures from the player's disc: logo, background, portraits (docs/runtime/host/game_art.md). Where each
# game keeps them is its manifest, games/<id>/art.json, every one built in (a new game adds a file, no code).
file(GLOB SSA_ART_MANIFESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/games/*/art.json)
list(SORT SSA_ART_MANIFESTS)
string(REPLACE ";" "|" SSA_ART_MANIFEST_LIST "${SSA_ART_MANIFESTS}")
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/embedded_art_manifests.cpp
    COMMAND ${CMAKE_COMMAND} "-DINPUTS=${SSA_ART_MANIFEST_LIST}" -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/art_manifests.json
        -P ${CMAKE_SOURCE_DIR}/cmake/join_json.cmake
    COMMAND ${CMAKE_COMMAND} -DNAME=g_embeddedArtManifests -DINPUT=${CMAKE_CURRENT_BINARY_DIR}/art_manifests.json
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/embedded_art_manifests.cpp -P ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/cmake/join_json.cmake ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake ${SSA_ART_MANIFESTS}
    COMMENT "Embedding the art manifests" VERBATIM)
set(SSA_ART_SOURCES
    host/art/alchemy_archive.cpp
    host/art/disc_languages.cpp
    host/art/game_art.cpp
    host/art/game_text.cpp
    host/art/lux_images.cpp
    host/art/xenos_texture.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/embedded_art_manifests.cpp)

# The pictures' readers as a library, for SkyBuild (the game's icon is made from the disc's logo: tools/skybuild).
add_library(SSAArtReader STATIC ${SSA_ART_SOURCES} ${SSA_COLLECTION_SOURCES})
target_include_directories(SSAArtReader PUBLIC .)
target_link_libraries(SSAArtReader PUBLIC SDL3::SDL3-static)

# The built game's name: the executable, the macOS app bundle and the Windows .exe (games/<id>/info.cpp names it for the
# launcher). The CMake target stays SSARecomp for every game, so every build and test command keeps working.
if(SSA_GAME STREQUAL "giants")
    set(SSA_RUNTIME_NAME SGRecomp)
elseif(SSA_GAME STREQUAL "swapforce")
    set(SSA_RUNTIME_NAME SSFRecomp)
else()
    set(SSA_RUNTIME_NAME SSARecomp)
endif()

add_executable(SSARecomp
    apu/xma_context.cpp
    d3d/bink_display.cpp
    d3d/capture.cpp
    d3d/command_buffers.cpp
    d3d/device.cpp
    d3d/render.cpp
    d3d/ring.cpp
    d3d/ultrawide.cpp
    d3d/shaders.cpp
    d3d/xenos.cpp
    d3d/null_gpu.cpp
    game.cpp
    main.cpp
    memory.cpp
    loader.cpp
    platform/timebase.cpp
    ${SSA_PLATFORM_SOURCES}
    thread.cpp
    indirect_call_guard.cpp
    vfs.cpp
    kernel/audio.cpp
    host/audio.cpp
    host/display.cpp
    host/dds.cpp
    host/glyphs.cpp
    host/texture_pack.cpp
    ${SSA_COLLECTION_SOURCES}
    ${SSA_ART_SOURCES}
    host/input.cpp
    host/launcher.cpp
    host/launcher_games.cpp
    host/xex_title.cpp
    host/log.cpp
    host/portal.cpp
    host/portal_async.cpp
    host/portal_emulated.cpp
    host/portal_identity.cpp
    host/portal_prompt.cpp
    host/portal_hid.cpp
    host/portal_usb.cpp
    host/settings.cpp
    host/toybox.cpp
    host/toybox/draw.cpp
    host/toybox/model.cpp
    kernel/files.cpp
    kernel/imports_giants.cpp
    kernel/imports_swapforce.cpp
    kernel/input.cpp
    kernel/memory.cpp
    kernel/module.cpp
    kernel/net.cpp
    kernel/objects.cpp
    kernel/overlapped.cpp
    kernel/printf.cpp
    kernel/portal.cpp
    kernel/rtl_time.cpp
    kernel/strings.cpp
    kernel/stubs.cpp
    kernel/sync.cpp
    kernel/system.cpp
    kernel/threads.cpp
    kernel/video.cpp
    kernel/xam.cpp
    kernel/xam_achievements.cpp
    kernel/xam_content.cpp
    kernel/xam_user.cpp
    kernel/xconfig.cpp
    kernel/xma.cpp
    kernel/xmp.cpp)
set_target_properties(SSARecomp PROPERTIES OUTPUT_NAME ${SSA_RUNTIME_NAME})
target_compile_definitions(SSARecomp PRIVATE SSA_VERSION="${PROJECT_VERSION}")
# The commit the runtime was configured from, for the log's first lines (host/log.cpp).
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE SSA_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git diff --quiet HEAD -- runtime SSARecompLib WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE SSA_BUILD_DIRTY ERROR_QUIET)
if(SSA_BUILD AND SSA_BUILD_DIRTY)
    string(APPEND SSA_BUILD "-dirty")
endif()
set_source_files_properties(host/log.cpp PROPERTIES COMPILE_DEFINITIONS "SSA_BUILD=\"${SSA_BUILD}\"")

add_subdirectory(gpu)

# The game's zlib inflate runs as the host's (inflate.cpp): loads are mostly inflating. The system zlib when there is
# one (macOS, Linux), else miniz from the tree (Windows).
find_package(ZLIB QUIET)
set(SSA_MINIZ_DIR ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/smol-v/testing/external/miniz)
if(ZLIB_FOUND)
    target_sources(SSARecomp PRIVATE inflate.cpp)
    target_link_libraries(SSARecomp PRIVATE ZLIB::ZLIB)
elseif(EXISTS ${SSA_MINIZ_DIR}/miniz.c)
    target_sources(SSARecomp PRIVATE inflate.cpp ${SSA_MINIZ_DIR}/miniz.c)
    target_include_directories(SSARecomp PRIVATE ${SSA_MINIZ_DIR})
    set_source_files_properties(inflate.cpp PROPERTIES COMPILE_DEFINITIONS SSA_INFLATE_MINIZ)
    set_source_files_properties(${SSA_MINIZ_DIR}/miniz.c PROPERTIES LANGUAGE C
        COMPILE_DEFINITIONS "MINIZ_NO_STDIO;MINIZ_NO_TIME;MINIZ_NO_ARCHIVE_APIS;MINIZ_NO_ZLIB_COMPATIBLE_NAMES")
else()
    message(STATUS "No zlib and no miniz: levels are inflated by the game's own translated zlib (slower loads)")
endif()

# XMA audio (Wwise music and sound effects) is decoded with FFmpeg's libavcodec: the minimal static LGPL build of
# tools/ffmpeg/build.sh (built into the build tree by default, or SSA_FFMPEG_ROOT), else a system one through
# pkg-config. GPL/non-free FFmpeg builds are rejected at configure time.
include(${CMAKE_SOURCE_DIR}/cmake/ffmpeg.cmake)
ssa_setup_ffmpeg(SSARecomp)

# The engine's hooks (engines/<engine>/) and the game's adapter (games/<id>/) are compiled into the runtime: the hooks
# override the game's translated functions, the adapter answers game.h.
file(GLOB SSA_ENGINE_SOURCES CONFIGURE_DEPENDS ${SSA_ENGINE_DIR}/*.cpp)
file(GLOB SSA_GAME_SOURCES CONFIGURE_DEPENDS ${SSA_GAME_DIR}/*.cpp)
target_sources(SSARecomp PRIVATE ${SSA_ENGINE_SOURCES} ${SSA_GAME_SOURCES})
target_include_directories(SSARecomp PRIVATE . ${SSA_GAME_DIR} ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/xxHash
    ${CMAKE_SOURCE_DIR}/thirdparty/stb)
# Button glyphs are drawn with the embedded Bungee (embedded_fonts.cpp, below); stb_truetype comes from
# SSALauncherUi's stb_impl.cpp, which the UWP build does not link.
if(SSA_UWP)
    target_sources(SSARecomp PRIVATE host/stb_impl.cpp)
    # Xbox present test (tools/xbox/presenttest): a small separate package, built on request.
    add_executable(SSAPresentTest WIN32 EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/tools/xbox/presenttest/main.cpp host/uwp/uwp_compat.cpp)
    target_include_directories(SSAPresentTest PRIVATE host/uwp)
    target_link_libraries(SSAPresentTest PRIVATE plume)
    target_link_options(SSAPresentTest PRIVATE -municode)
endif()
target_link_libraries(SSARecomp PRIVATE SSARecompLib SSARecompGpu SSAXenonUtils fmt::fmt SDL3::SDL3-static)

# The launcher window: SDL_Renderer drawing, a small TrueType atlas (stb_truetype, MIT/public domain) and two
# OFL fonts. It links into both the game and the launcher test (the test links it even though it never opens a window,
# because launcher.cpp calls into it).
if(NOT SSA_UWP)
    add_library(SSALauncherUi STATIC host/launcher_ui.cpp host/launcher_ui_draw.cpp host/launcher_assets.cpp host/stb_impl.cpp host/widgets/launcher_widgets.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/smol-v/testing/external/miniz/miniz.c)
    target_include_directories(SSALauncherUi PRIVATE . ${CMAKE_SOURCE_DIR}/thirdparty/stb
        ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/smol-v/testing/external/miniz)
    target_link_libraries(SSALauncherUi PUBLIC SDL3::SDL3-static fmt::fmt SSAXenonUtils)
    # A development convenience (fonts are looked for in the source tree); not in the player kit, which holds no path.
    if(NOT SSA_KIT_BUILD)
        target_compile_definitions(SSALauncherUi PRIVATE SSA_SOURCE_FONT_DIR="${CMAKE_SOURCE_DIR}/resources/launcher/fonts")
    endif()
    target_link_libraries(SSARecomp PRIVATE SSALauncherUi)

endif()

# The Toy Box overlay: Dear ImGui (MIT) rasterized on the CPU by host/imgui_software.cpp (every platform, the Xbox
# too), with Outfit and Bungee built into the executable.
set(IMGUI_ROOT ${CMAKE_SOURCE_DIR}/thirdparty/imgui)
add_library(imgui STATIC ${IMGUI_ROOT}/imgui.cpp ${IMGUI_ROOT}/imgui_draw.cpp ${IMGUI_ROOT}/imgui_tables.cpp
    ${IMGUI_ROOT}/imgui_widgets.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_ROOT})
# Outfit's variable font defaults to its Thin weight, which stb_truetype renders as is: the Toy Box uses static
# Bold and Black instances (fontTools varLib.instancer wght=700/900).
set(SSA_OUTFIT_BOLD ${CMAKE_SOURCE_DIR}/resources/toybox/fonts/Outfit-Bold.ttf)
set(SSA_OUTFIT_BLACK ${CMAKE_SOURCE_DIR}/resources/toybox/fonts/Outfit-Black.ttf)
set(SSA_BUNGEE ${CMAKE_SOURCE_DIR}/resources/launcher/fonts/Bungee-Regular.ttf)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/embedded_fonts.cpp
    COMMAND ${CMAKE_COMMAND} -DOUTFIT_BOLD=${SSA_OUTFIT_BOLD} -DOUTFIT_BLACK=${SSA_OUTFIT_BLACK} -DBUNGEE=${SSA_BUNGEE}
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/embedded_fonts.cpp -P ${CMAKE_SOURCE_DIR}/cmake/embed_fonts.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/cmake/embed_fonts.cmake ${SSA_OUTFIT_BOLD} ${SSA_OUTFIT_BLACK} ${SSA_BUNGEE}
    COMMENT "Embedding the Toy Box fonts")
target_sources(SSARecomp PRIVATE host/imgui_software.cpp ${CMAKE_CURRENT_BINARY_DIR}/embedded_fonts.cpp)
target_link_libraries(SSARecomp PRIVATE imgui)

# Launcher checks without UI: SSALauncherTest <game disc .iso> <scratch dir> [<damaged copy>...]
add_executable(SSALauncherTest EXCLUDE_FROM_ALL host/launcher_test.cpp host/launcher.cpp host/launcher_games.cpp host/xex_title.cpp ${SSA_GAME_DIR}/info.cpp vfs.cpp
    host/collection/datadir.cpp host/collection/game_data.cpp ${SSA_PLATFORM_SOURCES})
target_include_directories(SSALauncherTest PRIVATE .)
target_link_libraries(SSALauncherTest PRIVATE SSAXenonUtils fmt::fmt SDL3::SDL3-static)
if(NOT SSA_UWP)
    target_link_libraries(SSALauncherTest PRIVATE SSALauncherUi)
endif()

# DDS reader and writer, content hash and texture pack index (no game data needed): ./SSATexturePackTest
add_executable(SSATexturePackTest EXCLUDE_FROM_ALL host/texture_pack_test.cpp host/dds.cpp host/texture_pack.cpp host/collection/datadir.cpp)
target_include_directories(SSATexturePackTest PRIVATE . ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/xxHash)

# Settings parser test (launcher.ini, environment, command line; no game data needed): ./SSASettingsTest
add_executable(SSASettingsTest host/settings_test.cpp host/settings.cpp)
target_include_directories(SSASettingsTest PRIVATE .)
target_link_libraries(SSASettingsTest PRIVATE fmt::fmt SDL3::SDL3-static)

# Fonts are data, not game content: Outfit and Bungee (SIL Open Font License, licence files next to them). On macOS
# they live in the app bundle's Resources/fonts (SDL_GetBasePath returns that directory); elsewhere they are copied
# next to the executable.
set(SSA_FONT_FILES
    ${CMAKE_SOURCE_DIR}/resources/launcher/fonts/Outfit-Variable.ttf
    ${CMAKE_SOURCE_DIR}/resources/launcher/fonts/Bungee-Regular.ttf
    ${CMAKE_SOURCE_DIR}/resources/launcher/fonts/OFL-Outfit.txt
    ${CMAKE_SOURCE_DIR}/resources/launcher/fonts/OFL-Bungee.txt)
if(NOT SSA_UWP)
    if(APPLE)
        set_source_files_properties(${SSA_FONT_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/fonts)
        target_sources(SSARecomp PRIVATE ${SSA_FONT_FILES})
    else()
        add_custom_command(TARGET SSARecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources/launcher/fonts $<TARGET_FILE_DIR:SSARecomp>/fonts)
        add_custom_command(TARGET SSALauncherTest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources/launcher/fonts $<TARGET_FILE_DIR:SSALauncherTest>/fonts)
    endif()
endif()

if(APPLE)
    # Double-clickable SSARecomp.app, ad-hoc signed. build/SSARecomp/SSARecomp stays a link to its executable
    # so command-line runs keep working.
    set(SSA_MACOS_MINIMUM ${CMAKE_OSX_DEPLOYMENT_TARGET})
    if(NOT SSA_MACOS_MINIMUM)
        # Without a deployment target the compiler targets the build machine's macOS major release.
        execute_process(COMMAND sw_vers -productVersion OUTPUT_VARIABLE SSA_MACOS_MINIMUM OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX REPLACE "^([0-9]+).*" "\\1.0" SSA_MACOS_MINIMUM "${SSA_MACOS_MINIMUM}")
    endif()

    set(SSA_ICON ${CMAKE_SOURCE_DIR}/resources/macos/SSARecomp.icns)
    target_sources(SSARecomp PRIVATE ${SSA_ICON})
    set_source_files_properties(${SSA_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # Ship the third-party notices in the bundle; listing it as a bundle resource keeps them in sync with the target.
    target_sources(SSARecomp PRIVATE ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(SSARecomp PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/resources/macos/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME ${SSA_RUNTIME_NAME}
        MACOSX_BUNDLE_GUI_IDENTIFIER org.ssarecomp.${SSA_RUNTIME_NAME}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION 1
        MACOSX_BUNDLE_ICON_FILE SSARecomp.icns)
    target_link_libraries(SSARecomp PRIVATE "-framework CoreGraphics")
    add_custom_command(TARGET SSARecomp POST_BUILD
        COMMAND codesign --force --sign - --timestamp=none $<TARGET_BUNDLE_DIR:SSARecomp>
        COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_CURRENT_BINARY_DIR}/${SSA_RUNTIME_NAME}
        COMMAND ${CMAKE_COMMAND} -E create_symlink ${SSA_RUNTIME_NAME}.app/Contents/MacOS/${SSA_RUNTIME_NAME} ${CMAKE_CURRENT_BINARY_DIR}/${SSA_RUNTIME_NAME}
        VERBATIM)
endif()

# Protocol test of the emulated Skylanders portal (no game data needed): ./SSAPortalTest
add_executable(SSAPortalTest host/portal_emulated.cpp host/portal_identity.cpp host/portal_prompt.cpp ${CMAKE_SOURCE_DIR}/games/ssa/ssa_portal_prompt.cpp ${CMAKE_SOURCE_DIR}/games/giants/giants_portal_prompt.cpp ${CMAKE_SOURCE_DIR}/games/swapforce/swapforce_portal_prompt.cpp host/portal_test.cpp host/figures/figure_data.cpp)
target_include_directories(SSAPortalTest PRIVATE . ${CMAKE_SOURCE_DIR}/games/ssa ${CMAKE_SOURCE_DIR}/games/giants ${CMAKE_SOURCE_DIR}/games/swapforce)
target_link_libraries(SSAPortalTest PRIVATE SDL3::SDL3-static)

# Real portal I/O off the title's threads, with a fake device that stalls (no game data needed): ./SSAPortalAsyncTest
add_executable(SSAPortalAsyncTest host/portal_async.cpp host/portal_async_test.cpp)
target_include_directories(SSAPortalAsyncTest PRIVATE .)
target_link_libraries(SSAPortalAsyncTest PRIVATE SDL3::SDL3-static)

# Figure library test (no game data needed): ./SSAFigureTest [--iso <disc image>] [<figure dump>...]
add_executable(SSAFigureTest host/figures/figure_test.cpp ${SSA_COLLECTION_SOURCES}
    host/portal_emulated.cpp host/portal_identity.cpp d3d/xenos.cpp vfs.cpp ${SSA_PLATFORM_SOURCES})
target_include_directories(SSAFigureTest PRIVATE .)
target_link_libraries(SSAFigureTest PRIVATE plume SSAXenonUtils fmt::fmt SDL3::SDL3-static)

# Disc pictures: the texture, archive and image readers on synthetic data (no game data needed); with SSA_ISO and/or
# GIANTS_ISO set, each game's pictures read from its disc into a scratch SKYLANDERS_DATA: ./SSAArtTest
add_executable(SSAArtTest host/art/art_test.cpp ${SSA_ART_SOURCES} ${SSA_COLLECTION_SOURCES} vfs.cpp ${SSA_PLATFORM_SOURCES})
target_include_directories(SSAArtTest PRIVATE .)
target_link_libraries(SSAArtTest PRIVATE SDL3::SDL3-static)

# Collection library test (no game data, never the player's folders: a scratch collection): ./SSACollectionTest
add_executable(SSACollectionTest host/collection/collection_test.cpp ${SSA_COLLECTION_SOURCES})
target_include_directories(SSACollectionTest PRIVATE .)
target_link_libraries(SSACollectionTest PRIVATE SDL3::SDL3-static)

# Data folder helper and its migration (no game data, scratch folders only): ./SSADatadirTest
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_executable(SSADatadirTest host/collection/datadir_test.cpp host/collection/datadir.cpp)
    target_include_directories(SSADatadirTest PRIVATE .)
endif()

# "Copy the game data" and the folder reader (synthetic files, scratch folders only): ./SSAGameDataTest <scratch dir>
add_executable(SSAGameDataTest host/collection/game_data_test.cpp host/collection/game_data.cpp host/collection/datadir.cpp vfs.cpp ${SSA_PLATFORM_SOURCES})
target_include_directories(SSAGameDataTest PRIVATE .)
target_link_libraries(SSAGameDataTest PRIVATE SDL3::SDL3-static)

# Save backups, atomic writes, restoring and the running game's marker (no game data, scratch folders only): ./SSASavesTest
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_executable(SSASavesTest host/collection/save_backup_test.cpp host/collection/save_backup.cpp host/collection/datadir.cpp)
    target_include_directories(SSASavesTest PRIVATE .)
endif()

# On Windows/packaged builds, copy third-party notices into the output directory.
if(WIN32)
    target_sources(SSARecomp PRIVATE platform/win32/SSARecomp.rc)
    target_compile_definitions(SSARecomp PRIVATE _WIN32_WINNT=0x0A00) # Windows 10
    if(NOT SSA_UWP)
        target_link_libraries(SSARecomp PRIVATE winmm)
    endif()
    add_custom_command(TARGET SSARecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
            $<TARGET_FILE_DIR:SSARecomp>/THIRD_PARTY_NOTICES.md
        VERBATIM)
endif()

if(SSA_UWP)
    # Xbox Dev Mode (UWP app, packaged by tools/xbox/package.sh): host/uwp replaces main.cpp and the SDL host
    # (window and events, audio, launcher, real portals).
    get_target_property(SSA_SOURCES SSARecomp SOURCES)
    list(REMOVE_ITEM SSA_SOURCES main.cpp host/audio.cpp host/launcher.cpp host/launcher_games.cpp host/xex_title.cpp host/log.cpp host/portal_hid.cpp
        host/portal_usb.cpp)
    set_property(TARGET SSARecomp PROPERTY SOURCES ${SSA_SOURCES} host/audio_xaudio2.cpp host/uwp/main.cpp
        host/uwp/portal_backends.cpp host/uwp/settings_uwp.cpp host/uwp/uwp_compat.cpp host/uwp/cpu_check.cpp)
    # The CPU check runs before any other code, so it stays on the baseline whatever SSA_UWP_MARCH (toolchain file).
    set_source_files_properties(host/uwp/cpu_check.cpp PROPERTIES COMPILE_OPTIONS "-march=btver2")
    if(NOT SSA_UWP_MARCH STREQUAL "btver2")
        target_compile_definitions(SSARecomp PRIVATE SSA_REQUIRE_X86_64_V3)
    endif()
    set_target_properties(SSARecomp PROPERTIES WIN32_EXECUTABLE ON)
    target_link_options(SSARecomp PRIVATE -municode)
    # Shared files that include SDL's headers (see SSA_UWP_DESKTOP_FAMILY in the root CMakeLists.txt).
    set_source_files_properties(host/display.cpp host/portal.cpp host/portal_emulated.cpp kernel/xam_content.cpp
        host/settings.cpp host/uwp/settings_uwp.cpp host/toybox.cpp host/toybox/draw.cpp host/toybox/model.cpp
        host/art/game_art.cpp host/art/game_text.cpp
        host/collection/catalogue.cpp host/collection/collection.cpp host/collection/icons.cpp
        PROPERTIES COMPILE_OPTIONS "${SSA_UWP_DESKTOP_FAMILY}")
    set_source_files_properties(gpu/gpu.cpp DIRECTORY gpu PROPERTIES COMPILE_OPTIONS "${SSA_UWP_DESKTOP_FAMILY}")
    set_target_properties(SSAPortalTest SSAFigureTest SSAArtTest SSACollectionTest SSASettingsTest SSARecompGpuTest PROPERTIES EXCLUDE_FROM_ALL ON)
endif()

# The game's version in its window title (games/<id>/game.txt, read by the top CMakeLists.txt): main.cpp only, so a new
# version leaves the other files' compile commands (and the kit's compile.args) unchanged.
set_source_files_properties(main.cpp PROPERTIES COMPILE_DEFINITIONS SSA_GAME_VERSION="${SSA_GAME_VERSION}")
