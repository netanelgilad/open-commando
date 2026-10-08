set(SOURCES
    BandTest.cpp
    BandTest.rc
)

if(REBEL_PORTABLE)
    # Raw-socket bandwidth probe can't run in a browser; link a stub instead.
    add_library(BandTest STATIC ${PROJECT_SOURCE_DIR}/Code/Platform/net/bandtest_portable.cpp)
    target_include_directories(BandTest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(BandTest PRIVATE BANDTEST_EXPORTS)
    return()
endif()

add_library(BandTest SHARED ${SOURCES})
target_compile_definitions(BandTest PRIVATE
    _MBCS
    _USRDLL
    BANDTEST_EXPORTS
)
target_link_libraries(BandTest PRIVATE
    odbc32
    odbccp32
    ws2_32
    winmm
)
set_target_properties(BandTest PROPERTIES
        OUTPUT_NAME_DEBUG "BandTestD"
        OUTPUT_NAME_RELWITHDEBINFO "BandTestP"
)