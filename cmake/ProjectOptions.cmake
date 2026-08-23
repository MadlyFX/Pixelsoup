function(mediaserver_set_project_options target)
    target_compile_features(${target} PUBLIC cxx_std_20)

    if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
        )
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /Zc:preprocessor)
        endif()
        if(MEDIASERVER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
        )
        if(MEDIASERVER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    if(MEDIASERVER_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
endfunction()

# Floating-point settings for code whose results must match bit for bit on
# every node in a cohort.
#
# The hazard is contraction: `a * b + c` compiled to a fused multiply-add
# rounds once instead of twice, so the same source produces different bits
# depending on the target, the compiler, and the optimisation level. One node
# with FMA and one without would then disagree about where an object is, and
# the disagreement would surface as a seam between their slices.
#
# This does not make the arithmetic slower in any way that matters here - the
# scene evaluation is a few thousand matrix products per frame - and it is far
# cheaper than discovering the problem on a stage.
function(mediaserver_set_deterministic_float_options target)
    if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # /fp:precise forbids reassociation and, since VS 2022, contraction.
        # /fp:except- keeps the exception state out of the optimiser's way
        # without relaxing any of that.
        target_compile_options(${target} PRIVATE /fp:precise /fp:except-)
    else()
        target_compile_options(${target} PRIVATE
            -ffp-contract=off
            -fno-fast-math
        )
    endif()
endfunction()

function(mediaserver_disable_rtti target)
    if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /GR-)
    else()
        target_compile_options(${target} PRIVATE -fno-rtti)
    endif()
endfunction()
