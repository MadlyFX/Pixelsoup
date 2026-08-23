add_library(mediaserver_core INTERFACE)
add_library(MediaServer::Core ALIAS mediaserver_core)

target_include_directories(mediaserver_core
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(mediaserver_core INTERFACE cxx_std_20)

