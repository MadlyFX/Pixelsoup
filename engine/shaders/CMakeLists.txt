# Shader bytecode for the GPU output path.
#
# The plan's long-term shape is one universal HLSL source compiled to SPIR-V
# and DXIL. That is what this does, with the honest limitation that it emits
# only the formats a compiler on *this* machine can produce: the Windows SDK
# ships a dxc without the SPIR-V backend, so a Windows build yields DXIL alone.
#
# A configuration that can compile neither still builds. The GPU presenter is
# then simply not constructible and the shell says so and uses the CPU path,
# which is better than a build failure on a machine that was never going to run
# the GPU path anyway.

set(MEDIASERVER_SHADER_OUTPUT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/mediaserver/shaders"
)
file(MAKE_DIRECTORY "${MEDIASERVER_SHADER_OUTPUT_DIR}")

add_library(mediaserver_shaders INTERFACE)
add_library(MediaServer::Shaders ALIAS mediaserver_shaders)
target_include_directories(mediaserver_shaders
    INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

# The Windows SDK installs dxc beside the other x64 tools. Prefer whatever is
# on PATH so a developer can point at a newer DirectXShaderCompiler release -
# notably one built with SPIR-V codegen enabled.
set(_mediaserver_dxc_hints "")
file(GLOB _mediaserver_sdk_bins
    "C:/Program Files (x86)/Windows Kits/10/bin/*/x64"
    "C:/Program Files/Windows Kits/10/bin/*/x64"
)
list(SORT _mediaserver_sdk_bins)
list(REVERSE _mediaserver_sdk_bins)
list(APPEND _mediaserver_dxc_hints ${_mediaserver_sdk_bins})

find_program(MEDIASERVER_DXC
    NAMES dxc dxc.exe
    HINTS ${_mediaserver_dxc_hints}
    DOC "DirectX shader compiler used to produce DXIL (and SPIR-V when built with it)"
)

set(_mediaserver_shader_headers "")

# stage: vs_6_0 or ps_6_0. Emits a C header with a named byte array, which
# keeps the bytecode inside the executable and out of the runtime file layout.
function(mediaserver_compile_dxil source stage symbol output_name)
    set(_output "${MEDIASERVER_SHADER_OUTPUT_DIR}/${output_name}")
    add_custom_command(
        OUTPUT "${_output}"
        COMMAND "${MEDIASERVER_DXC}"
                -T ${stage}
                -E main
                -Fh "${_output}"
                -Vn ${symbol}
                -WX
                "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        COMMENT "Compiling ${source} to DXIL"
        VERBATIM
    )
    set(_mediaserver_shader_headers
        ${_mediaserver_shader_headers} "${_output}"
        PARENT_SCOPE
    )
endfunction()

if(MEDIASERVER_DXC)
    # The 2D output path: one instanced quad per rectangle or glyph.
    mediaserver_compile_dxil(
        quad.vert.hlsl vs_6_0 mediaserver_quad_vert_dxil quad_vert_dxil.h)
    mediaserver_compile_dxil(
        quad.frag.hlsl ps_6_0 mediaserver_quad_frag_dxil quad_frag_dxil.h)
    # The 3D path: fully pulled geometry, depth tested, metallic-roughness.
    mediaserver_compile_dxil(
        scene.vert.hlsl vs_6_0 mediaserver_scene_vert_dxil scene_vert_dxil.h)
    mediaserver_compile_dxil(
        scene.frag.hlsl ps_6_0 mediaserver_scene_frag_dxil scene_frag_dxil.h)
    # The media clip pass reuses the pulled scene vertex stage and samples the
    # clip texture in its own fragment stage.
    mediaserver_compile_dxil(
        scene_media.frag.hlsl ps_6_0
        mediaserver_scene_media_frag_dxil scene_media_frag_dxil.h)
    # The output stage: a fullscreen triangle that samples a source texture and
    # applies the feed-map crop and soft-edge blend.
    mediaserver_compile_dxil(
        output.vert.hlsl vs_6_0 mediaserver_output_vert_dxil output_vert_dxil.h)
    mediaserver_compile_dxil(
        output.frag.hlsl ps_6_0 mediaserver_output_frag_dxil output_frag_dxil.h)
    # Arbitrary LED surfaces: one asymmetric scene render, then physical mesh
    # XYZ projected into it while UV0 places triangles in connector space.
    mediaserver_compile_dxil(
        projection_mesh_warp.vert.hlsl vs_6_0
        mediaserver_projection_mesh_warp_vert_dxil
        projection_mesh_warp_vert_dxil.h)
    mediaserver_compile_dxil(
        projection_mesh_warp.frag.hlsl ps_6_0
        mediaserver_projection_mesh_warp_frag_dxil
        projection_mesh_warp_frag_dxil.h)
    mediaserver_compile_dxil(
        scene_mix.frag.hlsl ps_6_0
        mediaserver_scene_mix_frag_dxil scene_mix_frag_dxil.h)
    # A two-pass separable Gaussian blur reuses the output fullscreen vertex
    # stage and selects its axis through one small fragment constant block.
    mediaserver_compile_dxil(
        gaussian_blur.frag.hlsl ps_6_0
        mediaserver_gaussian_blur_frag_dxil gaussian_blur_frag_dxil.h)
    # Box and directional blur are distinct spatial processors. They share
    # neither Gaussian weights nor Gaussian shader bytecode.
    mediaserver_compile_dxil(
        box_blur.frag.hlsl ps_6_0
        mediaserver_box_blur_frag_dxil box_blur_frag_dxil.h)
    mediaserver_compile_dxil(
        directional_blur.frag.hlsl ps_6_0
        mediaserver_directional_blur_frag_dxil directional_blur_frag_dxil.h)
    # Keyers calculate a matte first, optionally reshape it, then compose the
    # untouched premultiplied source against that matte. Keeping these stages
    # distinct makes spill and source-alpha coverage ordering explicit.
    mediaserver_compile_dxil(
        chroma_key_matte.frag.hlsl ps_6_0
        mediaserver_chroma_key_matte_frag_dxil
        chroma_key_matte_frag_dxil.h)
    mediaserver_compile_dxil(
        luma_key_matte.frag.hlsl ps_6_0
        mediaserver_luma_key_matte_frag_dxil luma_key_matte_frag_dxil.h)
    mediaserver_compile_dxil(
        key_morphology.frag.hlsl ps_6_0
        mediaserver_key_morphology_frag_dxil key_morphology_frag_dxil.h)
    mediaserver_compile_dxil(
        chroma_key_compose.frag.hlsl ps_6_0
        mediaserver_chroma_key_compose_frag_dxil
        chroma_key_compose_frag_dxil.h)
    mediaserver_compile_dxil(
        luma_key_compose.frag.hlsl ps_6_0
        mediaserver_luma_key_compose_frag_dxil luma_key_compose_frag_dxil.h)
    # First-class layer masks rasterize one deterministic ordered coverage
    # operation per pass, then gate premultiplied colour in a final pass.
    mediaserver_compile_dxil(
        mask_raster.frag.hlsl ps_6_0
        mediaserver_mask_raster_frag_dxil mask_raster_frag_dxil.h)
    mediaserver_compile_dxil(
        mask_compose.frag.hlsl ps_6_0
        mediaserver_mask_compose_frag_dxil mask_compose_frag_dxil.h)
    # Binary Matte consumes two independently rendered image branches. It
    # changes source coverage while retaining source depth in the executor.
    mediaserver_compile_dxil(
        matte.frag.hlsl ps_6_0
        mediaserver_matte_frag_dxil matte_frag_dxil.h)
    mediaserver_compile_dxil(
        compositor_layer.frag.hlsl ps_6_0
        mediaserver_compositor_layer_frag_dxil compositor_layer_frag_dxil.h)
    # The bespoke 2D UI: pulled quads, one atlas for text and solid fills, clip
    # rect carried per batch. The operator UI is built on this.
    mediaserver_compile_dxil(
        ui.vert.hlsl vs_6_0 mediaserver_ui_vert_dxil ui_vert_dxil.h)
    mediaserver_compile_dxil(
        ui.frag.hlsl ps_6_0 mediaserver_ui_frag_dxil ui_frag_dxil.h)

    add_custom_target(mediaserver_shader_bytecode
        DEPENDS ${_mediaserver_shader_headers}
    )
    set_target_properties(mediaserver_shader_bytecode
        PROPERTIES FOLDER "engine"
    )
    add_dependencies(mediaserver_shaders mediaserver_shader_bytecode)
    target_compile_definitions(mediaserver_shaders
        INTERFACE MEDIASERVER_HAS_DXIL_SHADERS=1
    )
    message(STATUS "Shader bytecode: DXIL via ${MEDIASERVER_DXC}")
else()
    message(STATUS
        "Shader bytecode: none (no dxc found); the GPU output path will be "
        "unavailable and playback will use the CPU presenter"
    )
endif()
