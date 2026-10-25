cmake_minimum_required(VERSION 3.22)
project(Mobile3D LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions -fno-rtti")

include(FetchContent)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)
FetchContent_Declare(tinygltf GIT_REPOSITORY https://github.com/syoyo/tinygltf.git GIT_TAG v2.9.3)
FetchContent_Declare(bullet GIT_REPOSITORY https://github.com/bulletphysics/bullet3.git GIT_TAG 3.25)

set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_BULLET2_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_CPU_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_OPENGL3_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(USE_GRAPHICAL_BENCHMARK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glm tinygltf bullet)

add_library(mobile3d SHARED
  src/render/Renderer.cpp
  src/assets/GltfLoader.cpp
  src/platform/engine_c_api.cpp)

target_include_directories(mobile3d PUBLIC src ${BULLET_PHYSICS_SOURCE_DIR}/src)
target_link_libraries(mobile3d PUBLIC glm::glm tinygltf BulletDynamics BulletCollision LinearMath)

if(ANDROID)
  target_sources(mobile3d PRIVATE src/platform/android_jni.cpp)
  target_link_libraries(mobile3d PUBLIC GLESv3 EGL android log)
endif()
