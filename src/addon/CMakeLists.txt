find_package(Fcitx5ModuleDBus REQUIRED)
find_package(Fcitx5ModuleClipboard REQUIRED)

add_library(fcitx5-vinput MODULE
    vinput.cpp
)
target_link_libraries(fcitx5-vinput PRIVATE
    vinput-common
    Fcitx5::Core
    Fcitx5::Module::DBus
    Fcitx5::Module::Clipboard
)
target_include_directories(fcitx5-vinput PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
set_target_properties(fcitx5-vinput PROPERTIES PREFIX "")

install(TARGETS fcitx5-vinput DESTINATION "${FCITX_INSTALL_LIBDIR}/fcitx5")

configure_file(vinput-addon.conf.in vinput-addon.conf @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vinput-addon.conf"
        DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
        RENAME vinput.conf)
