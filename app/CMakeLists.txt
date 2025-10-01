cmake_minimum_required(VERSION 3.16)
project(editor_app LANGUAGES CXX)

cmake_policy(SET CMP0167 NEW)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ------------------------------
# Sources
# ------------------------------
file(GLOB_RECURSE APP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(editor_app ${APP_SOURCES})

# ------------------------------
# Include directories
# ------------------------------
# App headers
target_include_directories(editor_app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/editor
    ${CMAKE_CURRENT_SOURCE_DIR}/include/scene
)

# External dependencies
set(EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external")

# Header-only libraries
target_include_directories(editor_app PRIVATE
    ${EXTERNAL_DIR}/entt/single_include   # EnTT
    ${EXTERNAL_DIR}/rapidjson/include     # RapidJSON
)

# ------------------------------
# Boost.Log
# ------------------------------
set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)

find_package(Boost 1.81 REQUIRED COMPONENTS
    log
    log_setup
    filesystem
    system
    thread
)

if(NOT Boost_FOUND)
    message(FATAL_ERROR "Boost not found")
endif()

# Include Boost for core_engine
target_include_directories(core_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${Boost_INCLUDE_DIRS}
)

# ------------------------------
# SFML 3
# ------------------------------
find_package(SFML 3 REQUIRED COMPONENTS Audio Graphics System Window)

# ------------------------------
# ImGui + ImGui-SFML
# ------------------------------
set(IMGUI_DIR "${EXTERNAL_DIR}/imgui")
set(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared libraries")
set(IMGUI_SFML_FIND_SFML ON CACHE BOOL "Use find_package for SFML")

add_subdirectory(${EXTERNAL_DIR}/imgui-sfml ${CMAKE_CURRENT_BINARY_DIR}/imgui-sfml)

target_include_directories(editor_app PRIVATE
    ${IMGUI_DIR}               # ImGui headers
    ${EXTERNAL_DIR}/imgui-sfml # ImGui-SFML headers
)

# ------------------------------
# Link libraries
# ------------------------------
target_link_libraries(editor_app PRIVATE
    core_engine
    SFML::Audio
    SFML::Graphics
    SFML::System
    SFML::Window
    ImGui-SFML::ImGui-SFML
    Boost::log
    Boost::log_setup
    Boost::filesystem
    Boost::system
    Boost::thread
)
