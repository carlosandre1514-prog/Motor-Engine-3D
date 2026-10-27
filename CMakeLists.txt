cmake_minimum_required(VERSION 3.22)
project(Mobile3D LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Obs.: exceções/RTTI ficam ligados por enquanto (tinygltf/Bullet). Desligar depois, com tudo funcionando.

include(FetchContent)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)
FetchContent_Declare(tinygltf GIT_REPOSITORY https://github.com/syoyo/tinygltf.git GIT_TAG v2.9.3)
FetchContent_Declare(bullet GIT_REPOSITORY https://github.com/bulletphysics/bullet3.git GIT_TAG 3.25)

set(TINYGLTF_HEADER_ONLY ON CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_BULLET2_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_CPU_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_OPENGL3_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(USE_GRAPHICAL_BENCHMARK OFF CACHE BOOL "" FORCE)
set(INSTALL_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glm tinygltf bullet)

add_library(mobile3d SHARED
  src/render/Renderer.cpp
  src/assets/GltfLoader.cpp
  src/platform/engine_c_api.cpp)

target_compile_definitions(mobile3d PUBLIC GLM_ENABLE_EXPERIMENTAL)
target_include_directories(mobile3d PUBLIC
  src
  ${glm_SOURCE_DIR}
  ${tinygltf_SOURCE_DIR}
  ${bullet_SOURCE_DIR}/src)
target_link_libraries(mobile3d PUBLIC BulletDynamics BulletCollision LinearMath)

if(ANDROID)
  target_sources(mobile3d PRIVATE src/platform/android_jni.cpp)
  target_link_libraries(mobile3d PUBLIC GLESv3 EGL android log)
else()
  # Build de desenvolvimento no PC/CI (Linux + Mesa): teste headless que renderiza as cenas em PNG
  target_link_libraries(mobile3d PUBLIC GLESv2 EGL)
  option(MOBILE3D_HEADLESS_TEST "Compila tools/headless_test" ON)
  if(MOBILE3D_HEADLESS_TEST)
    add_executable(mobile3d_headless tools/headless_test.cpp)
    target_link_libraries(mobile3d_headless PRIVATE mobile3d)
  endif()
endif()
