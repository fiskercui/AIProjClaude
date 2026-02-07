cmake_minimum_required(VERSION 3.16)
project(NaniteDemo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dependencies via FetchContent ---
include(FetchContent)

# GLFW
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.3.8
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# GLM
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        0.9.9.8
)

FetchContent_MakeAvailable(glfw glm)

# --- GLAD (OpenGL loader, bundled) ---
add_library(glad STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/glad/glad.c
)
target_include_directories(glad PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/glad/include
)

# --- Main executable ---
add_executable(NaniteDemo
    src/main.cpp
    src/core/mesh_loader.cpp
    src/build/cluster.cpp
    src/build/cluster_dag.cpp
    src/build/simplify.cpp
    src/runtime/packed_view.cpp
    src/runtime/dag_traversal.cpp
    src/runtime/rasterizer.cpp
    src/render/display.cpp
    src/render/camera.cpp
)

target_include_directories(NaniteDemo PRIVATE src)
target_link_libraries(NaniteDemo PRIVATE glfw glm::glm glad)

# Link OpenGL
find_package(OpenGL REQUIRED)
target_link_libraries(NaniteDemo PRIVATE OpenGL::GL)

# Copy assets to build directory
add_custom_command(TARGET NaniteDemo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/assets
    $<TARGET_FILE_DIR:NaniteDemo>/assets
)
