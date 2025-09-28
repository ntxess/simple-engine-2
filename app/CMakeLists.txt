# Create executable from main.cpp and all other sources
file(GLOB_RECURSE APP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(editor_app ${APP_SOURCES})

# Include directories for app headers
target_include_directories(editor_app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/editor
    ${CMAKE_CURRENT_SOURCE_DIR}/include/scene
)

# Link core_engine library
target_link_libraries(editor_app PRIVATE core_engine)

# External Dependencies (Docker preinstalled)
set(EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external")

# Thor
add_subdirectory(${EXTERNAL_DIR}/Thor ${CMAKE_CURRENT_BINARY_DIR}/Thor)

# ImGui + ImGui-SFML
target_sources(editor_app PRIVATE
    ${EXTERNAL_DIR}/imgui/imgui.cpp
    ${EXTERNAL_DIR}/imgui/imgui_draw.cpp
    ${EXTERNAL_DIR}/imgui/imgui_tables.cpp
    ${EXTERNAL_DIR}/imgui/imgui_widgets.cpp
    ${EXTERNAL_DIR}/imgui-sfml/imgui-SFML.cpp
)

target_include_directories(editor_app PRIVATE
    ${EXTERNAL_DIR}/entt/single_include
    ${EXTERNAL_DIR}/imgui
    ${EXTERNAL_DIR}/imgui-sfml
)

target_link_libraries(editor_app PRIVATE
    Thor
    sfml-graphics
    sfml-window
    sfml-system
    # OpenGL::GL
)