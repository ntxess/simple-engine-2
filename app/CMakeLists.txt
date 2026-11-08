# ------------------------------
# Sources
# ------------------------------
file(GLOB_RECURSE APP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
# headless/ holds legacy_replay, which has its own main().
list(FILTER APP_SOURCES EXCLUDE REGEX "/headless/")

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

# ------------------------------
# legacy_replay: the Sandbox scene run headless (or paced with --live) from a scenario and a
# recorded .seinput, writing its semantic projection. Drives the legacy reference tests
# (tests/legacy). No ImGui: the scenes are compiled in directly, without the editor.
# ------------------------------
file(GLOB LEGACY_REPLAY_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/headless/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scene/*.cpp"
)

add_executable(legacy_replay ${LEGACY_REPLAY_SOURCES})

target_include_directories(legacy_replay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} # app/ as include root
)

target_link_libraries(legacy_replay PRIVATE
    core_engine
)
