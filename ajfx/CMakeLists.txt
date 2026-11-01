cmake_minimum_required(VERSION 3.21)

# ajfx: deterministic, seekable GPU particles and volumes, and ajfx Studio,
# the block editor that authors them.
#
# Built two ways: standalone (this file is the top-level project; see
# CMakePresets.json) or as a subdirectory of a host such as the media server
# beside it in this repository. Nothing here may name a host option; a host
# sets the AJFX_* options below before adding this directory.
project(
    ajfx
    VERSION 0.1.0
    DESCRIPTION "Deterministic, seekable GPU particles and volumes"
    LANGUAGES CXX
)

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif()

option(AJFX_WARNINGS_AS_ERRORS "Treat ajfx warnings as errors" OFF)
# The Studio application previews through the media server's renderer and
# draws with its UI toolkit, so only a host that provides MediaServer::Render,
# MediaServer::Gui and MediaServer::Editor turns it on. Studio's page model,
# compiler and tests build everywhere.
option(AJFX_BUILD_STUDIO_APP "Build the ajfx-studio application (needs the media server renderer)" OFF)

# Warnings plus the floating-point settings determinism depends on: no
# contraction into fused multiply-adds and no reassociation, so the CPU
# reference produces the same bits on every compiler and node.
function(ajfx_set_options target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /Zc:__cplusplus /fp:precise /fp:except-)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /Zc:preprocessor)
        endif()
        if(AJFX_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
            -ffp-contract=off -fno-fast-math)
        if(AJFX_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_subdirectory(lib)
add_subdirectory(studio)
