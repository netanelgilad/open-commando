# Rebel portable platform layer: Win32 / DirectX / Miles / Bink replacements
# used when REBEL_PORTABLE is on (Emscripten web build).
file(GLOB REBEL_PLATFORM_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/win32/*.cpp
)
add_library(RebelPlatform STATIC ${REBEL_PLATFORM_SOURCES})
target_include_directories(RebelPlatform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/compat
    PRIVATE
        ${PROJECT_SOURCE_DIR}/Code/wwlib
        ${PROJECT_SOURCE_DIR}/Code/wwdebug
)
