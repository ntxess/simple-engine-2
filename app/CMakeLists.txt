# ------------------------------
# Sources
# ------------------------------
file(GLOB_RECURSE APP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_executable(editor_app ${APP_SOURCES})

# ------------------------------
# Include directories
# ------------------------------
# App headers
target_include_directories(editor_app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} # app/ as include root
)

# ------------------------------
# Link libraries
# ------------------------------
target_link_libraries(editor_app PRIVATE
    core_engine
    ImGui-SFML::ImGui-SFML
)
