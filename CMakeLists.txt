cmake_minimum_required(VERSION 3.16)

set(VINPUT_PROJECT_VERSION "0.1.6"
    CACHE STRING "Project version for builds and release artifacts")
project(fcitx5-vinput VERSION ${VINPUT_PROJECT_VERSION} LANGUAGES CXX)

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

find_package(Fcitx5Core REQUIRED)
find_package(Fcitx5Config REQUIRED)
include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")

# Override to C++20 after Fcitx5CompilerSettings (which may set an older standard)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(PkgConfig REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)
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
set(VINPUT_DAEMON_EXECUTABLE
    "${CMAKE_INSTALL_FULL_BINDIR}/vinput-daemon")

configure_file(
    ${CMAKE_SOURCE_DIR}/src/common/config.h.in
    ${CMAKE_BINARY_DIR}/config.h
    @ONLY)

include_directories(${CMAKE_BINARY_DIR})


add_subdirectory(src/common)
add_subdirectory(src/daemon)
add_subdirectory(src/addon)
add_subdirectory(src/cli)
add_subdirectory(src/gui)
add_subdirectory(po)

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
set(CPACK_DEBIAN_PACKAGE_DEPENDS "fcitx5")
set(CPACK_DEBIAN_PACKAGE_VERSION "${VINPUT_DEBIAN_UPSTREAM_VERSION}")
set(CPACK_DEBIAN_PACKAGE_RELEASE "${VINPUT_PACKAGE_RELEASE}")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

include(CPack)
