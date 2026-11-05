# Host OS services (memory, threads, faults, files): POSIX or Win32.
if(WIN32)
    set(SSA_PLATFORM_SOURCES platform/platform_win32.cpp)
    # Windows 10 (CreateFile2) in every target that links it, before any header (fmt forces <cstdlib> first on mingw).
    set_source_files_properties(platform/platform_win32.cpp PROPERTIES COMPILE_DEFINITIONS _WIN32_WINNT=0x0A00)
else()
    set(SSA_PLATFORM_SOURCES platform/platform_posix.cpp)
endif()

include(${CMAKE_SOURCE_DIR}/cmake/art_reader.cmake)

# Keep target names and bundle identifiers stable; player-visible names come from game.txt.
file(STRINGS "${SSA_GAME_DIR}/game.txt" SSA_RUNTIME_NAME REGEX "^runtime=")
string(REGEX REPLACE "^runtime=" "" SSA_RUNTIME_NAME "${SSA_RUNTIME_NAME}")
if(SSA_GAME STREQUAL "giants")
    set(SSA_BUNDLE_ID SGRecomp)
elseif(SSA_GAME STREQUAL "swapforce")
    set(SSA_BUNDLE_ID SSFRecomp)
elseif(SSA_GAME STREQUAL "trapteam")
    set(SSA_BUNDLE_ID TTRecomp)
elseif(SSA_GAME STREQUAL "superchargers")
    set(SSA_BUNDLE_ID SSCRecomp)
elseif(SSA_GAME STREQUAL "imaginators")
    set(SSA_BUNDLE_ID SIMRecomp)
else()
    set(SSA_BUNDLE_ID SSARecomp)
endif()
# Optional disc-derived icon family, generated locally by Arkeyan-Builder --icon-only. Never checked in.
set(SSA_GAME_ICON_DIR "${CMAKE_SOURCE_DIR}/private/${SSA_GAME}/icon" CACHE PATH "Local game_icon.png/.ico/.icns folder")

add_executable(ArkeyanRuntime
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
    write_watch.cpp
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
    host/glyph_models.cpp
    host/texture_pack.cpp
    ${SSA_COLLECTION_SOURCES}
    ${SSA_ART_SOURCES}
    host/input.cpp
    host/pad_map.cpp
    host/mouse.cpp
    host/mouse_input.cpp
    host/launcher.cpp
    host/launcher_games.cpp
    host/portable.cpp
    host/xex_title.cpp
    host/collection/achievements.cpp
    host/log.cpp
    host/gamemenu.cpp
    host/gamemenu_debug.cpp
    host/commands.cpp
    host/report.cpp
    host/portal.cpp
    host/portal_adpcm.cpp
    host/portal_async.cpp
    host/portal_emulated.cpp
    host/portal_identity.cpp
    host/portal_prompt.cpp
    host/portal_speaker.cpp
    host/portal_hid.cpp
    host/portal_usb.cpp
    host/settings.cpp
    host/toybox.cpp
    host/toybox/draw.cpp
    host/toybox/menu.cpp
    host/toybox/model.cpp
    host/toybox/trap_villain.cpp
    kernel/files.cpp
    kernel/imports_giants.cpp
    kernel/imports_swapforce.cpp
    kernel/imports_trapteam.cpp
    kernel/input.cpp
    kernel/memory.cpp
    kernel/module.cpp
    kernel/net.cpp
    kernel/objects.cpp
    kernel/overlapped.cpp
    kernel/guest_format.cpp
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
set_target_properties(ArkeyanRuntime PROPERTIES OUTPUT_NAME "${SSA_RUNTIME_NAME}")
if(NOT SSA_KIT_BUILD)
    if(WIN32 AND NOT SSA_UWP)
        string(REPLACE "." "," SSA_GAME_VERSION_PARTS "${SSA_GAME_VERSION}.0")
        set(SSA_ICON_RESOURCE "")
        if(EXISTS "${SSA_GAME_ICON_DIR}/game_icon.ico")
            set(SSA_ICON_RESOURCE "1 ICON \"${SSA_GAME_ICON_DIR}/game_icon.ico\"")
        endif()
        configure_file(${CMAKE_SOURCE_DIR}/tools/arkeyan-builder/game.rc.in game.rc @ONLY)
        target_sources(ArkeyanRuntime PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/game.rc)
    endif()
    if(EXISTS "${SSA_GAME_ICON_DIR}/game_icon.png")
        if(APPLE)
            target_sources(ArkeyanRuntime PRIVATE "${SSA_GAME_ICON_DIR}/game_icon.png")
            set_source_files_properties("${SSA_GAME_ICON_DIR}/game_icon.png" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        else()
            add_custom_command(TARGET ArkeyanRuntime POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SSA_GAME_ICON_DIR}/game_icon.png" "$<TARGET_FILE_DIR:ArkeyanRuntime>/game_icon.png"
                VERBATIM)
        endif()
    else()
        message(STATUS "No local disc icon family: generate it with Arkeyan-Builder --icon-only; platform defaults apply")
    endif()
endif()
target_compile_definitions(ArkeyanRuntime PRIVATE SSA_VERSION="${PROJECT_VERSION}")
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
# miniz (public domain, from the tree; the launcher's PNG reader, and inflate.cpp's without zlib): a library of its own,
# so our warning flags stay off it, and its directory is SYSTEM for the targets that include it.
add_library(SSAMiniz STATIC ${SSA_MINIZ_DIR}/miniz.c)
target_compile_definitions(SSAMiniz PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ARCHIVE_APIS MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_include_directories(SSAMiniz SYSTEM INTERFACE ${SSA_MINIZ_DIR})
if(ZLIB_FOUND)
    target_sources(ArkeyanRuntime PRIVATE inflate.cpp)
    target_link_libraries(ArkeyanRuntime PRIVATE ZLIB::ZLIB)
elseif(EXISTS ${SSA_MINIZ_DIR}/miniz.c)
    target_sources(ArkeyanRuntime PRIVATE inflate.cpp)
    target_link_libraries(ArkeyanRuntime PRIVATE SSAMiniz)
    set_source_files_properties(inflate.cpp PROPERTIES COMPILE_DEFINITIONS SSA_INFLATE_MINIZ)
else()
    message(STATUS "No zlib and no miniz: levels are inflated by the game's own translated zlib (slower loads)")
endif()

# XMA audio (Wwise music and sound effects) is decoded with FFmpeg's libavcodec: the minimal static LGPL build of
# tools/ffmpeg/build.sh (built into the build tree by default, or SSA_FFMPEG_ROOT), else a system one through
# pkg-config. GPL/non-free FFmpeg builds are rejected at configure time.
include(${CMAKE_SOURCE_DIR}/cmake/ffmpeg.cmake)
ssa_setup_ffmpeg(ArkeyanRuntime)

# The engine's hooks (engines/<engine>/) and the game's adapter (games/<id>/) are compiled into the runtime: the hooks
# override the game's translated functions, the adapter answers game.h.
file(GLOB SSA_ENGINE_SOURCES CONFIGURE_DEPENDS ${SSA_ENGINE_DIR}/*.cpp)
file(GLOB SSA_GAME_SOURCES CONFIGURE_DEPENDS ${SSA_GAME_DIR}/*.cpp)
target_sources(ArkeyanRuntime PRIVATE ${SSA_ENGINE_SOURCES} ${SSA_GAME_SOURCES})
target_include_directories(ArkeyanRuntime PRIVATE . ${SSA_GAME_DIR} ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/xxHash
    ${CMAKE_SOURCE_DIR}/thirdparty/stb)
# Button glyphs are drawn with the embedded Bungee (embedded_fonts.cpp, below); stb_truetype comes from
# SSALauncherUi's stb_impl.cpp, which the UWP build does not link.
if(SSA_UWP)
    target_sources(ArkeyanRuntime PRIVATE host/stb_impl.cpp)
    # Xbox present test (tools/xbox/presenttest): a small separate package, built on request.
    add_executable(SSAPresentTest WIN32 EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/tools/xbox/presenttest/main.cpp host/uwp/uwp_compat.cpp)
    target_include_directories(SSAPresentTest PRIVATE host/uwp)
    target_link_libraries(SSAPresentTest PRIVATE plume)
    target_link_options(SSAPresentTest PRIVATE -municode)
endif()
target_link_libraries(ArkeyanRuntime PRIVATE SSARecompLib SSARecompGpu SSAXenonUtils fmt::fmt SDL3::SDL3-static)
# ppc_context.h and ppc_config.h are XenonRecomp's output: system headers here, so their warnings stay out of ours. System
# directories come after every -I one, so XenonUtils (which has its own ppc_context.h) is one too, after the game's.
target_include_directories(ArkeyanRuntime SYSTEM PRIVATE ${SSA_PPC_DIR} ${SSA_XENON_UTILS})
# Linux's C library has its own byteswap.h, found before a SYSTEM directory.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_options(ArkeyanRuntime PRIVATE "SHELL:-iquote ${SSA_XENON_UTILS}")
endif()

# The launcher window: SDL_Renderer drawing, a small TrueType atlas (stb_truetype, MIT/public domain) and two
# OFL fonts. It links into both the game and the launcher test (the test links it even though it never opens a window,
# because launcher.cpp calls into it).
if(NOT SSA_UWP)
    add_library(SSALauncherUi STATIC host/launcher_ui.cpp host/launcher_ui_draw.cpp host/launcher_assets.cpp host/stb_impl.cpp host/widgets/launcher_widgets.cpp)
    target_include_directories(SSALauncherUi PRIVATE .)
    target_include_directories(SSALauncherUi SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/stb) # stb's warnings are not ours
    target_link_libraries(SSALauncherUi PUBLIC SDL3::SDL3-static fmt::fmt SSAXenonUtils SSAMiniz)
    # A development convenience (fonts are looked for in the source tree); not in the player kit, which holds no path.
    if(NOT SSA_KIT_BUILD)
        target_compile_definitions(SSALauncherUi PRIVATE SSA_SOURCE_FONT_DIR="${CMAKE_SOURCE_DIR}/resources/launcher/fonts")
    endif()
    target_link_libraries(ArkeyanRuntime PRIVATE SSALauncherUi)

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
target_sources(ArkeyanRuntime PRIVATE host/imgui_software.cpp ${CMAKE_CURRENT_BINARY_DIR}/embedded_fonts.cpp)
target_link_libraries(ArkeyanRuntime PRIVATE imgui)

# CPU-only diagnostic overlay check; synthetic measurements, no GPU device or game is started.
add_executable(SSADebugScreenTest host/gamemenu_debug_test.cpp host/gamemenu_debug.cpp host/imgui_software.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/embedded_fonts.cpp)
target_include_directories(SSADebugScreenTest PRIVATE . ${CMAKE_SOURCE_DIR}/thirdparty/plume)
target_compile_definitions(SSADebugScreenTest PRIVATE SSA_VERSION="${PROJECT_VERSION}")
target_link_libraries(SSADebugScreenTest PRIVATE imgui SDL3::SDL3-static)
if(WIN32)
    target_compile_definitions(SSADebugScreenTest PRIVATE _WIN32_WINNT=0x0A00)
endif()

# Launcher checks without UI: SSALauncherTest <game disc .iso> <scratch dir> [<damaged copy>...]
add_executable(SSALauncherTest EXCLUDE_FROM_ALL host/launcher_test.cpp host/launcher.cpp host/launcher_games.cpp host/portable.cpp host/xex_title.cpp ${SSA_GAME_DIR}/info.cpp vfs.cpp
    host/collection/datadir.cpp host/collection/game_data.cpp host/collection/portable_pack.cpp host/collection/json.cpp ${SSA_PLATFORM_SOURCES})
target_include_directories(SSALauncherTest PRIVATE .)
target_link_libraries(SSALauncherTest PRIVATE SSAXenonUtils fmt::fmt SDL3::SDL3-static)
if(NOT SSA_UWP)
    target_link_libraries(SSALauncherTest PRIVATE SSALauncherUi)
endif()

# The F3 menu's Commands page on a scratch launcher.ini: keys, pad buttons, swaps, reset (no game data): ./SSACommandsTest
# The launcher.ini reader and writer come with SSALauncherTest's sources (its test file aside).
get_target_property(SSA_COMMANDS_TEST_SOURCES SSALauncherTest SOURCES)
list(REMOVE_ITEM SSA_COMMANDS_TEST_SOURCES host/launcher_test.cpp)
add_executable(SSACommandsTest host/commands_test.cpp host/commands.cpp host/settings.cpp host/pad_map.cpp host/mouse.cpp
    ${SSA_COMMANDS_TEST_SOURCES})
target_include_directories(SSACommandsTest PRIVATE .)
target_link_libraries(SSACommandsTest PRIVATE SSAXenonUtils fmt::fmt SDL3::SDL3-static)
if(NOT SSA_UWP)
    target_link_libraries(SSACommandsTest PRIVATE SSALauncherUi)
endif()

# DDS reader and writer, content hash and texture pack index (no game data needed): ./SSATexturePackTest
add_executable(SSATexturePackTest EXCLUDE_FROM_ALL host/texture_pack_test.cpp host/dds.cpp host/texture_pack.cpp host/collection/datadir.cpp)
target_include_directories(SSATexturePackTest PRIVATE . ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/xxHash)

# The guest's printf (flags, width, precision, the Microsoft conversions; no game data needed): ./SSAGuestFormatTest
add_executable(SSAGuestFormatTest kernel/guest_format_test.cpp kernel/guest_format.cpp)
target_include_directories(SSAGuestFormatTest PRIVATE .)
target_link_libraries(SSAGuestFormatTest PRIVATE SSAXenonUtils)

# The in-game interface's toast motion and F3 menu layout (host/toybox/look.h): ./SSAToyboxLookTest
add_executable(SSAToyboxLookTest host/toybox/look_test.cpp)
target_include_directories(SSAToyboxLookTest PRIVATE .)

# A trap's Villain row (host/toybox/trap_villain.h): the choices of each element, the write, the evolved switch, in a
# scratch collection (no game data): ./SSAToyboxTrapTest
add_executable(SSAToyboxTrapTest host/toybox/trap_villain_test.cpp host/toybox/trap_villain.cpp ${SSA_COLLECTION_SOURCES})
target_include_directories(SSAToyboxTrapTest PRIVATE .)
target_link_libraries(SSAToyboxTrapTest PRIVATE SDL3::SDL3-static)

# The remembered "resolution scale auto" step-down's file line: ./SSAAutoScaleTest
add_executable(SSAAutoScaleTest gpu/auto_scale_test.cpp)
target_include_directories(SSAAutoScaleTest PRIVATE .)

# The depth write-back's tile-by-tile offsets against xenos::Tiled2D: ./SSATiledDepthTest
add_executable(SSATiledDepthTest d3d/tiled_depth_test.cpp d3d/xenos.cpp)
target_include_directories(SSATiledDepthTest PRIVATE .)
target_link_libraries(SSATiledDepthTest PRIVATE plume SSAXenonUtils SDL3::SDL3-static)

# The write watch (faults on watched guest pages, the stamps, the cool-down): ./SSAWriteWatchTest
add_executable(SSAWriteWatchTest write_watch_test.cpp write_watch.cpp ${SSA_PLATFORM_SOURCES})
target_include_directories(SSAWriteWatchTest PRIVATE .)
if(WIN32)
    target_link_libraries(SSAWriteWatchTest PRIVATE winmm) # platform_win32.cpp's timer resolution
endif()

# The buffer cache's range hash (chunks shared by overlapping ranges): ./SSARangeHashTest
add_executable(SSARangeHashTest gpu/range_hash_test.cpp)
target_include_directories(SSARangeHashTest PRIVATE . ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/xxHash)

# 16-bit vertex component pairs back in order after the 32-bit swap: ./SSAVertexPairsTest
add_executable(SSAVertexPairsTest d3d/vertex_pairs_test.cpp)
target_include_directories(SSAVertexPairsTest PRIVATE .)

# Where host thunks go in the code's padding: ./SSAThunkSlotTest
add_executable(SSAThunkSlotTest thunk_slot_test.cpp)
target_include_directories(SSAThunkSlotTest PRIVATE .)

# The frame rate limiter's schedule and wait, with a fake clock: ./SSAFrameLimiterTest
add_executable(SSAFrameLimiterTest kernel/frame_limiter_test.cpp)
target_include_directories(SSAFrameLimiterTest PRIVATE .)

# The models that show a button over a glyph texture (host/glyphs.h, Model: the letters skipped, the texture matrix): ./SSAGlyphModelsTest
add_executable(SSAGlyphModelsTest host/glyph_models_test.cpp host/glyph_models.cpp)
target_include_directories(SSAGlyphModelsTest PRIVATE .)
target_include_directories(SSAGlyphModelsTest SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/xxHash)

# The mouse's names, bindings and capture (host/mouse.h; no SDL, no game data): ./SSAMouseTest
add_executable(SSAMouseTest host/mouse_test.cpp host/mouse.cpp)
target_include_directories(SSAMouseTest PRIVATE .)

# Settings parser test (launcher.ini, environment, command line; no game data needed): ./SSASettingsTest
add_executable(SSASettingsTest host/settings_test.cpp host/settings.cpp host/pad_map.cpp host/mouse.cpp)
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
        target_sources(ArkeyanRuntime PRIVATE ${SSA_FONT_FILES})
    else()
        add_custom_command(TARGET ArkeyanRuntime POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources/launcher/fonts $<TARGET_FILE_DIR:ArkeyanRuntime>/fonts)
        add_custom_command(TARGET SSALauncherTest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources/launcher/fonts $<TARGET_FILE_DIR:SSALauncherTest>/fonts)
    endif()
endif()

if(APPLE)
    # Double-clickable game bundle, ad-hoc signed. A sibling link points to its executable
    # so command-line runs keep working.
    set(SSA_MACOS_MINIMUM ${CMAKE_OSX_DEPLOYMENT_TARGET})
    if(NOT SSA_MACOS_MINIMUM)
        # Without a deployment target the compiler targets the build machine's macOS major release.
        execute_process(COMMAND sw_vers -productVersion OUTPUT_VARIABLE SSA_MACOS_MINIMUM OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX REPLACE "^([0-9]+).*" "\\1.0" SSA_MACOS_MINIMUM "${SSA_MACOS_MINIMUM}")
    endif()

    set(SSA_ICON ${CMAKE_SOURCE_DIR}/resources/macos/ArkeyanRuntime.icns)
    if(EXISTS "${SSA_GAME_ICON_DIR}/game_icon.icns" AND NOT SSA_KIT_BUILD)
        set(SSA_ICON "${SSA_GAME_ICON_DIR}/game_icon.icns")
    endif()
    target_sources(ArkeyanRuntime PRIVATE ${SSA_ICON})
    set_source_files_properties(${SSA_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # Ship the third-party notices in the bundle; listing it as a bundle resource keeps them in sync with the target.
    target_sources(ArkeyanRuntime PRIVATE ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    get_filename_component(SSA_ICON_FILE "${SSA_ICON}" NAME)
    set_target_properties(ArkeyanRuntime PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/resources/macos/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "${SSA_RUNTIME_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER org.ssarecomp.${SSA_BUNDLE_ID}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION 1
        MACOSX_BUNDLE_ICON_FILE "${SSA_ICON_FILE}")
    target_link_libraries(ArkeyanRuntime PRIVATE "-framework CoreGraphics")
    add_custom_command(TARGET ArkeyanRuntime POST_BUILD
        COMMAND codesign --force --sign - --timestamp=none $<TARGET_BUNDLE_DIR:ArkeyanRuntime>
        COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_CURRENT_BINARY_DIR}/${SSA_RUNTIME_NAME}"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${SSA_RUNTIME_NAME}.app/Contents/MacOS/${SSA_RUNTIME_NAME}" "${CMAKE_CURRENT_BINARY_DIR}/${SSA_RUNTIME_NAME}"
        VERBATIM)
endif()

# Remove only this game's old output after a successful rebuild; the target and identifier stay stable.
if(NOT SSA_RUNTIME_NAME STREQUAL SSA_BUNDLE_ID)
    if(APPLE)
        add_custom_command(TARGET ArkeyanRuntime POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/${SSA_BUNDLE_ID}.app"
            COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_CURRENT_BINARY_DIR}/${SSA_BUNDLE_ID}"
            VERBATIM)
    else()
        add_custom_command(TARGET ArkeyanRuntime POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_CURRENT_BINARY_DIR}/${SSA_BUNDLE_ID}${CMAKE_EXECUTABLE_SUFFIX}"
            VERBATIM)
    endif()
endif()

add_executable(SSARenderScaleTest d3d/render_scale_test.cpp)

# The wide scene and frame sizes for a window's shape: ./SSAUltrawideTest
add_executable(SSAUltrawideTest d3d/ultrawide_test.cpp d3d/ultrawide.cpp)
target_include_directories(SSAUltrawideTest PRIVATE . ${SSA_GAME_DIR})

# The pacer of per-frame simulation steps, with a fake clock: ./SSAStepPacerTest
add_executable(SSAStepPacerTest d3d/step_pacer_test.cpp)
target_include_directories(SSAStepPacerTest PRIVATE .)

# Protocol test of the emulated Skylanders portal (no game data needed): ./SSAPortalTest
add_executable(SSAPortalTest host/portal_emulated.cpp host/portal_identity.cpp host/portal_prompt.cpp ${CMAKE_SOURCE_DIR}/games/ssa/ssa_portal_prompt.cpp ${CMAKE_SOURCE_DIR}/games/giants/giants_portal_prompt.cpp ${CMAKE_SOURCE_DIR}/games/swapforce/swapforce_portal_prompt.cpp ${CMAKE_SOURCE_DIR}/games/superchargers/superchargers_portal_prompt.cpp host/portal_test.cpp host/figures/figure_data.cpp)
target_include_directories(SSAPortalTest PRIVATE . ${CMAKE_SOURCE_DIR}/games/ssa ${CMAKE_SOURCE_DIR}/games/giants ${CMAKE_SOURCE_DIR}/games/swapforce ${CMAKE_SOURCE_DIR}/games/superchargers)
target_link_libraries(SSAPortalTest PRIVATE SDL3::SDL3-static)

# The portal speaker's ADPCM decoder against a test-only encoder (no game data needed): ./SSAPortalAdpcmTest
add_executable(SSAPortalAdpcmTest host/portal_adpcm.cpp host/portal_adpcm_test.cpp)
target_include_directories(SSAPortalAdpcmTest PRIVATE .)

# The Traptanium speaker's route per portal model and the computer's stream, on the game's framing (no game data needed): ./SSAPortalSpeakerTest
add_executable(SSAPortalSpeakerTest host/portal_speaker.cpp host/portal_adpcm.cpp host/portal_identity.cpp host/figures/figure_data.cpp host/portal_speaker_test.cpp)
target_include_directories(SSAPortalSpeakerTest PRIVATE .)

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
if(NOT SSA_UWP)
    target_sources(SSAArtTest PRIVATE host/accents_test.cpp host/launcher.cpp host/launcher_games.cpp host/xex_title.cpp
        host/portal_emulated.cpp host/texture_pack.cpp host/dds.cpp host/log.cpp host/settings.cpp host/pad_map.cpp host/mouse.cpp host/portable.cpp
        ${CMAKE_SOURCE_DIR}/tools/arkeyan-builder/process.cpp ${SSA_GAME_DIR}/info.cpp)
    target_compile_definitions(SSAArtTest PRIVATE SSA_VERSION="test" SKY_ACCENTS_TEST)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Keep existing launcher/log warnings visible without hiding path-test failures.
        target_compile_options(SSAArtTest PRIVATE -Wno-error=unused-const-variable -Wno-error=missing-field-initializers)
    endif()
    target_include_directories(SSAArtTest PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/xxHash)
    target_include_directories(SSAArtTest SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/stb)
    target_link_libraries(SSAArtTest PRIVATE SSALauncherUi SSAXenonUtils)
    if(WIN32)
        target_sources(SSAArtTest PRIVATE platform/win32/SSARecomp.rc)
        target_link_libraries(SSAArtTest PRIVATE winmm)
    endif()
endif()

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
add_executable(SSAGameDataTest host/collection/game_data_test.cpp host/collection/game_data.cpp host/collection/datadir.cpp vfs.cpp
    host/collection/portable_pack.cpp host/collection/json.cpp ${SSA_PLATFORM_SOURCES})
target_include_directories(SSAGameDataTest PRIVATE .)
target_link_libraries(SSAGameDataTest PRIVATE SDL3::SDL3-static)

# The file list of the kit and of an installed game, and its checker (synthetic files, scratch folders only): ./SSAFileListTest <scratch dir>
add_executable(SSAFileListTest host/collection/file_list_test.cpp host/collection/file_list.cpp host/collection/datadir.cpp)
target_include_directories(SSAFileListTest PRIVATE .)

# The disc reader on damaged and crafted images (synthetic files, scratch folders only): ./SSAVfsTest <scratch dir>
add_executable(SSAVfsTest vfs_test.cpp vfs.cpp host/collection/portable_pack.cpp host/collection/json.cpp
    host/collection/datadir.cpp ${SSA_PLATFORM_SOURCES})
target_include_directories(SSAVfsTest PRIVATE .)
target_link_libraries(SSAVfsTest PRIVATE SDL3::SDL3-static)

# The portable game's pack and the game's reader of it (synthetic files, scratch folders only): ./SSAPortablePackTest <scratch dir>
add_executable(SSAPortablePackTest host/collection/portable_pack_test.cpp host/collection/portable_pack.cpp host/collection/json.cpp
    host/collection/datadir.cpp vfs.cpp ${SSA_PLATFORM_SOURCES})
target_include_directories(SSAPortablePackTest PRIVATE .)
target_link_libraries(SSAPortablePackTest PRIVATE SDL3::SDL3-static)

# Save backups, atomic writes, restoring and the running game's marker (no game data, scratch folders only): ./SSASavesTest
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_executable(SSASavesTest host/collection/save_backup_test.cpp host/collection/save_backup.cpp host/collection/datadir.cpp)
    target_include_directories(SSASavesTest PRIVATE .)
endif()

# Achievements (the XDBF reader), unlocks and play time (synthetic data, scratch folders; the discs given are only read):
# ./SSAProgressTest [<disc>.iso...]. And the achievement calls of a title on a fake guest context, against a disc:
# ./SSAAchievementsKernelTest <disc>.iso
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_executable(SSAProgressTest host/collection/progress_test.cpp host/collection/achievements.cpp host/collection/progress.cpp
        host/collection/save_backup.cpp host/collection/datadir.cpp host/collection/portable_pack.cpp host/collection/json.cpp vfs.cpp ${SSA_PLATFORM_SOURCES})
    target_include_directories(SSAProgressTest PRIVATE .)
    target_link_libraries(SSAProgressTest PRIVATE SSAXenonUtils fmt::fmt SDL3::SDL3-static)

    add_executable(SSAAchievementsKernelTest EXCLUDE_FROM_ALL kernel/xam_achievements_test.cpp kernel/xam_achievements.cpp kernel/objects.cpp
        kernel/overlapped.cpp memory.cpp host/collection/achievements.cpp host/collection/progress.cpp host/collection/save_backup.cpp
        host/collection/datadir.cpp host/collection/portable_pack.cpp host/collection/json.cpp vfs.cpp ${SSA_PLATFORM_SOURCES})
    target_include_directories(SSAAchievementsKernelTest PRIVATE .)
    target_include_directories(SSAAchievementsKernelTest SYSTEM PRIVATE ${SSA_PPC_DIR} ${SSA_XENON_UTILS}) # as ArkeyanRuntime's
    target_link_libraries(SSAAchievementsKernelTest PRIVATE SSAXenonUtils fmt::fmt SDL3::SDL3-static)

    # Host calls into the guest, thread memory over 2000 create/exit cycles, the dispatcher table; the game's code range,
    # no game code: ./SSAGuestThreadTest
    add_executable(SSAGuestThreadTest EXCLUDE_FROM_ALL thread_test.cpp thread.cpp indirect_call_guard.cpp kernel/objects.cpp
        memory.cpp ${SSA_PLATFORM_SOURCES})
    target_include_directories(SSAGuestThreadTest PRIVATE .)
    target_include_directories(SSAGuestThreadTest SYSTEM PRIVATE ${SSA_PPC_DIR} ${SSA_XENON_UTILS}) # as ArkeyanRuntime's
    target_link_libraries(SSAGuestThreadTest PRIVATE SSAXenonUtils)
endif()

# On Windows/packaged builds, copy third-party notices into the output directory.
if(WIN32)
    target_sources(ArkeyanRuntime PRIVATE platform/win32/SSARecomp.rc)
    target_compile_definitions(ArkeyanRuntime PRIVATE _WIN32_WINNT=0x0A00) # Windows 10
    if(NOT SSA_UWP)
        target_link_libraries(ArkeyanRuntime PRIVATE winmm)
    endif()
    add_custom_command(TARGET ArkeyanRuntime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
            $<TARGET_FILE_DIR:ArkeyanRuntime>/THIRD_PARTY_NOTICES.md
        VERBATIM)
endif()

if(SSA_UWP)
    # Xbox Dev Mode (UWP app, packaged by tools/xbox/package.sh): host/uwp replaces main.cpp and the SDL host
    # (window and events, audio, launcher, real portals).
    get_target_property(SSA_SOURCES ArkeyanRuntime SOURCES)
    list(REMOVE_ITEM SSA_SOURCES main.cpp host/mouse_input.cpp host/audio.cpp host/launcher.cpp host/launcher_games.cpp host/portable.cpp host/xex_title.cpp host/log.cpp host/portal_hid.cpp
        host/portal_usb.cpp host/gamemenu.cpp host/report.cpp)
    set_property(TARGET ArkeyanRuntime PROPERTY SOURCES ${SSA_SOURCES} host/audio_xaudio2.cpp host/uwp/main.cpp
        host/uwp/portal_backends.cpp host/uwp/settings_uwp.cpp host/uwp/uwp_compat.cpp host/uwp/cpu_check.cpp host/uwp/gamemenu_uwp.cpp)
    # The CPU check runs before any other code, so it stays on the baseline whatever SSA_UWP_MARCH (toolchain file).
    set_source_files_properties(host/uwp/cpu_check.cpp PROPERTIES COMPILE_OPTIONS "-march=btver2")
    if(NOT SSA_UWP_MARCH STREQUAL "btver2")
        target_compile_definitions(ArkeyanRuntime PRIVATE SSA_REQUIRE_X86_64_V3)
    endif()
    set_target_properties(ArkeyanRuntime PROPERTIES WIN32_EXECUTABLE ON)
    target_link_options(ArkeyanRuntime PRIVATE -municode)
    # Shared files that include SDL's headers (see SSA_UWP_DESKTOP_FAMILY in the root CMakeLists.txt).
    set_source_files_properties(host/display.cpp host/portal.cpp host/portal_emulated.cpp kernel/xam_content.cpp
        host/settings.cpp host/uwp/settings_uwp.cpp host/gamemenu_debug.cpp host/toybox.cpp host/toybox/draw.cpp host/toybox/menu.cpp host/toybox/model.cpp host/toybox/trap_villain.cpp host/uwp/gamemenu_uwp.cpp
        host/art/game_art.cpp host/art/game_text.cpp
        host/collection/catalogue.cpp host/collection/collection.cpp host/collection/icons.cpp
        PROPERTIES COMPILE_OPTIONS "${SSA_UWP_DESKTOP_FAMILY}")
    set_source_files_properties(gpu/gpu.cpp DIRECTORY gpu PROPERTIES COMPILE_OPTIONS "${SSA_UWP_DESKTOP_FAMILY}")
    set_target_properties(SSAPortalTest SSAPortalAdpcmTest SSAPortalSpeakerTest SSAFigureTest SSAArtTest SSACollectionTest SSASettingsTest SSACommandsTest SSADebugScreenTest SSARecompGpuTest PROPERTIES EXCLUDE_FROM_ALL ON)
endif()

# The game's version in its window title (games/<id>/game.txt, read by the top CMakeLists.txt): main.cpp only, so a new
# version leaves the other files' compile commands (and the kit's compile.args) unchanged.
set_source_files_properties(main.cpp PROPERTIES COMPILE_DEFINITIONS SSA_GAME_VERSION="${SSA_GAME_VERSION}")
