cmake_minimum_required(VERSION 3.24)
project(simple-engine-2 VERSION 1.0 LANGUAGES CXX)

if(POLICY CMP0167)
  # CMake 4 warns about FindBoost deprecation/removal; keep using FindBoost for now.
  cmake_policy(SET CMP0167 OLD)
endif()
if(POLICY CMP0144)
  # Make find_package honor <PackageName>_ROOT variables (e.g. Boost_ROOT).
  cmake_policy(SET CMP0144 NEW)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable compile_commands.json for VSCode IntelliSense
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(DEP_DIR "${CMAKE_SOURCE_DIR}/dep")

# ------------------------------
# Dependencies (vendored in dep/)
# ------------------------------
# SFML 3 (build from source)
set(SFML_BUILD_EXAMPLES OFF CACHE BOOL "Build SFML examples" FORCE)
set(SFML_BUILD_TEST_SUITE OFF CACHE BOOL "Build SFML tests" FORCE)
set(SFML_BUILD_DOC OFF CACHE BOOL "Build SFML docs" FORCE)
add_subdirectory("${DEP_DIR}/SFML-3.0.2" "${CMAKE_BINARY_DIR}/_deps/sfml")

# ImGui + ImGui-SFML (build from source)
set(IMGUI_DIR "${DEP_DIR}/imgui-1.92.5" CACHE PATH "ImGui top-level directory" FORCE)
set(IMGUI_SFML_FIND_SFML OFF CACHE BOOL "Use existing SFML targets" FORCE)
set(IMGUI_SFML_BUILD_EXAMPLES OFF CACHE BOOL "Build ImGui-SFML examples" FORCE)
set(IMGUI_SFML_BUILD_TESTING OFF CACHE BOOL "Build ImGui-SFML tests" FORCE)
add_subdirectory("${DEP_DIR}/imgui-sfml-3.0" "${CMAKE_BINARY_DIR}/_deps/imgui-sfml")

# Boost (expects a built/installed Boost with compiled libraries)
set(Boost_NO_WARN_NEW_VERSIONS ON)
set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)

# Prefer headers from the vendored Boost tree. Compiled libraries still need to
# be available to the linker (either built separately from this tree, or installed).
set(Boost_ROOT "${DEP_DIR}/boost_1_90_0" CACHE PATH "Boost root directory" FORCE)
set(BOOST_INCLUDEDIR "${DEP_DIR}/boost_1_90_0" CACHE PATH "Boost include directory" FORCE)
set(BOOST_LIBRARYDIR "${DEP_DIR}/boost_1_90_0/stage/lib" CACHE PATH "Boost library directory (after building Boost)" FORCE)

find_package(Boost 1.90.0 REQUIRED COMPONENTS log log_setup filesystem system thread)

# Add subdirectories for core and app
add_subdirectory(core)
add_subdirectory(app)
