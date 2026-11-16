# data/CMakeLists.txt Installation of static assets, default configuration, man pages, and shell
# completions.

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/default-config.json
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/fcitx5-vinput
)

# Manual pages (English)
install(
  FILES ${CMAKE_CURRENT_SOURCE_DIR}/man/en/vinput.1
        ${CMAKE_CURRENT_SOURCE_DIR}/man/en/vinput-daemon.1
        ${CMAKE_CURRENT_SOURCE_DIR}/man/en/vinput-gui.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/man/en/vinput-config.5
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man5
)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/man/en/fcitx5-vinput.7
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man7
)

# Manual pages (Simplified Chinese)
install(
  FILES ${CMAKE_CURRENT_SOURCE_DIR}/man/zh_CN/vinput.1
        ${CMAKE_CURRENT_SOURCE_DIR}/man/zh_CN/vinput-daemon.1
        ${CMAKE_CURRENT_SOURCE_DIR}/man/zh_CN/vinput-gui.1
  DESTINATION ${CMAKE_INSTALL_MANDIR}/zh_CN/man1
)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/man/zh_CN/vinput-config.5
        DESTINATION ${CMAKE_INSTALL_MANDIR}/zh_CN/man5
)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/man/zh_CN/fcitx5-vinput.7
        DESTINATION ${CMAKE_INSTALL_MANDIR}/zh_CN/man7
)

# Shell completions
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/completions/bash/vinput
        DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions
)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/completions/zsh/_vinput
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/site-functions
)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/completions/fish/vinput.fish
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d
)

# Udev rules for hardware triggers
if (NOT DEFINED UDEV_RULES_DIR)
  find_package(PkgConfig QUIET)
  if (PKG_CONFIG_FOUND)
    pkg_get_variable(_UDEV_DIR udev udevdir)
  endif ()
  if (_UDEV_DIR)
    set(_DEFAULT_UDEV_RULES_DIR "${_UDEV_DIR}/rules.d")
  else ()
    set(_DEFAULT_UDEV_RULES_DIR "/usr/lib/udev/rules.d")
  endif ()
  set(UDEV_RULES_DIR
      "${_DEFAULT_UDEV_RULES_DIR}"
      CACHE PATH "Install directory for udev rules"
  )
endif ()

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/udev/70-fcitx5-vinput.rules
        DESTINATION "${UDEV_RULES_DIR}"
)
