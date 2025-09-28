# Create a library for the engine core
file(GLOB_RECURSE CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_library(core_engine STATIC ${CORE_SOURCES})

# Include directories
target_include_directories(core_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/command
    ${CMAKE_CURRENT_SOURCE_DIR}/include/component
    ${CMAKE_CURRENT_SOURCE_DIR}/include/manager
    ${CMAKE_CURRENT_SOURCE_DIR}/include/system
    ${CMAKE_CURRENT_SOURCE_DIR}/include/util
    ${CMAKE_CURRENT_SOURCE_DIR}/include/serializer
)

# Umbrella headers
target_sources(core_engine PUBLIC
    include/ApplicationContext.hpp
    include/Commands.hpp
    include/Managers.hpp
    include/Systems.hpp
    include/Components.hpp
)
