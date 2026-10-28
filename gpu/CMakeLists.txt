# Host renderer (plume) and its offscreen self-test.
# SSA_METAL: the Metal backend (macOS default). SSA_MACOS_VULKAN=ON builds the Vulkan backend on macOS instead (MoltenVK),
# a test configuration for the validation layers: docs/runtime/gpu/plume_backends.md.
set(SSA_METAL OFF)
if(APPLE AND NOT SSA_MACOS_VULKAN)
    set(SSA_METAL ON)
endif()
add_library(SSARecompGpu STATIC gpu.cpp draw.cpp buffer_cache.cpp worker.cpp)
if(SSA_METAL)
    target_compile_definitions(SSARecompGpu PUBLIC SSA_METAL)
endif()
if(WIN32)
    target_sources(SSARecompGpu PRIVATE d3d12_diagnostics.cpp d3d12_pipeline_cache.cpp) # DRED, removal report; PSO cache
endif()
target_include_directories(SSARecompGpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/xxHash)
target_link_libraries(SSARecompGpu PUBLIC plume SDL3::SDL3-static PRIVATE libzstd_static)

# The game's shaders, translated from the player's disc by tools/shaders/build_cache.sh (never committed, like ppc/).
set(SSA_SHADER_CACHE_SOURCE ${SSA_PPC_DIR}/shader_cache.cpp)
if(EXISTS ${SSA_SHADER_CACHE_SOURCE})
    add_library(SSAShaderCache STATIC ${SSA_SHADER_CACHE_SOURCE})
    target_include_directories(SSAShaderCache PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(SSARecompGpu PRIVATE SSA_SHADER_CACHE)
    if(SSA_METAL)
        target_compile_definitions(SSAShaderCache PRIVATE SSA_METAL)
    endif()
    target_link_libraries(SSARecompGpu PRIVATE SSAShaderCache)
    if(NOT SSA_METAL)
        # SPIR-V entries are smol-v encoded (public domain / MIT, like XenosRecomp's encoder).
        set(SMOLV_DIR ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/smol-v/source)
        target_sources(SSARecompGpu PRIVATE ${SMOLV_DIR}/smolv.cpp)
        target_include_directories(SSARecompGpu PRIVATE ${SMOLV_DIR})
    endif()
else()
    message(WARNING "No ppc/shader_cache.cpp: run tools/shaders/build_cache.sh <disc .iso> to render the game's draws")
endif()

set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})

# The baseline pipeline list (draw.cpp; how it is made: docs/runtime/gpu/pipeline_cache.md): render states and shader hashes,
# no game content. Built in, so every platform and package has it.
# One per game: each game's in games/<game>/pipelines/.
set(SSA_PIPELINE_BASELINE ${SSA_GAME_DIR}/pipelines/baseline.zst)
if(EXISTS ${SSA_PIPELINE_BASELINE})
    add_custom_command(OUTPUT ${SHADER_OUT}/pipeline_baseline.h
        COMMAND ${CMAKE_COMMAND} -DINPUT=${SSA_PIPELINE_BASELINE} -DOUTPUT=${SHADER_OUT}/pipeline_baseline.h
            -DNAME=g_pipelineBaseline -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
        DEPENDS ${SSA_PIPELINE_BASELINE} embed.cmake VERBATIM)
    target_sources(SSARecompGpu PRIVATE ${SHADER_OUT}/pipeline_baseline.h)
    target_compile_definitions(SSARecompGpu PRIVATE SSA_PIPELINE_BASELINE)
endif()

if(SSA_METAL)
    # MSL -> AIR -> metallib, embedded. Xcode 26+ ships the compiler separately.
    execute_process(COMMAND xcrun -sdk macosx metal --version RESULT_VARIABLE METAL_MISSING OUTPUT_QUIET ERROR_QUIET)
    if(METAL_MISSING)
        message(FATAL_ERROR "The Metal shader compiler is missing: run xcodebuild -downloadComponent MetalToolchain")
    endif()
    # Metal 3 (macOS 13): plume writes argument buffers as tier 2 resource IDs.
    add_custom_command(OUTPUT ${SHADER_OUT}/present.air
        COMMAND xcrun -sdk macosx metal -std=metal3.0 -mmacosx-version-min=13.0 -c ${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.metal
            -o ${SHADER_OUT}/present.air
        DEPENDS shaders/present.metal VERBATIM)
    add_custom_command(OUTPUT ${SHADER_OUT}/present.metallib
        COMMAND xcrun -sdk macosx metallib ${SHADER_OUT}/present.air -o ${SHADER_OUT}/present.metallib
        DEPENDS ${SHADER_OUT}/present.air VERBATIM)
    add_custom_command(OUTPUT ${SHADER_OUT}/present_metallib.h
        COMMAND ${CMAKE_COMMAND} -DINPUT=${SHADER_OUT}/present.metallib -DOUTPUT=${SHADER_OUT}/present_metallib.h
            -DNAME=g_presentMetallib -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
        DEPENDS ${SHADER_OUT}/present.metallib embed.cmake VERBATIM)
    target_sources(SSARecompGpu PRIVATE ${SHADER_OUT}/present_metallib.h)
else()
    # HLSL -> DXIL (D3D12) and/or SPIR-V (Vulkan), one blob per entry point, embedded as present_<entry>_<format>.h.
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

    # Desktop Windows has both backends (Vulkan first, D3D12 when there is no Vulkan device); the Xbox only D3D12.
    if(SSA_UWP)
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
            target_sources(SSARecompGpu PRIVATE ${SHADER_OUT}/present_${name}_${format}.h)
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
        target_sources(SSARecompGpu PRIVATE ${SHADER_OUT}/point_sprite_PointSpriteGS_dxil.h)
    endif()
endif()

# datadir.cpp: draw.cpp keeps pipelines.bin in the game folder (the game executable compiles it itself).
add_executable(SSARecompGpuTest selftest.cpp ../host/collection/datadir.cpp)
target_link_libraries(SSARecompGpuTest PRIVATE SSARecompGpu)
