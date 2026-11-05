# cmake/CompilerSettings.cmake
# Project-wide compiler behavior, diagnostic warnings, C++20 standard enforcement,
# symbol visibility, and link-time optimization (LTO/IPO).
#
# Toolchain preferences (such as compiler selection, mold linker, or sccache/ccache)
# belong to CMakePresets.json and the build environment, keeping project rules decoupled.

# 1. C++ standard enforcement
# Override to C++20 after Fcitx5CompilerSettings (which may set an older standard)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 2. Symbol visibility and dead-code stripping
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_compile_options(-ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections -Wl,--as-needed)

# 3. Interprocedural Optimization (IPO / LTO)
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
    if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
    if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    endif()
    message(STATUS "Interprocedural Optimization (LTO) enabled for Release builds")
else()
    message(STATUS "Interprocedural Optimization (LTO) not supported: ${ipo_error}")
endif()

# 4. Compiler warnings and diagnostic flags
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
