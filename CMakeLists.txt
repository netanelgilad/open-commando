cmake_minimum_required(VERSION 3.20)

project(CnC_Rebel VERSION 1.37.3)
set(REBEL_CODENAME "SANDWORM")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(REBEL_ENABLE_DEBUG_MESSAGES "Enable debug messages (WWDEBUG)" ON)
option(REBEL_ENABLE_EDITOR "Enable editor building" ON)
option(REBEL_ENABLE_TOOLS "Enable tools building" ON)
option(BUILD_TESTING "Enable unit-tests" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

option(REBEL_PORTABLE "Build for non-Windows targets (macOS native, Emscripten/web)" OFF)
if(NOT WIN32)
    set(REBEL_PORTABLE ON)
endif()

if(REBEL_PORTABLE)
    set(REBEL_ENABLE_EDITOR OFF)
    set(REBEL_ENABLE_TOOLS OFF)
    # Replacement Win32 / D3D / Miles / Bink headers live here; backends in Code/Platform
    include_directories(BEFORE SYSTEM "${CMAKE_SOURCE_DIR}/Code/Platform/compat")
    # _WINDOWS: the compat layer provides Win32, so take the engine's Windows paths.
    add_compile_definitions(REBEL_PORTABLE _WINDOWS)
    add_compile_options("SHELL:-include ${CMAKE_SOURCE_DIR}/Code/Platform/compat/rebel_portable.h" -pthread)
    # C++ TUs also get the Win32/COM declarations at global scope up front, as
    # the Windows SDK headers effectively were. WWOnline includes wolapi.h
    # inside "namespace WOL {}", which would otherwise capture first includes.
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_SOURCE_DIR}/Code/Platform/compat/ole2.h>")
    # No -fwasm-exceptions: the engine never catches (no try blocks), and the
    # game links with ASYNCIFY, which does not support wasm exception handling.
    add_link_options(-pthread)
    add_compile_options(-Wno-everything -fms-extensions -fno-delayed-template-parsing)
    # MSVC passes class objects through "..." bitwise; clang compiles that to a
    # runtime trap (e.g. data-safe floats in DIAG_LOG). Make it a build error.
    add_compile_options(-Werror=non-pod-varargs)
    # Windows system / proprietary SDK libraries become empty targets; the
    # replacements are compiled into RebelPlatform instead.
    foreach(_lib dinput8 imagehlp odbc32 odbccp32 shlwapi version vfw32 Vfw32 winmm ws2_32)
        add_library(${_lib} INTERFACE)
    endforeach()
    add_library(Microsoft::D3DX INTERFACE IMPORTED)
    add_library(Microsoft::D3DX9 INTERFACE IMPORTED)
    set(MSS_INCLUDE_DIRS "")
    set(MSS_LIBRARIES "")
    set(BINK_INCLUDE_DIRS "")
    set(BINK_LIBRARIES "")
else()
    find_package(dxsdk-d3dx CONFIG REQUIRED)
    find_package(MSS REQUIRED)
    find_package(BINK REQUIRED)
endif()
if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    include(GoogleTest)
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Globally set Mt(d) since whole project depends on it
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(
    "$<$<BOOL:${REBEL_ENABLE_DEBUG_MESSAGES}>:WWDEBUG>"
    "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:_CRT_SECURE_NO_WARNINGS;_CRT_NONSTDC_NO_DEPRECATE;WINVER=0x0601;WIN32;_WINDOWS>"
)

add_compile_options(
    "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Gy;/GF;/W4;/Zc:inline;/Zc:wchar_t>"
)

add_custom_target(GetGitHash ALL)

add_custom_command(
    POST_BUILD
    TARGET GetGitHash
    COMMAND ${CMAKE_COMMAND}
    -D SOURCE_DIR=${PROJECT_SOURCE_DIR}
    -D TARGET_DIR=${PROJECT_BINARY_DIR}
    -D PROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    -D PROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    -D PROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    -D PROJECT_CODENAME=${REBEL_CODENAME}
    -P ${PROJECT_SOURCE_DIR}/cmake/CheckGit.cmake
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
)

add_subdirectory(Code)
