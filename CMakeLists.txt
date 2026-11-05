cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED VINPUT_PROJECT_VERSION)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
        VINPUT_PROJECT_VERSION
        LIMIT_COUNT 1)
endif()
set(VINPUT_PROJECT_VERSION "${VINPUT_PROJECT_VERSION}"
    CACHE STRING "Project version for builds and release artifacts")
project(fcitx5-vinput VERSION ${VINPUT_PROJECT_VERSION} LANGUAGES CXX)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    # Fcitx5 discovers addons from its system prefix. Keep the source-build
    # default aligned so `cmake --install` produces a visible addon.
    set(CMAKE_INSTALL_PREFIX "/usr"
        CACHE PATH "Install path prefix, prepended onto install directories."
        FORCE)
endif()

include(GNUInstallDirs)

option(VINPUT_ENABLE_LOCAL_ASR "Build with sherpa-onnx local ASR support" ON)
option(VINPUT_ENABLE_WERROR "Treat compiler warnings as errors" OFF)

find_package(Fcitx5Core REQUIRED)
find_package(Fcitx5Config REQUIRED)
include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")

# Override to C++20 after Fcitx5CompilerSettings (which may set an older standard)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_compile_options(-ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections -Wl,--as-needed)

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
find_package(PkgConfig REQUIRED)
find_package(Gettext REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

pkg_check_modules(SDBUS REQUIRED libsystemd)
find_package(LibArchive REQUIRED)
find_package(OpenSSL REQUIRED)

# 3rdparty vendored header-only libraries (SYSTEM INTERFACE)
include(cmake/ThirdParty.cmake)

set(VINPUT_SYSTEMD_USER_DIR
    "${CMAKE_INSTALL_DATAROOTDIR}/systemd/user"
    CACHE PATH "Install directory for the vinput systemd --user unit")
set(VINPUT_DBUS_SERVICE_DIR
    "${CMAKE_INSTALL_DATAROOTDIR}/dbus-1/services"
    CACHE PATH "Install directory for the vinput D-Bus activation file")
set(VINPUT_RUNTIME_LIBDIR
    "${CMAKE_INSTALL_LIBDIR}/fcitx5-vinput"
    CACHE PATH "Install directory for bundled sherpa-onnx runtime libraries")
set(VINPUT_ASR_PROVIDER_DIR
    "${CMAKE_INSTALL_DATAROOTDIR}/fcitx5-vinput/asr-providers"
    CACHE PATH "Install directory for built-in ASR provider scripts")
set(VINPUT_LLM_ADAPTOR_DIR
    "${CMAKE_INSTALL_DATAROOTDIR}/fcitx5-vinput/llm-adapters"
    CACHE PATH "Install directory for built-in LLM adapter scripts")
set(VINPUT_RUNTIME_MODE "bundled"
    CACHE STRING "Runtime library mode for vinput-daemon: system or bundled")
set_property(CACHE VINPUT_RUNTIME_MODE PROPERTY STRINGS system bundled)
if(NOT VINPUT_RUNTIME_MODE STREQUAL "system" AND
   NOT VINPUT_RUNTIME_MODE STREQUAL "bundled")
    message(FATAL_ERROR
        "VINPUT_RUNTIME_MODE must be either 'system' or 'bundled'")
endif()
set(VINPUT_DAEMON_EXECUTABLE
    "${CMAKE_INSTALL_FULL_BINDIR}/vinput-daemon")

configure_file(
    ${CMAKE_SOURCE_DIR}/src/common/config.h.in
    ${CMAKE_BINARY_DIR}/config.h
    @ONLY)

add_subdirectory(data)
add_subdirectory(src/common)
add_subdirectory(src/daemon)
add_subdirectory(src/addon)
add_subdirectory(src/cli)
add_subdirectory(src/gui)
add_subdirectory(po)

include(CTest)
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# Umbrella target that builds every translation artefact (gettext .mo + Qt .qm)
set(VINPUT_TRANSLATIONS_DEPS gettext-translations)
if(TARGET qt-translations)
    list(APPEND VINPUT_TRANSLATIONS_DEPS qt-translations)
endif()
add_custom_target(translations ALL DEPENDS ${VINPUT_TRANSLATIONS_DEPS})

include(cmake/Packaging.cmake)
