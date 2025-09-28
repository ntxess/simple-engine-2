cmake_minimum_required(VERSION 3.16)
project(simple-engine-2 VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable compile_commands.json for VSCode IntelliSense
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add subdirectories for core, app, and tests
add_subdirectory(core)
add_subdirectory(app)
