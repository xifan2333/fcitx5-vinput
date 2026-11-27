# cmake/VinputHelpers.cmake Standardized helper functions for target configuration and unit testing.

# vinput_add_test(NAME [SOURCES src1 src2...] [LIBS lib1 lib2...] [ENV "KEY=VAL;..."]) Automatically
# handles include directories, standard library linkage (common, Threads), and test registration via
# CTest.
function (vinput_add_test TEST_NAME)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;LIBS;ENV")

  if (NOT ARG_SOURCES)
    set(ARG_SOURCES "${TEST_NAME}.cpp")
  endif ()

  add_executable(${TEST_NAME} ${ARG_SOURCES})

  target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)

  target_link_libraries(${TEST_NAME} PRIVATE common Threads::Threads ${ARG_LIBS})

  add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

  if (ARG_ENV)
    set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "${ARG_ENV}")
  endif ()
endfunction ()

# vinput_find_qt(OUT_MAJOR_VAR [MAJOR auto|6|5] COMPONENTS comp1 [comp2 ...] [REQUIRED])
# Resolves Qt6 or Qt5 (>= 5.12) according to MAJOR (defaulting to VINPUT_QT_MAJOR). Sets
# ${OUT_MAJOR_VAR} in the caller scope to 6, 5, or "" when an optional lookup misses.
function (vinput_find_qt OUT_MAJOR_VAR)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "REQUIRED" "MAJOR" "COMPONENTS")

  set(requested_major "${ARG_MAJOR}")
  if (NOT requested_major)
    set(requested_major "${VINPUT_QT_MAJOR}")
  endif ()

  set(found_major "")

  if (requested_major STREQUAL "6" OR requested_major STREQUAL "auto")
    find_package(
      Qt6
      COMPONENTS ${ARG_COMPONENTS}
      QUIET
    )
    if (Qt6_FOUND)
      set(found_major "6")
    elseif (requested_major STREQUAL "6" AND ARG_REQUIRED)
      find_package(
        Qt6
        COMPONENTS ${ARG_COMPONENTS}
        REQUIRED
      )
    endif ()
  endif ()

  if (NOT found_major AND (requested_major STREQUAL "5" OR requested_major STREQUAL "auto"))
    find_package(
      Qt5 5.12
      COMPONENTS ${ARG_COMPONENTS}
      QUIET
    )
    if (Qt5_FOUND)
      set(found_major "5")
    elseif (ARG_REQUIRED)
      find_package(
        Qt5 5.12
        COMPONENTS ${ARG_COMPONENTS}
        REQUIRED
      )
    endif ()
  endif ()

  if (NOT found_major AND ARG_REQUIRED)
    message(
      FATAL_ERROR
        "Could not find a suitable Qt installation (VINPUT_QT_MAJOR=${requested_major}; requires Qt 6 or Qt 5 >= 5.12)."
    )
  endif ()

  set(${OUT_MAJOR_VAR}
      "${found_major}"
      PARENT_SCOPE
  )
endfunction ()
