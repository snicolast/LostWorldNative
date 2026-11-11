# dlss_bridge.dll: src/video/dlss.cpp and NVIDIA's NGX library behind a table
# of C functions (src/video/dlss_bridge.h), for games built with a compiler
# NGX does not link with. A project of its own: it needs MSVC, and neither the
# ROM nor anything generated from it.
#
#   cmake -S tools/dlss_bridge -B build/dlss_bridge -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/dlss_bridge
#
# from a "x64 Native Tools Command Prompt". The result is
# build/dlss_bridge/dlss_bridge.dll, which goes next to the game's executable
# together with NVIDIA's nvngx_dlss.dll.
cmake_minimum_required(VERSION 3.20)
project(dlss_bridge LANGUAGES C CXX)

if(NOT MSVC)
    message(FATAL_ERROR "dlss_bridge.dll is built with MSVC: NVIDIA's NGX library links with nothing else")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# The NGX SDK, from NVIDIA's own repository and checked file by file. The
# library is the one for the static C runtime (_s), so the DLL asks for no
# Visual C++ redistributable on the PC it runs on.
set(NGX_TAG v310.9.16)
set(NGX_URL https://raw.githubusercontent.com/NVIDIA/DLSS/${NGX_TAG})
set(NGX_DIR ${CMAKE_BINARY_DIR}/ngx-${NGX_TAG})
function(ngx_file path url sha256)
    if(NOT EXISTS ${NGX_DIR}/${path})
        file(DOWNLOAD ${NGX_URL}/${url} ${NGX_DIR}/${path}.part EXPECTED_HASH SHA256=${sha256} STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE ${NGX_DIR}/${path}.part)
            message(FATAL_ERROR "Could not download ${NGX_URL}/${url}: ${status}")
        endif()
        file(RENAME ${NGX_DIR}/${path}.part ${NGX_DIR}/${path})
    endif()
endfunction()
ngx_file(include/nvsdk_ngx.h include/nvsdk_ngx.h dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848)
ngx_file(include/nvsdk_ngx_defs.h include/nvsdk_ngx_defs.h ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e)
ngx_file(include/nvsdk_ngx_defs_vk.h include/nvsdk_ngx_defs_vk.h 0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031)
ngx_file(include/nvsdk_ngx_helpers.h include/nvsdk_ngx_helpers.h 5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471)
ngx_file(include/nvsdk_ngx_helpers_vk.h include/nvsdk_ngx_helpers_vk.h c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623)
ngx_file(include/nvsdk_ngx_loader.h include/nvsdk_ngx_loader.h abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75)
ngx_file(include/nvsdk_ngx_params.h include/nvsdk_ngx_params.h 943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc)
ngx_file(include/nvsdk_ngx_vk.h include/nvsdk_ngx_vk.h 2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d)
ngx_file(lib/nvsdk_ngx_s.lib lib/Windows_x86_64/x64/nvsdk_ngx_s.lib 7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765)
ngx_file(LICENSE.txt LICENSE.txt d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0)

set(INCLUDES ${ROOT}/src/video ${ROOT}/third_party/vulkan-headers/include ${ROOT}/third_party/volk)
# volk loads vulkan-1.dll at runtime: the SDK is not needed
set(DEFINES VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR _CRT_SECURE_NO_WARNINGS)

add_library(dlss_bridge SHARED
    ${ROOT}/src/video/dlss_bridge.cpp
    ${ROOT}/src/video/dlss.cpp
    ${ROOT}/third_party/volk/volk.c)
target_include_directories(dlss_bridge PRIVATE ${INCLUDES} ${NGX_DIR}/include)
target_compile_definitions(dlss_bridge PRIVATE ${DEFINES} DLSS_BRIDGE_BUILD)
target_compile_options(dlss_bridge PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8>)
target_link_libraries(dlss_bridge PRIVATE ${NGX_DIR}/lib/nvsdk_ngx_s.lib)
set_target_properties(dlss_bridge PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded PREFIX "")

# The game's side of the bridge against the DLL just built: it must load, agree
# on the version and answer the first question the game asks, on any PC (no
# NVIDIA GPU needed: then the answer is "no extensions").
enable_testing()
add_executable(dlss_bridge_test
    ${CMAKE_CURRENT_SOURCE_DIR}/bridge_test.cpp
    ${ROOT}/src/video/dlss_loader.cpp
    ${ROOT}/third_party/volk/volk.c)
target_include_directories(dlss_bridge_test PRIVATE ${INCLUDES})
target_compile_definitions(dlss_bridge_test PRIVATE ${DEFINES})
add_dependencies(dlss_bridge_test dlss_bridge)
add_test(NAME dlss_bridge_test COMMAND dlss_bridge_test)
