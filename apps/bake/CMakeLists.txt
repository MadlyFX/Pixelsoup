add_executable(media-bake main.cpp)
target_link_libraries(media-bake PRIVATE MediaServer::Render MediaServer::Persist MediaServer::Show)
mediaserver_set_project_options(media-bake)
set_target_properties(media-bake PROPERTIES FOLDER "apps")
