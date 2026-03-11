set(GETTEXT_PACKAGE "fcitx5-vinput")

set(PO_FILES
    zh_CN.po
)

gettext_process_po_files(
    zh_CN
    ALL
    PO_FILES ${PO_FILES}
)

install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/zh_CN.gmo
    DESTINATION ${CMAKE_INSTALL_LOCALEDIR}/zh_CN/LC_MESSAGES
    RENAME fcitx5-vinput.mo
)
