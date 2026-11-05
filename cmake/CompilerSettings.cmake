# cmake/CompilerSettings.cmake
# Configures modern compiler toolchains, ccache acceleration, mold linker,
# C++20 standard enforcement, symbol visibility, and warning diagnostics.

# 1. Automatic ccache detection
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    message(STATUS "Found ccache: ${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# 2. Modern concurrent linker: mold
option(VINPUT_USE_MOLD "Use mold linker if available" ON)
if(VINPUT_USE_MOLD)
    find_program(MOLD_PROGRAM mold)
    if(MOLD_PROGRAM)
        if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.18")
            include(CheckCXXLinkerFlag)
            check_cxx_linker_flag("-fuse-ld=mold" VINPUT_HAS_MOLD_FLAG)
            if(VINPUT_HAS_MOLD_FLAG)
                message(STATUS "Using mold linker: -fuse-ld=mold")
                add_link_options("-fuse-ld=mold")
            else()
                message(STATUS "mold found but compiler does not accept -fuse-ld=mold; using default linker")
            endif()
        else()
            message(STATUS "Using mold linker: -fuse-ld=mold")
            add_link_options("-fuse-ld=mold")
        endif()
    endif()
endif()
