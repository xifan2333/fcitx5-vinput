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

# 3. C++ standard enforcement
# Override to C++20 after Fcitx5CompilerSettings (which may set an older standard)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 4. Symbol visibility and dead-code stripping
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_compile_options(-ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections -Wl,--as-needed)

# 5. Interprocedural Optimization (IPO / LTO)
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
    if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION AND NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
    if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION AND NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    endif()
    message(STATUS "Interprocedural Optimization (LTO) enabled for Release builds")
else()
    message(STATUS "Interprocedural Optimization (LTO) not supported: ${ipo_error}")
endif()

# 6. Compiler warnings and diagnostic flags
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wunused
        -Wunneeded-internal-declaration
        -Wunneeded-member-function
        -Wunreachable-code
        -Wredundant-decls
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_compile_options(
        -Wall
        -Wextra
        -Wunused
        -Wredundant-decls
    )
endif()

if(VINPUT_ENABLE_WERROR)
    add_compile_options(-Werror)
endif()
