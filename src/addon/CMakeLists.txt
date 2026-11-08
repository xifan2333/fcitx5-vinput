find_package(Fcitx5ModuleDBus REQUIRED)
find_package(Fcitx5ModuleClipboard REQUIRED)
find_package(Fcitx5ModuleNotifications REQUIRED)

add_library(
  addon_core OBJECT core/vinput.cpp core/context_writer.cpp dbus/vinput_dbus.cpp
                    input/vinput_keyevent.cpp menu/palette_command.cpp menu/vinput_menu.cpp
)

set_target_properties(addon_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Fcitx5Core REQUIRED)
if (Fcitx5Core_VERSION VERSION_GREATER_EQUAL "5.1.12")
  target_compile_definitions(addon_core PUBLIC VINPUT_FCITX5_CORE_HAVE_ADDON_FACTORY_V2)
endif ()
if (Fcitx5Core_VERSION VERSION_GREATER_EQUAL "5.1.9")
  target_compile_definitions(addon_core PUBLIC VINPUT_FCITX5_CORE_HAVE_SET_COMMENT)
endif ()

target_include_directories(addon_core PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
  addon_core
  PUBLIC config
         Fcitx5::Core
         Fcitx5::Module::DBus
         Fcitx5::Module::Clipboard
         Fcitx5::Module::Notifications
         PkgConfig::SDBUS
         Threads::Threads
)

add_library(addon MODULE)

target_link_libraries(addon PRIVATE addon_core)
set_target_properties(addon PROPERTIES PREFIX "" OUTPUT_NAME "fcitx5-vinput")

install(TARGETS addon DESTINATION "${FCITX_INSTALL_LIBDIR}/fcitx5")

configure_file(vinput-addon.conf.in vinput-addon.conf @ONLY)
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/vinput-addon.conf"
  DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
  RENAME vinput.conf
)
