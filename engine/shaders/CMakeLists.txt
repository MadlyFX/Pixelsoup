# Shader bytecode for the GPU output path.
#
# Each shader is authored once in HLSL. DXIL is always emitted when DXC is
# present; SPIR-V is emitted from the same source when that exact compiler
# proves its SPIR-V backend with a configure-time compile. The generated build
# manifest records the compiler and flags used by this build.
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

# Advanced nodes must run byte-identical shaders, so Windows builds use one
# reviewed DirectXShaderCompiler archive by default. Acquisition happens only
# while configuring a build; no compiler or network access exists at runtime.
option(
    MEDIASERVER_USE_PINNED_DXC
    "Acquire and use the reviewed DirectXShaderCompiler artifact"
    ${MEDIASERVER_ENABLE_ADVANCED_RENDERER}
)
set(MEDIASERVER_DXC_RELEASE "v1.9.2607")
set(MEDIASERVER_DXC_ARCHIVE "dxc_2026_07_29.zip")
set(MEDIASERVER_DXC_ARCHIVE_SHA256
    "a1dfb116ba3eeae6a1582291b53a8e7bf65ad760676bd3194685c8f7367cd241"
)
set(MEDIASERVER_DXC_URL
    "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${MEDIASERVER_DXC_RELEASE}/${MEDIASERVER_DXC_ARCHIVE}"
)

set(_mediaserver_dxc_hints "")
file(GLOB _mediaserver_sdk_bins
    "C:/Program Files (x86)/Windows Kits/10/bin/*/x64"
    "C:/Program Files/Windows Kits/10/bin/*/x64"
)
list(SORT _mediaserver_sdk_bins)
list(REVERSE _mediaserver_sdk_bins)
list(APPEND _mediaserver_dxc_hints ${_mediaserver_sdk_bins})

if(WIN32 AND MEDIASERVER_USE_PINNED_DXC)
    set(_mediaserver_dxc_root
        "${CMAKE_BINARY_DIR}/_tools/dxc-${MEDIASERVER_DXC_RELEASE}")
    set(_mediaserver_dxc_archive
        "${_mediaserver_dxc_root}/${MEDIASERVER_DXC_ARCHIVE}")
    set(_mediaserver_dxc_extract "${_mediaserver_dxc_root}/extracted")
    set(_mediaserver_pinned_dxc
        "${_mediaserver_dxc_extract}/bin/x64/dxc.exe")
    if(NOT EXISTS "${_mediaserver_pinned_dxc}")
        file(MAKE_DIRECTORY "${_mediaserver_dxc_root}")
        if(NOT EXISTS "${_mediaserver_dxc_archive}")
            message(STATUS
                "Acquiring pinned DXC ${MEDIASERVER_DXC_RELEASE}")
            file(DOWNLOAD
                "${MEDIASERVER_DXC_URL}"
                "${_mediaserver_dxc_archive}"
                EXPECTED_HASH
                    "SHA256=${MEDIASERVER_DXC_ARCHIVE_SHA256}"
                TLS_VERIFY ON
                STATUS _mediaserver_dxc_download_status
            )
            list(GET _mediaserver_dxc_download_status 0
                 _mediaserver_dxc_download_code)
            list(GET _mediaserver_dxc_download_status 1
                 _mediaserver_dxc_download_message)
            if(NOT _mediaserver_dxc_download_code EQUAL 0)
                message(FATAL_ERROR
                    "Pinned DXC download failed: "
                    "${_mediaserver_dxc_download_message}")
            endif()
        else()
            file(SHA256 "${_mediaserver_dxc_archive}"
                 _mediaserver_dxc_archive_actual)
            if(NOT _mediaserver_dxc_archive_actual STREQUAL
                   MEDIASERVER_DXC_ARCHIVE_SHA256)
                message(FATAL_ERROR
                    "Cached DXC archive hash does not match the reviewed pin")
            endif()
        endif()
        file(MAKE_DIRECTORY "${_mediaserver_dxc_extract}")
        file(ARCHIVE_EXTRACT
            INPUT "${_mediaserver_dxc_archive}"
            DESTINATION "${_mediaserver_dxc_extract}"
        )
    endif()
    set(MEDIASERVER_DXC "${_mediaserver_pinned_dxc}")
else()
    find_program(MEDIASERVER_DXC
        NAMES dxc dxc.exe
        HINTS ${_mediaserver_dxc_hints}
        DOC "DirectX shader compiler used to produce DXIL and SPIR-V"
    )
endif()

set(_mediaserver_shader_headers "")
# Item 22: the production contract applies to every shader, not an audited
# subset of expressions. SM 6.2 is required for explicit FP32 denorm semantics.
# OFF is an experimental A/B build; the bundle and conformance report carry
# that choice and it cannot mix with a strict cohort.
option(MEDIASERVER_SHADER_STRICT_FLOAT "Use IEEE strict arithmetic in every shader" ON)
set(_mediaserver_float_flags -denorm preserve)
if(MEDIASERVER_SHADER_STRICT_FLOAT)
    list(APPEND _mediaserver_float_flags -Gis)
endif()
set(_mediaserver_dxil_flags -WX ${_mediaserver_float_flags})
set(_mediaserver_spirv_flags
    ${_mediaserver_float_flags}
    -WX
    -spirv
    -fspv-target-env=vulkan1.2
    -fvk-use-dx-layout
    -fspv-reflect
)

set(MEDIASERVER_DXC_SUPPORTS_SPIRV OFF)
if(MEDIASERVER_DXC)
    set(_mediaserver_spirv_probe
        "${CMAKE_CURRENT_BINARY_DIR}/mediaserver_spirv_probe.spv")
    execute_process(
        COMMAND "${MEDIASERVER_DXC}"
                ${_mediaserver_spirv_flags}
                -T vs_6_2
                -E main
                -Fo "${_mediaserver_spirv_probe}"
                "${CMAKE_CURRENT_SOURCE_DIR}/output.vert.hlsl"
        RESULT_VARIABLE _mediaserver_spirv_probe_result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    file(REMOVE "${_mediaserver_spirv_probe}")
    if(_mediaserver_spirv_probe_result EQUAL 0)
        set(MEDIASERVER_DXC_SUPPORTS_SPIRV ON)
    endif()

    execute_process(
        COMMAND "${MEDIASERVER_DXC}" --version
        RESULT_VARIABLE _mediaserver_dxc_version_result
        OUTPUT_VARIABLE _mediaserver_dxc_version
        ERROR_VARIABLE _mediaserver_dxc_version_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _mediaserver_dxc_version_result EQUAL 0)
        set(_mediaserver_dxc_version "${_mediaserver_dxc_version_error}")
    endif()
    string(REPLACE "\r" "" _mediaserver_dxc_version
           "${_mediaserver_dxc_version}")
    string(REPLACE "\n" " | " _mediaserver_dxc_version
           "${_mediaserver_dxc_version}")
    string(JOIN " " _mediaserver_dxil_flags_text
           ${_mediaserver_dxil_flags})
    string(JOIN " " _mediaserver_spirv_flags_text
           ${_mediaserver_spirv_flags})
    file(GENERATE
        OUTPUT "${MEDIASERVER_SHADER_OUTPUT_DIR}/build_manifest.txt"
        CONTENT
"compiler=${MEDIASERVER_DXC}
version=${_mediaserver_dxc_version}
release=${MEDIASERVER_DXC_RELEASE}
archive_sha256=${MEDIASERVER_DXC_ARCHIVE_SHA256}
dxil_flags=${_mediaserver_dxil_flags_text}
spirv_enabled=${MEDIASERVER_DXC_SUPPORTS_SPIRV}
spirv_flags=${_mediaserver_spirv_flags_text}
entry_point=main
")
endif()

if(MEDIASERVER_ENABLE_ADVANCED_RENDERER AND
   NOT MEDIASERVER_DXC_SUPPORTS_SPIRV)
    message(FATAL_ERROR
        "The Advanced renderer requires the reviewed DXC to emit both DXIL "
        "and SPIR-V; disable MEDIASERVER_ENABLE_ADVANCED_RENDERER only for "
        "a Basic-only build")
endif()

# stage: vs_6_2 or ps_6_2. Emits a C header with a named byte array, which
# keeps the bytecode inside the executable and out of the runtime file layout.
file(GLOB _mediaserver_shader_dependencies CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hlsli")
function(mediaserver_compile_dxil source stage symbol output_name)
    set(_output "${MEDIASERVER_SHADER_OUTPUT_DIR}/${output_name}")
    string(REPLACE ".h" ".reflection" _reflection_output_name
           "${output_name}")
    set(_reflection_output
        "${MEDIASERVER_SHADER_OUTPUT_DIR}/${_reflection_output_name}")
    set(_binary "${MEDIASERVER_SHADER_OUTPUT_DIR}/${source}.dxil")
    add_custom_command(
        OUTPUT "${_output}" "${_reflection_output}" "${_binary}"
        COMMAND "${MEDIASERVER_DXC}"
                -T ${stage}
                -E main
                -Fo "${_binary}"
                -Fh "${_output}"
                -Vn ${symbol}
                -Fre "${_reflection_output}"
                ${_mediaserver_dxil_flags}
                "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
                ${_mediaserver_shader_dependencies}
        COMMENT "Compiling ${source} to DXIL"
        VERBATIM
    )
    set(_mediaserver_shader_headers
        ${_mediaserver_shader_headers} "${_output}" "${_reflection_output}"
    )
    if(MEDIASERVER_DXC_SUPPORTS_SPIRV)
        string(REPLACE "_dxil" "_spirv" _spirv_symbol "${symbol}")
        string(REPLACE "_dxil.h" "_spirv.h" _spirv_output_name
               "${output_name}")
        set(_spirv_output
            "${MEDIASERVER_SHADER_OUTPUT_DIR}/${_spirv_output_name}")
        set(_spirv_binary "${MEDIASERVER_SHADER_OUTPUT_DIR}/${source}.spv")
        add_custom_command(
            OUTPUT "${_spirv_output}" "${_spirv_binary}"
            COMMAND "${MEDIASERVER_DXC}"
                    ${_mediaserver_spirv_flags}
                    -T ${stage}
                    -E main
                    -Fo "${_spirv_binary}"
                    -Fh "${_spirv_output}"
                    -Vn ${_spirv_symbol}
                    "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
                    ${_mediaserver_shader_dependencies}
            COMMENT "Compiling ${source} to SPIR-V"
            VERBATIM
        )
        list(APPEND _mediaserver_shader_headers "${_spirv_output}")
    endif()
    # Test probes are intentionally excluded from deployment identity.
    if(NOT source MATCHES "(probe|compute_fill|compute_gather|solid)\\.")
        set_property(GLOBAL APPEND PROPERTY MEDIASERVER_SHADER_BINARIES "${_binary}")
        if(MEDIASERVER_DXC_SUPPORTS_SPIRV)
            set_property(GLOBAL APPEND PROPERTY MEDIASERVER_SHADER_BINARIES "${_spirv_binary}")
        endif()
    endif()
    set(_mediaserver_shader_headers ${_mediaserver_shader_headers}
        PARENT_SCOPE)
endfunction()

if(MEDIASERVER_DXC)
    # The 2D output path: one instanced quad per rectangle or glyph.
    mediaserver_compile_dxil(
        quad.vert.hlsl vs_6_2 mediaserver_quad_vert_dxil quad_vert_dxil.h)
    mediaserver_compile_dxil(
        quad.frag.hlsl ps_6_2 mediaserver_quad_frag_dxil quad_frag_dxil.h)
    # The 3D path: fully pulled geometry, depth tested, metallic-roughness.
    mediaserver_compile_dxil(
        scene.vert.hlsl vs_6_2 mediaserver_scene_vert_dxil scene_vert_dxil.h)
    mediaserver_compile_dxil(
        scene_motion.vert.hlsl vs_6_2 mediaserver_scene_motion_vert_dxil scene_motion_vert_dxil.h)
    mediaserver_compile_dxil(
        scene.frag.hlsl ps_6_2 mediaserver_scene_frag_dxil scene_frag_dxil.h)
    mediaserver_compile_dxil(
        scene_linear.frag.hlsl ps_6_2
        mediaserver_scene_linear_frag_dxil scene_linear_frag_dxil.h)
    mediaserver_compile_dxil(
        scene_shadow.frag.hlsl ps_6_2
        mediaserver_scene_shadow_frag_dxil scene_shadow_frag_dxil.h)
    if(BUILD_TESTING)
        mediaserver_compile_dxil(
            shadow_probe.frag.hlsl ps_6_2
            mediaserver_shadow_probe_frag_dxil shadow_probe_frag_dxil.h)
    endif()
    mediaserver_compile_dxil(
        scene_rsm_normal.frag.hlsl ps_6_2
        mediaserver_scene_rsm_normal_frag_dxil scene_rsm_normal_frag_dxil.h)
    mediaserver_compile_dxil(
        scene_rsm_flux.frag.hlsl ps_6_2
        mediaserver_scene_rsm_flux_frag_dxil scene_rsm_flux_frag_dxil.h)
    mediaserver_compile_dxil(
        scene_object_id.frag.hlsl ps_6_2
        mediaserver_scene_object_id_frag_dxil scene_object_id_frag_dxil.h)
    mediaserver_compile_dxil(
        scene_velocity.frag.hlsl ps_6_2
        mediaserver_scene_velocity_frag_dxil scene_velocity_frag_dxil.h)
    # World normals, for occlusion now and screen-space reflections later.
    mediaserver_compile_dxil(
        scene_normal.frag.hlsl ps_6_2
        mediaserver_scene_normal_frag_dxil scene_normal_frag_dxil.h)
    # Identity, motion and normals in one multiple-target pass where the
    # device can, with the same coverage and equations as the three above.
    mediaserver_compile_dxil(
        scene_auxiliary.frag.hlsl ps_6_2
        mediaserver_scene_auxiliary_frag_dxil scene_auxiliary_frag_dxil.h)
    mediaserver_compile_dxil(
        scene_media_auxiliary.frag.hlsl ps_6_2
        mediaserver_scene_media_auxiliary_frag_dxil
        scene_media_auxiliary_frag_dxil.h)
    mediaserver_compile_dxil(
        scene_coverage.frag.hlsl ps_6_2
        mediaserver_scene_coverage_frag_dxil scene_coverage_frag_dxil.h)
    # The media clip pass reuses the pulled scene vertex stage and samples the
    # clip texture in its own fragment stage.
    mediaserver_compile_dxil(
        scene_media.frag.hlsl ps_6_2
        mediaserver_scene_media_frag_dxil scene_media_frag_dxil.h)
    mediaserver_compile_dxil(
        scene_media_linear.frag.hlsl ps_6_2
        mediaserver_scene_media_linear_frag_dxil
        scene_media_linear_frag_dxil.h)
    # Auxiliary media passes sample the same alpha/edge coverage as colour so
    # transparent padding cannot claim object identity, motion, or depth.
    mediaserver_compile_dxil(
        scene_media_object_id.frag.hlsl ps_6_2
        mediaserver_scene_media_object_id_frag_dxil
        scene_media_object_id_frag_dxil.h)
    mediaserver_compile_dxil(
        scene_media_velocity.frag.hlsl ps_6_2
        mediaserver_scene_media_velocity_frag_dxil
        scene_media_velocity_frag_dxil.h)
    mediaserver_compile_dxil(
        scene_media_normal.frag.hlsl ps_6_2
        mediaserver_scene_media_normal_frag_dxil
        scene_media_normal_frag_dxil.h)
    # The output stage: a fullscreen triangle that samples a source texture and
    # applies the feed-map crop and soft-edge blend.
    mediaserver_compile_dxil(texture_probe.frag.hlsl ps_6_2 mediaserver_texture_probe_frag_dxil texture_probe_frag_dxil.h)
    mediaserver_compile_dxil(
        output.vert.hlsl vs_6_2 mediaserver_output_vert_dxil output_vert_dxil.h)
    mediaserver_compile_dxil(
        output.frag.hlsl ps_6_2 mediaserver_output_frag_dxil output_frag_dxil.h)
    mediaserver_compile_dxil(
        output_linear.frag.hlsl ps_6_2
        mediaserver_output_linear_frag_dxil output_linear_frag_dxil.h)
    # Arbitrary LED surfaces: one asymmetric scene render, then physical mesh
    # XYZ projected into it while UV0 places triangles in connector space.
    mediaserver_compile_dxil(
        projection_mesh_warp.vert.hlsl vs_6_2
        mediaserver_projection_mesh_warp_vert_dxil
        projection_mesh_warp_vert_dxil.h)
    mediaserver_compile_dxil(
        projection_mesh_warp.frag.hlsl ps_6_2
        mediaserver_projection_mesh_warp_frag_dxil
        projection_mesh_warp_frag_dxil.h)
    mediaserver_compile_dxil(
        scene_mix.frag.hlsl ps_6_2
        mediaserver_scene_mix_frag_dxil scene_mix_frag_dxil.h)
    # Inner frustum over outer frustum. Both are complete renders of the same
    # scene from two viewpoints; this puts the tracked one over the authored
    # one with a feather evaluated in absolute canvas pixels.
    mediaserver_compile_dxil(
        icvfx_composite.frag.hlsl ps_6_2
        mediaserver_icvfx_composite_frag_dxil icvfx_composite_frag_dxil.h)
    # A two-pass separable Gaussian blur reuses the output fullscreen vertex
    # stage and selects its axis through one small fragment constant block.
    mediaserver_compile_dxil(
        gaussian_blur.frag.hlsl ps_6_2
        mediaserver_gaussian_blur_frag_dxil gaussian_blur_frag_dxil.h)
    # Box and directional blur are distinct spatial processors. They share
    # neither Gaussian weights nor Gaussian shader bytecode.
    mediaserver_compile_dxil(
        box_blur.frag.hlsl ps_6_2
        mediaserver_box_blur_frag_dxil box_blur_frag_dxil.h)
    mediaserver_compile_dxil(
        directional_blur.frag.hlsl ps_6_2
        mediaserver_directional_blur_frag_dxil directional_blur_frag_dxil.h)
    # Keyers calculate a matte first, optionally reshape it, then compose the
    # untouched premultiplied source against that matte. Keeping these stages
    # distinct makes spill and source-alpha coverage ordering explicit.
    mediaserver_compile_dxil(
        chroma_key_matte.frag.hlsl ps_6_2
        mediaserver_chroma_key_matte_frag_dxil
        chroma_key_matte_frag_dxil.h)
    mediaserver_compile_dxil(
        luma_key_matte.frag.hlsl ps_6_2
        mediaserver_luma_key_matte_frag_dxil luma_key_matte_frag_dxil.h)
    mediaserver_compile_dxil(
        key_morphology.frag.hlsl ps_6_2
        mediaserver_key_morphology_frag_dxil key_morphology_frag_dxil.h)
    mediaserver_compile_dxil(
        chroma_key_compose.frag.hlsl ps_6_2
        mediaserver_chroma_key_compose_frag_dxil
        chroma_key_compose_frag_dxil.h)
    mediaserver_compile_dxil(
        luma_key_compose.frag.hlsl ps_6_2
        mediaserver_luma_key_compose_frag_dxil luma_key_compose_frag_dxil.h)
    # First-class layer masks rasterize one deterministic ordered coverage
    # operation per pass, then gate premultiplied colour in a final pass.
    mediaserver_compile_dxil(
        mask_raster.frag.hlsl ps_6_2
        mediaserver_mask_raster_frag_dxil mask_raster_frag_dxil.h)
    mediaserver_compile_dxil(
        mask_compose.frag.hlsl ps_6_2
        mediaserver_mask_compose_frag_dxil mask_compose_frag_dxil.h)
    # Binary Matte consumes two independently rendered image branches. It
    # changes source coverage while retaining source depth in the executor.
    mediaserver_compile_dxil(
        matte.frag.hlsl ps_6_2
        mediaserver_matte_frag_dxil matte_frag_dxil.h)
    mediaserver_compile_dxil(
        compositor_layer.frag.hlsl ps_6_2
        mediaserver_compositor_layer_frag_dxil compositor_layer_frag_dxil.h)
    # A fullscreen triangle in one known colour. Not used by any renderer: it
    # is the minimal draw the RHI tests exercise the pipeline and viewport with,
    # kept here so it is built by the same pinned compiler and the same rules as
    # everything else rather than being a second shader path in a test.
    mediaserver_compile_dxil(
        solid.vert.hlsl vs_6_2 mediaserver_solid_vert_dxil solid_vert_dxil.h)
    mediaserver_compile_dxil(
        solid.frag.hlsl ps_6_2 mediaserver_solid_frag_dxil solid_frag_dxil.h)
    # The environment as the background. Shares the fullscreen vertex stage
    # below rather than carrying its own.
    mediaserver_compile_dxil(
        sky.frag.hlsl ps_6_2 mediaserver_sky_frag_dxil sky_frag_dxil.h)
    # Temporal antialiasing. Shares the fullscreen vertex stage; runs only for
    # an Advance, because every other disposition is a copy or nothing.
    mediaserver_compile_dxil(
        taa_resolve.frag.hlsl ps_6_2
        mediaserver_taa_resolve_frag_dxil taa_resolve_frag_dxil.h)
    # The spatial alternative to it. Stateless, so it cannot ghost - which is
    # the whole reason both exist.
    mediaserver_compile_dxil(
        spatial_antialias.frag.hlsl ps_6_2
        mediaserver_spatial_antialias_frag_dxil spatial_antialias_frag_dxil.h)
    mediaserver_compile_dxil(
        object_motion_dilate.frag.hlsl ps_6_2
        mediaserver_object_motion_dilate_frag_dxil object_motion_dilate_frag_dxil.h)
    mediaserver_compile_dxil(
        plate_dof.frag.hlsl ps_6_2
        mediaserver_plate_dof_frag_dxil plate_dof_frag_dxil.h)
    mediaserver_compile_dxil(
        object_motion_blur.frag.hlsl ps_6_2
        mediaserver_object_motion_blur_frag_dxil object_motion_blur_frag_dxil.h)
    mediaserver_compile_dxil(
        object_motion_merge.comp.hlsl cs_6_2
        mediaserver_object_motion_merge_comp_dxil object_motion_merge_comp_dxil.h)
    # Image-based lighting. Every one of these runs once per environment
    # generation at GPU adoption, with held frames reusing the products. Each is
    # written to be bit-reproducible across a cohort - fixed sample counts,
    # fixed sequences, and a reduction whose addition order does not depend on
    # scheduling.
    mediaserver_compile_dxil(
        physical_sky.comp.hlsl cs_6_2 mediaserver_physical_sky_comp_dxil physical_sky_comp_dxil.h)
    mediaserver_compile_dxil(
        ibl_equirect_to_cube.comp.hlsl cs_6_2
        mediaserver_ibl_equirect_to_cube_comp_dxil
        ibl_equirect_to_cube_comp_dxil.h)
    mediaserver_compile_dxil(
        ibl_irradiance_sh.comp.hlsl cs_6_2
        mediaserver_ibl_irradiance_sh_comp_dxil
        ibl_irradiance_sh_comp_dxil.h)
    mediaserver_compile_dxil(
        ibl_specular_prefilter.comp.hlsl cs_6_2
        mediaserver_ibl_specular_prefilter_comp_dxil
        ibl_specular_prefilter_comp_dxil.h)
    mediaserver_compile_dxil(
        ibl_brdf_lut.comp.hlsl cs_6_2
        mediaserver_ibl_brdf_lut_comp_dxil ibl_brdf_lut_comp_dxil.h)
    # Compute. The same pinned compiler, the same warnings-as-errors, and the
    # same reflection output as every raster stage - a compute entry point is
    # not a second class of shader with its own rules. cs_6_2 shares the same
    # Shader Model 6.2 floor and explicit FP32 denorm preservation as raster.
    # Ambient occlusion. Compute, because the horizon search reads a
    # neighbourhood per pixel and a fullscreen draw would re-fetch it per
    # fragment.
    mediaserver_compile_dxil(
        ambient_occlusion.comp.hlsl cs_6_2
        mediaserver_ambient_occlusion_comp_dxil
        ambient_occlusion_comp_dxil.h)
    # Bloom. Two stages of the same chain, kept as separate entry points
    # because a branch between them would be a branch taken per texel of every
    # level rather than once per dispatch.
    mediaserver_compile_dxil(
        bloom_downsample.comp.hlsl cs_6_2
        mediaserver_bloom_downsample_comp_dxil
        bloom_downsample_comp_dxil.h)
    mediaserver_compile_dxil(
        bloom_upsample.comp.hlsl cs_6_2
        mediaserver_bloom_upsample_comp_dxil
        bloom_upsample_comp_dxil.h)
    mediaserver_compile_dxil(
        bloom_combine.comp.hlsl cs_6_2
        mediaserver_bloom_combine_comp_dxil
        bloom_combine_comp_dxil.h)
    # Volumetric fog. Three stages of one volume: scatter per froxel, integrate
    # front to back along each ray, then resolve at each pixel's own depth.
    mediaserver_compile_dxil(
        fog_scatter.comp.hlsl cs_6_2
        mediaserver_fog_scatter_comp_dxil fog_scatter_comp_dxil.h)
    mediaserver_compile_dxil(
        fog_integrate.comp.hlsl cs_6_2
        mediaserver_fog_integrate_comp_dxil fog_integrate_comp_dxil.h)
    mediaserver_compile_dxil(
        fog_apply.comp.hlsl cs_6_2
        mediaserver_fog_apply_comp_dxil fog_apply_comp_dxil.h)
    # The nearest surface across compositor layers, so atmosphere and bloom
    # can be applied once to the finished composite rather than inside each
    # layer's render.
    mediaserver_compile_dxil(
        depth_merge.comp.hlsl cs_6_2
        mediaserver_depth_merge_comp_dxil depth_merge_comp_dxil.h)
    # Half-resolution video. One plane of a decoded frame into a surface of
    # half the size, on the decode worker rather than in a render pass.
    mediaserver_compile_dxil(
        video_halve.comp.hlsl cs_6_2
        mediaserver_video_halve_comp_dxil video_halve_comp_dxil.h)
    mediaserver_compile_dxil(
        compute_fill.comp.hlsl cs_6_2
        mediaserver_compute_fill_comp_dxil compute_fill_comp_dxil.h)
    mediaserver_compile_dxil(
        compute_gather.comp.hlsl cs_6_2
        mediaserver_compute_gather_comp_dxil compute_gather_comp_dxil.h)
    # The bespoke 2D UI: pulled quads, one atlas for text and solid fills, clip
    # rect carried per batch. The operator UI is built on this.
    mediaserver_compile_dxil(
        ui.vert.hlsl vs_6_2 mediaserver_ui_vert_dxil ui_vert_dxil.h)
    mediaserver_compile_dxil(
        ui.frag.hlsl ps_6_2 mediaserver_ui_frag_dxil ui_frag_dxil.h)

    get_property(_bundle_binaries GLOBAL PROPERTY MEDIASERVER_SHADER_BINARIES)
    list(SORT _bundle_binaries)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bundle-inputs.cmake"
        CONTENT "set(BUNDLE_INPUTS \"${_bundle_binaries}\")\n")
    set(_bundle_header "${MEDIASERVER_SHADER_OUTPUT_DIR}/bundle_identity.hpp")
    add_custom_command(OUTPUT "${_bundle_header}"
        COMMAND "${CMAKE_COMMAND}"
            "-DINPUTS=${CMAKE_CURRENT_BINARY_DIR}/bundle-inputs.cmake"
            "-DOUTPUT=${_bundle_header}"
            "-DSTRICT=${MEDIASERVER_SHADER_STRICT_FLOAT}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/bundle_identity.cmake"
        DEPENDS ${_bundle_binaries} "${CMAKE_CURRENT_BINARY_DIR}/bundle-inputs.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/bundle_identity.cmake"
        VERBATIM)
    add_custom_target(mediaserver_shader_bytecode
        DEPENDS ${_mediaserver_shader_headers} "${_bundle_header}"
    )
    set_target_properties(mediaserver_shader_bytecode
        PROPERTIES FOLDER "engine"
    )
    add_dependencies(mediaserver_shaders mediaserver_shader_bytecode)
    target_compile_definitions(mediaserver_shaders
        INTERFACE MEDIASERVER_HAS_DXIL_SHADERS=1
    )
    if(MEDIASERVER_DXC_SUPPORTS_SPIRV)
        target_compile_definitions(mediaserver_shaders
            INTERFACE MEDIASERVER_HAS_SPIRV_SHADERS=1
        )
        message(STATUS
            "Shader bytecode: DXIL and SPIR-V via ${MEDIASERVER_DXC}")
    else()
        message(STATUS
            "Shader bytecode: DXIL via ${MEDIASERVER_DXC}; this DXC did not "
            "pass the SPIR-V compile probe")
    endif()
else()
    message(STATUS
        "Shader bytecode: none (no dxc found); the GPU output path will be "
        "unavailable and playback will use the CPU presenter"
    )
endif()
