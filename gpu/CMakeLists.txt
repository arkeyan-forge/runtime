# Host renderer (plume) and its offscreen self-test.
# ARKEYAN_METAL: the Metal backend (macOS default). ARKEYAN_MACOS_VULKAN=ON builds the Vulkan backend on macOS instead (MoltenVK),
# a test configuration for the validation layers: docs/runtime/gpu/plume_backends.md.
set(ARKEYAN_METAL OFF)
if(APPLE AND NOT ARKEYAN_MACOS_VULKAN)
    set(ARKEYAN_METAL ON)
endif()
add_library(ArkeyanRecompGpu STATIC gpu.cpp draw.cpp buffer_cache.cpp worker.cpp frame_stats.cpp upscale.cpp)
if(ARKEYAN_METAL)
    target_compile_definitions(ArkeyanRecompGpu PUBLIC ARKEYAN_METAL)
    target_include_directories(ArkeyanRecompGpu SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/plume/contrib/metal-cpp)
    target_link_libraries(ArkeyanRecompGpu PRIVATE "-framework MetalFX") # upscale.cpp: the temporal scaler
endif()
if(WIN32)
    target_sources(ArkeyanRecompGpu PRIVATE d3d12_diagnostics.cpp d3d12_pipeline_cache.cpp) # DRED, removal report; PSO cache
endif()
target_include_directories(ArkeyanRecompGpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/xxHash)
target_link_libraries(ArkeyanRecompGpu PUBLIC plume SDL3::SDL3-static PRIVATE libzstd_static)

# The game's shaders, translated from the player's disc by tools/shaders/build_cache.sh (never committed, like ppc/).
set(ARKEYAN_SHADER_CACHE_SOURCE ${ARKEYAN_PPC_DIR}/shader_cache.cpp)
if(EXISTS ${ARKEYAN_SHADER_CACHE_SOURCE})
    add_library(ArkeyanShaderCache STATIC ${ARKEYAN_SHADER_CACHE_SOURCE})
    target_include_directories(ArkeyanShaderCache PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(ArkeyanRecompGpu PRIVATE ARKEYAN_SHADER_CACHE)
    if(ARKEYAN_METAL)
        target_compile_definitions(ArkeyanShaderCache PRIVATE ARKEYAN_METAL)
    endif()
    target_link_libraries(ArkeyanRecompGpu PRIVATE ArkeyanShaderCache)
    if(NOT ARKEYAN_METAL)
        # SPIR-V entries are smol-v encoded (public domain / MIT, like XenosRecomp's encoder).
        set(SMOLV_DIR ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/smol-v/source)
        target_sources(ArkeyanRecompGpu PRIVATE ${SMOLV_DIR}/smolv.cpp)
        target_include_directories(ArkeyanRecompGpu PRIVATE ${SMOLV_DIR})
    endif()
else()
    message(WARNING "No ppc/shader_cache.cpp: run tools/shaders/build_cache.sh <disc .iso> to render the game's draws")
endif()

set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})

# The baseline pipeline list (draw.cpp; how it is made: docs/runtime/gpu/pipeline_cache.md): render states and shader hashes,
# no game content. Built in, so every platform and package has it.
# One per game: each game's in games/<game>/pipelines/.
set(ARKEYAN_PIPELINE_BASELINE ${ARKEYAN_GAME_DIR}/pipelines/baseline.zst)
if(EXISTS ${ARKEYAN_PIPELINE_BASELINE})
    add_custom_command(OUTPUT ${SHADER_OUT}/pipeline_baseline.h
        COMMAND ${CMAKE_COMMAND} -DINPUT=${ARKEYAN_PIPELINE_BASELINE} -DOUTPUT=${SHADER_OUT}/pipeline_baseline.h
            -DNAME=g_pipelineBaseline -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
        DEPENDS ${ARKEYAN_PIPELINE_BASELINE} embed.cmake VERBATIM)
endif()
# The game's id and its baseline list are the only inputs of this library that differ between games: an object of their
# own, linked into whatever links the library (the kit keeps it per game; the library is the same for every game).
add_library(ArkeyanGameInputs OBJECT game_inputs.cpp)
target_include_directories(ArkeyanGameInputs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR})
if(EXISTS ${ARKEYAN_PIPELINE_BASELINE})
    target_sources(ArkeyanGameInputs PRIVATE ${SHADER_OUT}/pipeline_baseline.h)
    target_compile_definitions(ArkeyanGameInputs PRIVATE ARKEYAN_PIPELINE_BASELINE)
endif()
target_link_libraries(ArkeyanRecompGpu PUBLIC $<TARGET_OBJECTS:ArkeyanGameInputs>)

# dxc is the prebuilt one in XenosRecomp's dxc-bin submodule, picked for the build machine.
set(DXC_BIN ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/dxc-bin)
if(CMAKE_HOST_WIN32 AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "ARM64")
    set(DXC ${DXC_BIN}/bin/arm64/dxc.exe)
elseif(CMAKE_HOST_WIN32)
    set(DXC ${DXC_BIN}/bin/x64/dxc.exe)
elseif(CMAKE_HOST_APPLE)
    set(DXC ${CMAKE_COMMAND} -E env DYLD_LIBRARY_PATH=${DXC_BIN}/lib/x64 ${DXC_BIN}/bin/x64/dxc-macos)
else()
    set(DXC ${CMAKE_COMMAND} -E env LD_LIBRARY_PATH=${DXC_BIN}/lib/x64 ${DXC_BIN}/bin/x64/dxc-linux)
endif()
if(NOT EXISTS ${DXC_BIN}/bin/x64/dxc.exe)
    message(FATAL_ERROR "dxc is missing: run git submodule update --init thirdparty/XenosRecomp/thirdparty/dxc-bin")
endif()

if(ARKEYAN_METAL)
    # MSL -> AIR -> metallib, embedded. Xcode 26+ ships the compiler separately.
    execute_process(COMMAND xcrun -sdk macosx metal --version RESULT_VARIABLE METAL_MISSING OUTPUT_QUIET ERROR_QUIET)
    if(METAL_MISSING)
        message(FATAL_ERROR "The Metal shader compiler is missing: run xcodebuild -downloadComponent MetalToolchain")
    endif()
    # Metal 3 (macOS 13): plume writes argument buffers as tier 2 resource IDs.
    add_custom_command(OUTPUT ${SHADER_OUT}/present.air
        COMMAND xcrun -sdk macosx metal -fmodules-cache-path=${CMAKE_CURRENT_BINARY_DIR}/metal-cache -std=metal3.0 -mmacosx-version-min=13.0 -c ${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.metal
            -o ${SHADER_OUT}/present.air
        DEPENDS shaders/present.metal VERBATIM)
    add_custom_command(OUTPUT ${SHADER_OUT}/present.metallib
        COMMAND xcrun -sdk macosx metallib ${SHADER_OUT}/present.air -o ${SHADER_OUT}/present.metallib
        DEPENDS ${SHADER_OUT}/present.air VERBATIM)
    add_custom_command(OUTPUT ${SHADER_OUT}/present_metallib.h
        COMMAND ${CMAKE_COMMAND} -DINPUT=${SHADER_OUT}/present.metallib -DOUTPUT=${SHADER_OUT}/present_metallib.h
            -DNAME=g_presentMetallib -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
        DEPENDS ${SHADER_OUT}/present.metallib embed.cmake VERBATIM)
    target_sources(ArkeyanRecompGpu PRIVATE ${SHADER_OUT}/present_metallib.h)
else()
    # HLSL -> DXIL (D3D12) and/or SPIR-V (Vulkan), one blob per entry point, embedded as present_<entry>_<format>.h.

    # Desktop Windows has both backends (Vulkan first, D3D12 when there is no Vulkan device); the Xbox only D3D12.
    if(ARKEYAN_UWP)
        set(formats dxil)
    elseif(WIN32)
        set(formats dxil spirv)
    else()
        set(formats spirv)
    endif()
    foreach(entry PresentVS:vs_6_0 PresentPS:ps_6_0 ReadbackCS:cs_6_0 ReadbackRawCS:cs_6_0 ResolveDepthPS:ps_6_0 OverlayPS:ps_6_0)
        string(REPLACE ":" ";" entry ${entry})
        list(GET entry 0 name)
        list(GET entry 1 profile)
        foreach(format ${formats})
            if(format STREQUAL "dxil")
                # dxc leaves the container hash empty without its validator library (macOS); embed_dxil.cmake sets it.
                set(blob ${SHADER_OUT}/present_${name}.dxil)
                set(dxc_flags)
                set(embed_script embed_dxil.cmake)
                set(symbol g_present${name}Dxil)
            else()
                set(blob ${SHADER_OUT}/present_${name}.spv)
                set(dxc_flags -spirv)
                if(profile MATCHES "^vs_")
                    list(APPEND dxc_flags -fvk-invert-y)
                endif()
                set(embed_script embed.cmake)
                set(symbol g_present${name}Spirv)
            endif()
            add_custom_command(OUTPUT ${blob}
                COMMAND ${DXC} -nologo -T ${profile} -E ${name} ${dxc_flags} -Fo ${blob} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.hlsl
                DEPENDS shaders/present.hlsl VERBATIM)
            add_custom_command(OUTPUT ${SHADER_OUT}/present_${name}_${format}.h
                COMMAND ${CMAKE_COMMAND} -DINPUT=${blob} -DOUTPUT=${SHADER_OUT}/present_${name}_${format}.h -DNAME=${symbol}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/${embed_script}
                DEPENDS ${blob} ${embed_script} VERBATIM)
            target_sources(ArkeyanRecompGpu PRIVATE ${SHADER_OUT}/present_${name}_${format}.h)
        endforeach()
    endforeach()

    if(WIN32)
        # D3D12 point sprites for the game's draws (draw.cpp): a geometry shader, DXIL only.
        add_custom_command(OUTPUT ${SHADER_OUT}/point_sprite_PointSpriteGS.dxil
            COMMAND ${DXC} -nologo -T gs_6_0 -E PointSpriteGS -Fo ${SHADER_OUT}/point_sprite_PointSpriteGS.dxil
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/point_sprite.hlsl
            DEPENDS shaders/point_sprite.hlsl VERBATIM)
        add_custom_command(OUTPUT ${SHADER_OUT}/point_sprite_PointSpriteGS_dxil.h
            COMMAND ${CMAKE_COMMAND} -DINPUT=${SHADER_OUT}/point_sprite_PointSpriteGS.dxil
                -DOUTPUT=${SHADER_OUT}/point_sprite_PointSpriteGS_dxil.h -DNAME=g_pointSpritePointSpriteGSDxil
                -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_dxil.cmake
            DEPENDS ${SHADER_OUT}/point_sprite_PointSpriteGS.dxil embed_dxil.cmake VERBATIM)
        target_sources(ArkeyanRecompGpu PRIVATE ${SHADER_OUT}/point_sprite_PointSpriteGS_dxil.h)
    endif()
endif()

# FSR 3.1: the FidelityFX SDK, its shaders, our backend (gpu/fsr/).
include(fsr/fsr.cmake)

# datadir.cpp: draw.cpp keeps pipelines.bin in the game folder (the game executable compiles it itself); the write watch
# and the platform layer under it: the buffer cache asks the watch (the game executable compiles them too).
set(ARKEYAN_GPU_TEST_PLATFORM ../platform/platform_posix.cpp)
if(WIN32)
    set(ARKEYAN_GPU_TEST_PLATFORM ../platform/platform_win32.cpp)
    set_source_files_properties(../platform/platform_win32.cpp PROPERTIES COMPILE_DEFINITIONS _WIN32_WINNT=0x0A00)
endif()
add_executable(ArkeyanRecompGpuTest selftest.cpp ../host/collection/datadir.cpp ../write_watch.cpp ${ARKEYAN_GPU_TEST_PLATFORM})
if(WIN32)
    target_link_libraries(ArkeyanRecompGpuTest PRIVATE winmm)
endif()
target_link_libraries(ArkeyanRecompGpuTest PRIVATE ArkeyanRecompGpu)
