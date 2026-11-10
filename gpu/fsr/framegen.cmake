# FSR 3.1 frame generation: the FidelityFX SDK's frame interpolation and optical flow (v1.1.4, unmodified), their host
# code into ArkeyanFfx and their shaders built like the upscaler's (gpu/fsr/fsr.cmake, which includes this file before it
# writes fsr_shaders.h). docs/runtime/gpu/upscaling.md, "Frame generation".
target_sources(ArkeyanFfx PRIVATE
    ${FFX_SDK}/src/components/frameinterpolation/ffx_frameinterpolation.cpp
    ${FFX_SDK}/src/components/opticalflow/ffx_opticalflow.cpp)
# Its pass names get ffx_port.h's two-byte std::wstring (the C++ library's is four-byte: a heap fault at -O2).
if(NOT WIN32)
    set_source_files_properties(${FFX_SDK}/src/components/opticalflow/ffx_opticalflow.cpp PROPERTIES COMPILE_DEFINITIONS FFX_PORT_WSTRING)
endif()
# Ours: it includes the SDK's headers, so it is built with the SDK's options (fsr.cmake's FFX_OPTIONS).
target_sources(ArkeyanRecompGpu PRIVATE ${CMAKE_CURRENT_LIST_DIR}/framegen.cpp)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/framegen.cpp PROPERTIES COMPILE_OPTIONS "${FFX_OPTIONS}")

# One shader for every format of the running backend: SPIR-V always (the reflection), DXIL on Windows, MSL + metallib
# on Metal (with its argument buffer table). Appends to FSR_SHADER_HEADERS and FSR_SHADER_LIST.
function(ffx_shader name hlsl)
    set(dxc_common -nologo -T cs_6_2 -E CS -Wno-for-redefinition -Wno-ambig-lit-shift ${ARGN})
    get_filename_component(dir ${hlsl} DIRECTORY)
    file(GLOB headers ${FFX_SDK}/include/FidelityFX/gpu/*.h ${FFX_SDK}/include/FidelityFX/gpu/frameinterpolation/*.h
        ${FFX_SDK}/include/FidelityFX/gpu/opticalflow/*.h ${FFX_SDK}/include/FidelityFX/gpu/spd/*.h)
    set(spv ${SHADER_OUT}/fsr_${name}.spv)
    add_custom_command(OUTPUT ${spv}
        COMMAND ${DXC} ${dxc_common} -spirv -fspv-target-env=vulkan1.1 -fvk-use-dx-layout ${FSR_SPIRV_SHIFTS} -Fo ${spv} ${hlsl}
        DEPENDS ${hlsl} ${headers} VERBATIM)
    add_custom_command(OUTPUT ${SHADER_OUT}/fsr_${name}_spirv.h
        COMMAND ${CMAKE_COMMAND} -DINPUT=${spv} -DOUTPUT=${SHADER_OUT}/fsr_${name}_spirv.h -DNAME=g_fsr_${name}_spirv
            -P ${FSR_CMAKE_DIR}/../embed.cmake
        DEPENDS ${spv} ${FSR_CMAKE_DIR}/../embed.cmake VERBATIM)
    set(out ${SHADER_OUT}/fsr_${name}_spirv.h)
    set(list "#include \"fsr_${name}_spirv.h\"\n")
    if("dxil" IN_LIST fsr_formats)
        set(dxil ${SHADER_OUT}/fsr_${name}.dxil)
        add_custom_command(OUTPUT ${dxil} COMMAND ${DXC} ${dxc_common} -Fo ${dxil} ${hlsl} DEPENDS ${hlsl} ${headers} VERBATIM)
        add_custom_command(OUTPUT ${SHADER_OUT}/fsr_${name}_dxil.h
            COMMAND ${CMAKE_COMMAND} -DINPUT=${dxil} -DOUTPUT=${SHADER_OUT}/fsr_${name}_dxil.h -DNAME=g_fsr_${name}_dxil
                -P ${FSR_CMAKE_DIR}/../embed_dxil.cmake
            DEPENDS ${dxil} ${FSR_CMAKE_DIR}/../embed_dxil.cmake VERBATIM)
        list(APPEND out ${SHADER_OUT}/fsr_${name}_dxil.h)
        string(APPEND list "#include \"fsr_${name}_dxil.h\"\n")
    endif()
    if("metal" IN_LIST fsr_formats)
        set(msl ${SHADER_OUT}/fsr_${name}.metal)
        add_custom_command(OUTPUT ${msl} COMMAND ${MVK_CONVERTER} -si ${spv} -mo ${msl} -mab -mv 3.0 -q DEPENDS ${spv} VERBATIM)
        add_custom_command(OUTPUT ${SHADER_OUT}/fsr_${name}.metallib
            COMMAND xcrun -sdk macosx metal -fmodules-cache-path=${CMAKE_CURRENT_BINARY_DIR}/metal-cache -std=metal3.0
                -mmacosx-version-min=13.0 -w -c ${msl} -o ${SHADER_OUT}/fsr_${name}.air
            COMMAND xcrun -sdk macosx metallib ${SHADER_OUT}/fsr_${name}.air -o ${SHADER_OUT}/fsr_${name}.metallib
            DEPENDS ${msl} VERBATIM)
        add_custom_command(OUTPUT ${SHADER_OUT}/fsr_${name}_metallib.h
            COMMAND ${CMAKE_COMMAND} -DINPUT=${SHADER_OUT}/fsr_${name}.metallib
                -DOUTPUT=${SHADER_OUT}/fsr_${name}_metallib.h -DNAME=g_fsr_${name}_metallib
                -P ${FSR_CMAKE_DIR}/../embed.cmake
            DEPENDS ${SHADER_OUT}/fsr_${name}.metallib ${FSR_CMAKE_DIR}/../embed.cmake VERBATIM)
        add_custom_command(OUTPUT ${SHADER_OUT}/fsr_${name}_msl.h
            COMMAND ${CMAKE_COMMAND} -DINPUT=${msl} -DOUTPUT=${SHADER_OUT}/fsr_${name}_msl.h -DNAME=g_fsr_${name}_msl
                -P ${FSR_CMAKE_DIR}/msl_table.cmake
            DEPENDS ${msl} ${FSR_CMAKE_DIR}/msl_table.cmake VERBATIM)
        list(APPEND out ${SHADER_OUT}/fsr_${name}_metallib.h ${SHADER_OUT}/fsr_${name}_msl.h)
        string(APPEND list "#include \"fsr_${name}_metallib.h\"\n#include \"fsr_${name}_msl.h\"\n")
    endif()
    set(FSR_SHADER_HEADERS ${FSR_SHADER_HEADERS} ${out} PARENT_SCOPE)
    set(FSR_SHADER_LIST "${FSR_SHADER_LIST}${list}" PARENT_SCOPE)
endfunction()

set(FSR_CMAKE_DIR ${CMAKE_CURRENT_LIST_DIR})
set(FFX_GPU_DIR ${FFX_SDK}/include/FidelityFX/gpu)

# Frame interpolation, in the one permutation used: motion at the render size, not jittered, depth not inverted (the
# upscaler's dilated depth, 0 near), no FP16, wave 32. The pass order is FfxFrameInterpolationPass's.
set(FI_DEFINES -DFFX_GPU=1 -DFFX_HLSL=1
    -DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1
    -DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
    -DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1 -DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS=0
    -DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=0
    -I ${FFX_GPU_DIR} -I ${FFX_GPU_DIR}/frameinterpolation)
set(FI_DIR ${FFX_SDK}/src/backends/dx12/shaders/frameinterpolation)
foreach(pass reconstruct_and_dilate setup reconstruct_previous_depth game_motion_vector_field
        optical_flow_vector_field disocclusion_mask pass compute_inpainting_pyramid inpainting
        compute_game_vector_field_inpainting_pyramid debug_view)
    set(file ${FI_DIR}/ffx_frameinterpolation_${pass}_pass.hlsl)
    if(pass STREQUAL "pass")
        set(file ${FI_DIR}/ffx_frameinterpolation_pass.hlsl)
    endif()
    ffx_shader(fi_${pass} ${file} ${FI_DEFINES})
endforeach()

# Optical flow: SDR colour (the frame), no FP16, wave 32.
set(OF_DEFINES -DFFX_GPU=1 -DFFX_HLSL=1 -DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=0
    -I ${FFX_GPU_DIR} -I ${FFX_GPU_DIR}/opticalflow)
set(OF_DIR ${FFX_SDK}/src/backends/dx12/shaders/opticalflow)
foreach(pass prepare_luma compute_luminance_pyramid generate_scd_histogram compute_scd_divergence
        compute_optical_flow_advanced_pass_v5 filter_optical_flow_pass_v5 scale_optical_flow_advanced_pass_v5)
    string(REGEX REPLACE "_pass_v5$" "_v5" short ${pass})
    set(file ${OF_DIR}/ffx_opticalflow_${pass}.hlsl)
    if(NOT pass MATCHES "_v5$")
        set(file ${OF_DIR}/ffx_opticalflow_${pass}_pass.hlsl)
    endif()
    ffx_shader(of_${short} ${file} ${OF_DEFINES})
endforeach()
