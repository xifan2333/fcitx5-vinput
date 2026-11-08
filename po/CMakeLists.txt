set(GETTEXT_PACKAGE "fcitx5-vinput")

set(PO_FILES zh_CN.po)

gettext_process_po_files(zh_CN ALL PO_FILES ${PO_FILES})

set(VINPUT_BUILD_LOCALE_DIR "${CMAKE_BINARY_DIR}/locale/zh_CN/LC_MESSAGES")
set(VINPUT_BUILD_LOCALE_FILE "${VINPUT_BUILD_LOCALE_DIR}/fcitx5-vinput.mo")

add_custom_command(
  OUTPUT ${VINPUT_BUILD_LOCALE_FILE}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${VINPUT_BUILD_LOCALE_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/zh_CN.gmo ${VINPUT_BUILD_LOCALE_FILE}
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/zh_CN.gmo
  COMMENT "Copying CLI translations into build-tree locale directory"
)

add_custom_target(gettext-translations ALL DEPENDS ${VINPUT_BUILD_LOCALE_FILE})

install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/zh_CN.gmo
  DESTINATION ${CMAKE_INSTALL_LOCALEDIR}/zh_CN/LC_MESSAGES
  RENAME fcitx5-vinput.mo
)
