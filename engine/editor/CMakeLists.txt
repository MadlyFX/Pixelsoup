find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_library(mediaserver_editor STATIC
    src/editor_shell.cpp
)
add_library(MediaServer::Editor ALIAS mediaserver_editor)

target_include_directories(mediaserver_editor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Scene is PUBLIC because the editor's interface names scene types (the scene it
# edits, and the intents it returns). Everything below - the renderer it draws
# viewports with, the device, ImGui, and SDL - is an implementation detail no
# caller sees, so it links privately. This is the one module that names ImGui;
# keeping it here is what makes the throwaway shell deletable.
target_link_libraries(mediaserver_editor
    PUBLIC
        MediaServer::Core
        MediaServer::Scene
    PRIVATE
        MediaServer::Render
        MediaServer::Rhi
        imgui::imgui
        SDL3::SDL3
)

mediaserver_set_project_options(mediaserver_editor)
mediaserver_disable_rtti(mediaserver_editor)

set_target_properties(mediaserver_editor PROPERTIES FOLDER "engine")
