# AMD FSR 3.1 (FidelityFX SDK v1.1.4, thirdparty/FidelityFX-SDK, MIT, unmodified): its host code as a library, its
# shaders built for the running backend, and our backend over plume (docs/runtime/gpu/upscaling.md).
# Included by runtime/gpu/CMakeLists.txt, which sets ARKEYAN_METAL, SHADER_OUT and DXC.
set(FFX_SDK ${CMAKE_SOURCE_DIR}/thirdparty/FidelityFX-SDK/sdk)
if(NOT EXISTS ${FFX_SDK}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp)
    message(FATAL_ERROR "The FidelityFX SDK is missing: run git submodule update --init thirdparty/FidelityFX-SDK")
endif()

# The SDK's host code is written for Windows: two-byte wchar_t (its opaque context is sized for it) and a few MSVC
# functions, which ffx_port.h gives. Every file that includes an SDK header is built with the same options.
add_library(ArkeyanFfx STATIC
    ${FFX_SDK}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
    ${FFX_SDK}/src/shared/ffx_assert.cpp
    ${FFX_SDK}/src/shared/ffx_message.cpp
    ${FFX_SDK}/src/shared/ffx_object_management.cpp)
set(FFX_OPTIONS -include ${CMAKE_CURRENT_LIST_DIR}/ffx_port.h)
if(NOT MSVC)
    list(APPEND FFX_OPTIONS -fshort-wchar)
endif()
target_include_directories(ArkeyanFfx SYSTEM PRIVATE ${FFX_SDK}/include ${FFX_SDK}/src/shared)
target_compile_options(ArkeyanFfx PRIVATE ${FFX_OPTIONS} -w) # third-party code, as it is

# The shaders: every pass of FSR 3.1's upscaler in the permutations we use: SDR and HDR colour (a game whose composite
# reads a float scene: its tone mapping comes after), motion at the render size, depth not inverted (the inputs pass
# normalises it), no LUT, no FP16, wave 32; plus the accumulation with sharpening.
set(FSR_PASSES prepare_inputs luma_pyramid shading_change_pyramid shading_change prepare_reactivity luma_instability
    accumulate accumulate_sharpen rcas debug_view autogen_reactive)
set(FSR_DEFINES -DFFX_GPU=1 -DFFX_HLSL=1
    -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1
    -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2 -DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0
    -DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1
    -DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0 -DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=0)
# SPIR-V bindings: one number space per set, so each register class gets its own range (gpu/fsr/backend.cpp).
set(FSR_SPIRV_SHIFTS -fvk-b-shift 0 0 -fvk-s-shift 8 0 -fvk-t-shift 16 0 -fvk-u-shift 48 0)
set(FSR_SHADER_DIR ${FFX_SDK}/src/backends/dx12/shaders/fsr3upscaler)
set(FSR_INCLUDES -I ${FFX_SDK}/include/FidelityFX/gpu -I ${FFX_SDK}/include/FidelityFX/gpu/fsr3upscaler)

if(ARKEYAN_METAL)
    # SPIR-V -> MSL (argument buffers: plume binds set 0 at [[buffer(0)]]) with MoltenVK's converter (Homebrew's
    # molten-vk), then metallib. Only the build machine needs it; the kit carries the metallibs.
    find_program(MVK_CONVERTER MoltenVKShaderConverter HINTS /opt/homebrew/bin /usr/local/bin)
    if(NOT MVK_CONVERTER)
        message(FATAL_ERROR "MoltenVKShaderConverter is missing (FSR's Metal shaders): brew install molten-vk")
    endif()
    set(fsr_formats metal)
elseif(ARKEYAN_UWP)
    set(fsr_formats dxil)
elseif(WIN32)
    set(fsr_formats dxil spirv)
else()
    set(fsr_formats spirv)
endif()

set(FSR_SHADER_LIST "")
foreach(variant ${FSR_PASSES})
  foreach(hdr 0 1)
    set(pass ${variant})
    if(hdr)
        set(pass ${variant}_hdr)
    endif()
    set(source ${variant})
    set(defines ${FSR_DEFINES} -DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=${hdr} -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=0)
    if(variant STREQUAL "accumulate_sharpen")
        set(source accumulate)
        set(defines ${FSR_DEFINES} -DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=${hdr} -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=1)
    endif()
    set(hlsl ${FSR_SHADER_DIR}/ffx_fsr3upscaler_${source}_pass.hlsl)
    set(dxc_common -nologo -T cs_6_2 -E CS -Wno-for-redefinition -Wno-ambig-lit-shift ${defines} ${FSR_INCLUDES})
    file(GLOB fsr_headers ${FFX_SDK}/include/FidelityFX/gpu/fsr3upscaler/*.h ${FFX_SDK}/include/FidelityFX/gpu/*.h)

    # SPIR-V: the Vulkan shader, and on every backend the reflection (names, registers) the backend reads.
    set(spv ${SHADER_OUT}/fsr_${pass}.spv)
    add_custom_command(OUTPUT ${spv}
        COMMAND ${DXC} ${dxc_common} -spirv -fspv-target-env=vulkan1.1 -fvk-use-dx-layout ${FSR_SPIRV_SHIFTS} -Fo ${spv} ${hlsl}
        DEPENDS ${hlsl} ${fsr_headers} VERBATIM)
    add_custom_command(OUTPUT ${SHADER_OUT}/fsr_${pass}_spirv.h
        COMMAND ${CMAKE_COMMAND} -DINPUT=${spv} -DOUTPUT=${SHADER_OUT}/fsr_${pass}_spirv.h -DNAME=g_fsr_${pass}_spirv
            -P ${CMAKE_CURRENT_LIST_DIR}/../embed.cmake
        DEPENDS ${spv} ${CMAKE_CURRENT_LIST_DIR}/../embed.cmake VERBATIM)
    list(APPEND FSR_SHADER_HEADERS ${SHADER_OUT}/fsr_${pass}_spirv.h)
    string(APPEND FSR_SHADER_LIST "#include \"fsr_${pass}_spirv.h\"\n")

    if("dxil" IN_LIST fsr_formats)
        set(dxil ${SHADER_OUT}/fsr_${pass}.dxil)
        add_custom_command(OUTPUT ${dxil}
            COMMAND ${DXC} ${dxc_common} -Fo ${dxil} ${hlsl}
            DEPENDS ${hlsl} ${fsr_headers} VERBATIM)
        add_custom_command(OUTPUT ${SHADER_OUT}/fsr_${pass}_dxil.h
            COMMAND ${CMAKE_COMMAND} -DINPUT=${dxil} -DOUTPUT=${SHADER_OUT}/fsr_${pass}_dxil.h -DNAME=g_fsr_${pass}_dxil
                -P ${CMAKE_CURRENT_LIST_DIR}/../embed_dxil.cmake
            DEPENDS ${dxil} ${CMAKE_CURRENT_LIST_DIR}/../embed_dxil.cmake VERBATIM)
        list(APPEND FSR_SHADER_HEADERS ${SHADER_OUT}/fsr_${pass}_dxil.h)
        string(APPEND FSR_SHADER_LIST "#include \"fsr_${pass}_dxil.h\"\n")
    endif()

    if("metal" IN_LIST fsr_formats)
        set(msl ${SHADER_OUT}/fsr_${pass}.metal)
        add_custom_command(OUTPUT ${msl}
            COMMAND ${MVK_CONVERTER} -si ${spv} -mo ${msl} -mab -mv 3.0 -q
            DEPENDS ${spv} VERBATIM)
        add_custom_command(OUTPUT ${SHADER_OUT}/fsr_${pass}.air
            COMMAND xcrun -sdk macosx metal -fmodules-cache-path=${CMAKE_CURRENT_BINARY_DIR}/metal-cache -std=metal3.0
                -mmacosx-version-min=13.0 -w -c ${msl} -o ${SHADER_OUT}/fsr_${pass}.air
            DEPENDS ${msl} VERBATIM)
        add_custom_command(OUTPUT ${SHADER_OUT}/fsr_${pass}.metallib
            COMMAND xcrun -sdk macosx metallib ${SHADER_OUT}/fsr_${pass}.air -o ${SHADER_OUT}/fsr_${pass}.metallib
            DEPENDS ${SHADER_OUT}/fsr_${pass}.air VERBATIM)
        add_custom_command(OUTPUT ${SHADER_OUT}/fsr_${pass}_metallib.h
            COMMAND ${CMAKE_COMMAND} -DINPUT=${SHADER_OUT}/fsr_${pass}.metallib
                -DOUTPUT=${SHADER_OUT}/fsr_${pass}_metallib.h -DNAME=g_fsr_${pass}_metallib
                -P ${CMAKE_CURRENT_LIST_DIR}/../embed.cmake
            DEPENDS ${SHADER_OUT}/fsr_${pass}.metallib ${CMAKE_CURRENT_LIST_DIR}/../embed.cmake VERBATIM)
        # The argument buffer's layout, read from the MSL: what plume's descriptor set must match.
        add_custom_command(OUTPUT ${SHADER_OUT}/fsr_${pass}_msl.h
            COMMAND ${CMAKE_COMMAND} -DINPUT=${msl} -DOUTPUT=${SHADER_OUT}/fsr_${pass}_msl.h -DNAME=g_fsr_${pass}_msl
                -P ${CMAKE_CURRENT_LIST_DIR}/msl_table.cmake
            DEPENDS ${msl} ${CMAKE_CURRENT_LIST_DIR}/msl_table.cmake VERBATIM)
        list(APPEND FSR_SHADER_HEADERS ${SHADER_OUT}/fsr_${pass}_metallib.h ${SHADER_OUT}/fsr_${pass}_msl.h)
        string(APPEND FSR_SHADER_LIST "#include \"fsr_${pass}_metallib.h\"\n#include \"fsr_${pass}_msl.h\"\n")
    endif()
  endforeach()
endforeach()

# Our inputs pass (motion, depth) and the clears, as the present pass does: one MSL file; HLSL: fsr_inputs.hlsl, and
# fsr_layers.hlsl for InputsLayersCS (its own push constant block).
if(ARKEYAN_METAL)
    add_custom_command(OUTPUT ${SHADER_OUT}/fsr_inputs.air
        COMMAND xcrun -sdk macosx metal -fmodules-cache-path=${CMAKE_CURRENT_BINARY_DIR}/metal-cache -std=metal3.0
            -mmacosx-version-min=13.0 -c ${CMAKE_CURRENT_LIST_DIR}/fsr_inputs.metal -o ${SHADER_OUT}/fsr_inputs.air
        DEPENDS ${CMAKE_CURRENT_LIST_DIR}/fsr_inputs.metal VERBATIM)
    add_custom_command(OUTPUT ${SHADER_OUT}/fsr_inputs.metallib
        COMMAND xcrun -sdk macosx metallib ${SHADER_OUT}/fsr_inputs.air -o ${SHADER_OUT}/fsr_inputs.metallib
        DEPENDS ${SHADER_OUT}/fsr_inputs.air VERBATIM)
    add_custom_command(OUTPUT ${SHADER_OUT}/fsr_inputs_metallib.h
        COMMAND ${CMAKE_COMMAND} -DINPUT=${SHADER_OUT}/fsr_inputs.metallib -DOUTPUT=${SHADER_OUT}/fsr_inputs_metallib.h
            -DNAME=g_fsr_inputs_metallib -P ${CMAKE_CURRENT_LIST_DIR}/../embed.cmake
        DEPENDS ${SHADER_OUT}/fsr_inputs.metallib ${CMAKE_CURRENT_LIST_DIR}/../embed.cmake VERBATIM)
    list(APPEND FSR_SHADER_HEADERS ${SHADER_OUT}/fsr_inputs_metallib.h)
    string(APPEND FSR_SHADER_LIST "#include \"fsr_inputs_metallib.h\"\n")
else()
    foreach(entry InputsCS InputsLayersCS ClearFloatCS ClearUintCS SanitizeCS)
        set(source ${CMAKE_CURRENT_LIST_DIR}/fsr_inputs.hlsl)
        if(entry STREQUAL "InputsLayersCS")
            set(source ${CMAKE_CURRENT_LIST_DIR}/fsr_layers.hlsl)
        endif()
        foreach(format ${fsr_formats})
            if(format STREQUAL "dxil")
                set(blob ${SHADER_OUT}/fsr_${entry}.dxil)
                set(flags)
                set(script embed_dxil.cmake)
            else()
                set(blob ${SHADER_OUT}/fsr_${entry}.spv)
                set(flags -spirv -fspv-target-env=vulkan1.1)
                set(script embed.cmake)
            endif()
            add_custom_command(OUTPUT ${blob}
                COMMAND ${DXC} -nologo -T cs_6_0 -E ${entry} ${flags} -Fo ${blob} ${source}
                DEPENDS ${source} VERBATIM)
            add_custom_command(OUTPUT ${SHADER_OUT}/fsr_${entry}_${format}.h
                COMMAND ${CMAKE_COMMAND} -DINPUT=${blob} -DOUTPUT=${SHADER_OUT}/fsr_${entry}_${format}.h
                    -DNAME=g_fsr_${entry}_${format} -P ${CMAKE_CURRENT_LIST_DIR}/../${script}
                DEPENDS ${blob} ${CMAKE_CURRENT_LIST_DIR}/../${script} VERBATIM)
            list(APPEND FSR_SHADER_HEADERS ${SHADER_OUT}/fsr_${entry}_${format}.h)
            string(APPEND FSR_SHADER_LIST "#include \"fsr_${entry}_${format}.h\"\n")
        endforeach()
    endforeach()
endif()
# Frame generation's shaders and host code (frame interpolation, optical flow).
include(${CMAKE_CURRENT_LIST_DIR}/framegen.cmake)
file(CONFIGURE OUTPUT ${SHADER_OUT}/fsr_shaders.h CONTENT "#pragma once\n// Generated by gpu/fsr/fsr.cmake.\n${FSR_SHADER_LIST}")

# backend.cpp and fsr.cpp include the SDK's headers: the SDK's options on these two only (wide strings elsewhere keep
# their size).
target_sources(ArkeyanRecompGpu PRIVATE ${CMAKE_CURRENT_LIST_DIR}/backend.cpp ${CMAKE_CURRENT_LIST_DIR}/fsr.cpp
    ${CMAKE_CURRENT_LIST_DIR}/spirv_reflect.cpp ${FSR_SHADER_HEADERS})
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/backend.cpp ${CMAKE_CURRENT_LIST_DIR}/fsr.cpp PROPERTIES
    COMPILE_OPTIONS "${FFX_OPTIONS}")
target_include_directories(ArkeyanRecompGpu SYSTEM PRIVATE ${FFX_SDK}/include ${FFX_SDK}/src/components)
target_link_libraries(ArkeyanRecompGpu PRIVATE ArkeyanFfx)
