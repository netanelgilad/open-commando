# Rebel portable platform layer: Win32 / DirectX / Miles / Bink replacements
# used when REBEL_PORTABLE is on (Emscripten web build).
file(GLOB REBEL_PLATFORM_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/win32/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/*.cpp
)
add_library(RebelPlatform STATIC ${REBEL_PLATFORM_SOURCES})
target_include_directories(RebelPlatform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/compat
    PRIVATE
        ${PROJECT_SOURCE_DIR}/Code/wwlib
        ${PROJECT_SOURCE_DIR}/Code/wwdebug
        ${PROJECT_SOURCE_DIR}/Code/third_party/minimp3
)
# Emscripten's OpenAL is implemented on Web Audio.
target_link_options(RebelPlatform INTERFACE -lopenal)

# Route every libc path entry point through Rebel_Resolve_Path (path_resolve.cpp).
set(REBEL_WRAPPED_PATH_FUNCS fopen freopen open stat lstat access opendir mkdir rmdir unlink remove rename chdir truncate)
foreach(_fn ${REBEL_WRAPPED_PATH_FUNCS})
    target_link_options(RebelPlatform INTERFACE "LINKER:--wrap=${_fn}")
endforeach()

# Node-run smoke tests against a real Renegade install (REBEL_GAME_DIR).
add_executable(rebel_mix_smoke tests/mix_smoke.cpp)
target_link_libraries(rebel_mix_smoke PRIVATE WWLib WWDebug RebelPlatform)
target_link_options(rebel_mix_smoke PRIVATE -lnodefs.js -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1 -sENVIRONMENT=node,worker)
add_executable(rebel_audio_smoke tests/audio_smoke.cpp)
target_link_libraries(rebel_audio_smoke PRIVATE WWLib WWDebug RebelPlatform)
target_link_options(rebel_audio_smoke PRIVATE -lnodefs.js -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1 -sENVIRONMENT=node,worker)
