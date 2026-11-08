# cmake/Packaging.cmake CPack configuration for DEB, RPM, and source archive packaging.

set(VINPUT_PACKAGE_RELEASE
    "1"
    CACHE STRING "Package release revision for distro packages"
)
set(VINPUT_PACKAGE_CONTACT
    "fcitx5-vinput maintainers <noreply@github.com>"
    CACHE STRING "Contact metadata used by generated distro packages"
)
set(VINPUT_PACKAGE_HOMEPAGE_URL
    ""
    CACHE STRING "Homepage URL used by generated distro packages"
)
set(VINPUT_PACKAGE_DESCRIPTION_SUMMARY
    "Offline voice input addon for Fcitx5 with optional OpenAI-compatible postprocess"
    CACHE STRING "Short description used by generated distro packages"
)

string(REPLACE "-" "~" VINPUT_DEBIAN_UPSTREAM_VERSION "${PROJECT_VERSION}")

if (NOT VINPUT_ENABLE_LOCAL_ASR)
  set(CPACK_PACKAGE_NAME "fcitx5-vinput-lite")
  set(CPACK_DEBIAN_PACKAGE_NAME "fcitx5-vinput-lite")
  set(CPACK_DEBIAN_PACKAGE_PROVIDES
      "fcitx5-vinput (= ${VINPUT_DEBIAN_UPSTREAM_VERSION}-${VINPUT_PACKAGE_RELEASE})"
  )
  set(CPACK_DEBIAN_PACKAGE_CONFLICTS "fcitx5-vinput")
  set(CPACK_RPM_PACKAGE_NAME "fcitx5-vinput-lite")
  set(CPACK_RPM_PACKAGE_PROVIDES "fcitx5-vinput = ${PROJECT_VERSION}-${VINPUT_PACKAGE_RELEASE}")
  set(CPACK_RPM_PACKAGE_CONFLICTS "fcitx5-vinput")
else ()
  set(CPACK_PACKAGE_NAME "fcitx5-vinput")
  set(CPACK_DEBIAN_PACKAGE_NAME "fcitx5-vinput")
  set(CPACK_DEBIAN_PACKAGE_CONFLICTS "fcitx5-vinput-lite")
  set(CPACK_RPM_PACKAGE_NAME "fcitx5-vinput")
  set(CPACK_RPM_PACKAGE_CONFLICTS "fcitx5-vinput-lite")
endif ()

set(CPACK_PACKAGE_VENDOR "fcitx5-vinput")
set(CPACK_PACKAGE_CONTACT "${VINPUT_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${VINPUT_PACKAGE_DESCRIPTION_SUMMARY}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "fcitx5-vinput")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
if (VINPUT_PACKAGE_HOMEPAGE_URL)
  set(CPACK_PACKAGE_HOMEPAGE_URL "${VINPUT_PACKAGE_HOMEPAGE_URL}")
endif ()

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${VINPUT_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
list(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS fcitx5)
set(CPACK_DEBIAN_PACKAGE_VERSION "${VINPUT_DEBIAN_UPSTREAM_VERSION}")
set(CPACK_DEBIAN_PACKAGE_RELEASE "${VINPUT_PACKAGE_RELEASE}")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

include(CPack)
