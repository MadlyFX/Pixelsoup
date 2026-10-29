cmake_minimum_required(VERSION 3.21)

# ajfx: deterministic, seekable GPU particles and volumes.
#
# Built two ways: standalone (this file is the top-level project, the shape it
# will have once it moves to its own repository) or as a subdirectory of the
# media server. Nothing here may name a media server target, header or option;
# the host adapts to ajfx, never the reverse.
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
endif()

# A host's warnings policy is inherited when it has one, so the library is
# held to the same bar in-tree as the code around it.
if(DEFINED MEDIASERVER_WARNINGS_AS_ERRORS)
    set(_ajfx_werror_default ${MEDIASERVER_WARNINGS_AS_ERRORS})
else()
    set(_ajfx_werror_default OFF)
endif()
option(AJFX_WARNINGS_AS_ERRORS "Treat ajfx warnings as errors" ${_ajfx_werror_default})

# Warnings plus the floating-point settings determinism depends on: no
# contraction into fused multiply-adds and no reassociation, so the CPU
# reference produces the same bits on every compiler and node.
function(ajfx_set_options target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /Zc:__cplusplus /fp:precise /fp:except-)
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

add_library(ajfx STATIC
    src/builder.cpp
    src/effect.cpp
    src/json.cpp
    src/package.cpp
    src/presets.cpp
    src/simulation.cpp
    src/version.cpp
)
add_library(ajfx::ajfx ALIAS ajfx)

target_include_directories(ajfx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
ajfx_set_options(ajfx)
set_target_properties(ajfx PROPERTIES FOLDER "libs")

# HLSL that a host compiles into its own kernels; exposed as a directory so
# the host's shader compiler can add it to its include path.
set(AJFX_SHADER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    CACHE INTERNAL "ajfx HLSL include directory")

if(BUILD_TESTING)
    add_executable(ajfx_random_tests tests/random_tests.cpp)
    target_link_libraries(ajfx_random_tests PRIVATE ajfx::ajfx)
    ajfx_set_options(ajfx_random_tests)
    # The test reads the HLSL twin of random.hpp to prove the two agree.
    target_compile_definitions(ajfx_random_tests PRIVATE
        AJFX_SHADER_INCLUDE_DIR="${AJFX_SHADER_INCLUDE_DIR}")
    add_test(NAME ajfx.random COMMAND ajfx_random_tests)
    set_target_properties(ajfx_random_tests PROPERTIES FOLDER "tests")

    foreach(suite IN ITEMS effect package simulation)
        add_executable(ajfx_${suite}_tests tests/${suite}_tests.cpp)
        target_link_libraries(ajfx_${suite}_tests PRIVATE ajfx::ajfx)
        ajfx_set_options(ajfx_${suite}_tests)
        add_test(NAME ajfx.${suite} COMMAND ajfx_${suite}_tests)
        set_target_properties(ajfx_${suite}_tests PROPERTIES FOLDER "tests")
    endforeach()
endif()
