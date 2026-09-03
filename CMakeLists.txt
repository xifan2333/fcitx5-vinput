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

set(VINPUT_PACKAGE_RELEASE "1"
    CACHE STRING "Package release revision for distro packages")
set(VINPUT_PACKAGE_CONTACT "fcitx5-vinput maintainers <noreply@github.com>"
    CACHE STRING "Contact metadata used by generated distro packages")
set(VINPUT_PACKAGE_HOMEPAGE_URL ""
    CACHE STRING "Homepage URL used by generated distro packages")
set(VINPUT_PACKAGE_DESCRIPTION_SUMMARY
    "Offline voice input addon for Fcitx5 with optional OpenAI-compatible postprocess"
    CACHE STRING "Short description used by generated distro packages")

option(VINPUT_ENABLE_LOCAL_ASR "Build with sherpa-onnx local ASR support" ON)

find_package(Fcitx5Core REQUIRED)
find_package(Fcitx5Config REQUIRED)
include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")

# Override to C++20 after Fcitx5CompilerSettings (which may set an older standard)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
find_package(PkgConfig REQUIRED)
option(VINPUT_FETCH_NLOHMANN_JSON "Fetch nlohmann_json with FetchContent when not found" ON)
find_package(nlohmann_json 3.2.0 QUIET)
if(NOT nlohmann_json_FOUND)
    if(VINPUT_FETCH_NLOHMANN_JSON)
        include(FetchContent)
        FetchContent_Declare(
            nlohmann_json
            URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        )
        FetchContent_MakeAvailable(nlohmann_json)
    else()
        message(FATAL_ERROR
            "nlohmann_json was not found. Install nlohmann_json or enable VINPUT_FETCH_NLOHMANN_JSON.")
    endif()
endif()
find_package(Gettext REQUIRED)

pkg_check_modules(SDBUS REQUIRED libsystemd)
find_package(CURL REQUIRED)
find_package(LibArchive REQUIRED)
find_package(OpenSSL REQUIRED)

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

install(FILES
    ${CMAKE_SOURCE_DIR}/data/default-config.json
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/fcitx5-vinput
)

install(FILES
    ${CMAKE_SOURCE_DIR}/data/man/en/vinput.1
    ${CMAKE_SOURCE_DIR}/data/man/en/vinput-daemon.1
    ${CMAKE_SOURCE_DIR}/data/man/en/vinput-gui.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
)
install(FILES
    ${CMAKE_SOURCE_DIR}/data/man/en/vinput-config.5
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man5
)
install(FILES
    ${CMAKE_SOURCE_DIR}/data/man/en/fcitx5-vinput.7
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man7
)

install(FILES
    ${CMAKE_SOURCE_DIR}/data/man/zh_CN/vinput.1
    ${CMAKE_SOURCE_DIR}/data/man/zh_CN/vinput-daemon.1
    ${CMAKE_SOURCE_DIR}/data/man/zh_CN/vinput-gui.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/zh_CN/man1
)
install(FILES
    ${CMAKE_SOURCE_DIR}/data/man/zh_CN/vinput-config.5
    DESTINATION ${CMAKE_INSTALL_MANDIR}/zh_CN/man5
)
install(FILES
    ${CMAKE_SOURCE_DIR}/data/man/zh_CN/fcitx5-vinput.7
    DESTINATION ${CMAKE_INSTALL_MANDIR}/zh_CN/man7
)

# Install shell completions
install(FILES
    ${CMAKE_SOURCE_DIR}/data/completions/bash/vinput
    DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions
)
install(FILES
    ${CMAKE_SOURCE_DIR}/data/completions/zsh/_vinput
    DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/site-functions
)
install(FILES
    ${CMAKE_SOURCE_DIR}/data/completions/fish/vinput.fish
    DESTINATION ${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d
)

include_directories(${CMAKE_BINARY_DIR})


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

string(REPLACE "-" "~" VINPUT_DEBIAN_UPSTREAM_VERSION "${PROJECT_VERSION}")

set(CPACK_PACKAGE_NAME "fcitx5-vinput")
set(CPACK_PACKAGE_VENDOR "fcitx5-vinput")
set(CPACK_PACKAGE_CONTACT "${VINPUT_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${VINPUT_PACKAGE_DESCRIPTION_SUMMARY}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "fcitx5-vinput")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
if(VINPUT_PACKAGE_HOMEPAGE_URL)
    set(CPACK_PACKAGE_HOMEPAGE_URL "${VINPUT_PACKAGE_HOMEPAGE_URL}")
endif()

set(CPACK_DEBIAN_PACKAGE_NAME "fcitx5-vinput")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${VINPUT_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
list(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS
    fcitx5
)
set(CPACK_DEBIAN_PACKAGE_VERSION "${VINPUT_DEBIAN_UPSTREAM_VERSION}")
set(CPACK_DEBIAN_PACKAGE_RELEASE "${VINPUT_PACKAGE_RELEASE}")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

include(CPack)
