cmake_minimum_required(VERSION 3.16)

add_executable(hotkey-config-tests
    hotkey_config_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/common/config/vinput_config.cpp
)

target_include_directories(hotkey-config-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(hotkey-config-tests PRIVATE
    Fcitx5::Config
    Fcitx5::Utils
)

add_test(NAME hotkey-config-tests COMMAND hotkey-config-tests)

find_package(Fcitx5ModuleDBus REQUIRED)
find_package(Fcitx5ModuleClipboard REQUIRED)
find_package(Fcitx5ModuleNotifications REQUIRED)

add_executable(modifier-key-tests
    modifier_key_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/addon/core/vinput.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/addon/dbus/vinput_dbus.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/addon/input/vinput_keyevent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/addon/menu/palette_command.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/addon/menu/vinput_menu.cpp
)

target_include_directories(modifier-key-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/addon
)

target_link_libraries(modifier-key-tests PRIVATE
    common
    Fcitx5::Core
    Fcitx5::Module::DBus
    Fcitx5::Module::Clipboard
    Fcitx5::Module::Notifications
    ${SDBUS_LIBRARIES}
)

add_test(NAME modifier-key-tests COMMAND modifier-key-tests)
