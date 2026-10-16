cmake_minimum_required(VERSION 3.20)
project(capturer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)

find_package(X11 REQUIRED)

find_package(Vulkan REQUIRED)
find_library(PIPEWIRE_LIBRARY NAMES pipewire-0.3)
find_path(PIPEWIRE_INCLUDE_DIR NAMES pipewire/pipewire.h PATH_SUFFIXES pipewire-0.3)
find_path(SPA_INCLUDE_DIR NAMES spa/support/plugin.h PATH_SUFFIXES spa-0.2)
if(PIPEWIRE_INCLUDE_DIR AND SPA_INCLUDE_DIR)
    add_definitions(-DHAVE_PIPEWIRE=1)
else()
    add_definitions(-DHAVE_PIPEWIRE=0)
endif()

add_library(capturer_core STATIC
    src/capture/pipeline.cpp
    src/gpu/vulkan_runtime.cpp
    src/image_processing/frame_processor.cpp
    src/image_processing/tile_difference.cpp
)

target_include_directories(capturer_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        /usr/include/libdrm
)
if(PIPEWIRE_INCLUDE_DIR)
    target_include_directories(capturer_core PUBLIC ${PIPEWIRE_INCLUDE_DIR})
endif()
if(SPA_INCLUDE_DIR)
    target_include_directories(capturer_core PUBLIC ${SPA_INCLUDE_DIR})
endif()

target_compile_features(capturer_core PUBLIC cxx_std_20)
target_link_libraries(capturer_core PUBLIC Vulkan::Vulkan)
target_link_libraries(capturer_core PUBLIC drm gbm)
if(PIPEWIRE_LIBRARY)
    target_link_libraries(capturer_core PUBLIC ${PIPEWIRE_LIBRARY})
endif()

add_executable(capturer_tests
    tests/test_tile_difference.cpp
)

target_link_libraries(capturer_tests PRIVATE capturer_core)

add_executable(capturer_pipeline_tests
    tests/test_pipeline.cpp
)

target_link_libraries(capturer_pipeline_tests PRIVATE capturer_core)

add_executable(capturer_sample
    src/capture/x11_desktop_capture.cpp
    src/sample/desktop_capture_sample.cpp
    src/sample/wayland_portal_capture.cpp
)

if(PIPEWIRE_INCLUDE_DIR AND PIPEWIRE_LIBRARY)
    target_sources(capturer_sample PRIVATE src/sample/pipewire_desktop_capture.cpp)
    target_include_directories(capturer_sample PRIVATE ${PIPEWIRE_INCLUDE_DIR})
    if(SPA_INCLUDE_DIR)
        target_include_directories(capturer_sample PRIVATE ${SPA_INCLUDE_DIR})
    endif()
    target_link_libraries(capturer_sample PRIVATE ${PIPEWIRE_LIBRARY})
endif()

target_link_libraries(capturer_sample PRIVATE capturer_core X11::X11 PkgConfig::GIO)

add_executable(capturer_pipewire_benchmark
    src/sample/pipewire_capture_benchmark.cpp
    src/sample/wayland_portal_capture.cpp
)
target_include_directories(capturer_pipewire_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(PIPEWIRE_INCLUDE_DIR AND PIPEWIRE_LIBRARY)
    target_sources(capturer_pipewire_benchmark PRIVATE src/sample/pipewire_desktop_capture.cpp)
    target_include_directories(capturer_pipewire_benchmark PRIVATE ${PIPEWIRE_INCLUDE_DIR})
    if(SPA_INCLUDE_DIR)
        target_include_directories(capturer_pipewire_benchmark PRIVATE ${SPA_INCLUDE_DIR})
    endif()
    target_link_libraries(capturer_pipewire_benchmark PRIVATE ${PIPEWIRE_LIBRARY})
endif()

target_link_libraries(capturer_pipewire_benchmark PRIVATE PkgConfig::GIO)

if(BUILD_TESTING)
    add_test(NAME tile_difference_tests COMMAND capturer_tests)
    add_test(NAME pipeline_tests COMMAND capturer_pipeline_tests)
endif()
