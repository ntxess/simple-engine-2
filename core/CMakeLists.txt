# Create a library for the engine core
file(GLOB_RECURSE CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(core_engine STATIC ${CORE_SOURCES})

# ------------------------------
# Include directories
# ------------------------------
set(DEP_DIR "${CMAKE_SOURCE_DIR}/dep")

target_include_directories(core_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}            # core/ as include root
    ${DEP_DIR}/entt-3.16.0/src             # EnTT
    ${DEP_DIR}/rapidjson-1.1.0/include     # RapidJSON
    ${DEP_DIR}/tomlplusplus-3.4.0/include  # toml++
)

# ------------------------------
# Dependencies
# ------------------------------
target_link_libraries(core_engine PUBLIC
    Boost::log
    Boost::log_setup
    Boost::filesystem
    Boost::system
    Boost::thread
    SFML::Audio
    SFML::Graphics
    SFML::System
    SFML::Window
)
