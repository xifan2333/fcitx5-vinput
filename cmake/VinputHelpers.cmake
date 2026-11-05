# cmake/VinputHelpers.cmake
# Standardized helper functions for target configuration and unit testing.

# vinput_add_test(NAME [SOURCES src1 src2...] [LIBS lib1 lib2...] [ENV "KEY=VAL;..."])
# Automatically handles include directories, standard library linkage (common, Threads),
# and test registration via CTest.
function(vinput_add_test TEST_NAME)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;LIBS;ENV")

    if(NOT ARG_SOURCES)
        set(ARG_SOURCES "${TEST_NAME}.cpp")
    endif()

    add_executable(${TEST_NAME} ${ARG_SOURCES})

    target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )

    target_link_libraries(${TEST_NAME} PRIVATE
        common
        Threads::Threads
        ${ARG_LIBS}
    )

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

    if(ARG_ENV)
        set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "${ARG_ENV}")
    endif()
endfunction()
