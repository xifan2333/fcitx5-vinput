find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

add_executable(test_pipewire_target
    test_pipewire_target.cpp
    ${CMAKE_SOURCE_DIR}/src/common/audio/pipewire_device.cpp
)

target_include_directories(test_pipewire_target PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PIPEWIRE_INCLUDE_DIRS}
)

target_link_libraries(test_pipewire_target PRIVATE
    ${PIPEWIRE_LIBRARIES}
)

add_test(NAME test_pipewire_target COMMAND test_pipewire_target)

find_package(Fcitx5ModuleDBus REQUIRED)
find_package(Fcitx5ModuleClipboard REQUIRED)
find_package(Fcitx5ModuleNotifications REQUIRED)

add_executable(modifier_key_tests
    modifier_key_tests.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/core/vinput.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/dbus/vinput_dbus.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/input/vinput_keyevent.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/menu/palette_command.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/menu/vinput_menu.cpp
)

target_include_directories(modifier_key_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/addon
)

find_package(Fcitx5Core REQUIRED)
if(Fcitx5Core_VERSION VERSION_GREATER_EQUAL "5.1.12")
    target_compile_definitions(modifier_key_tests PRIVATE VINPUT_FCITX5_CORE_HAVE_ADDON_FACTORY_V2)
endif()
if(Fcitx5Core_VERSION VERSION_GREATER_EQUAL "5.1.9")
    target_compile_definitions(modifier_key_tests PRIVATE VINPUT_FCITX5_CORE_HAVE_SET_COMMENT)
endif()

target_link_libraries(modifier_key_tests PRIVATE
    common
    Fcitx5::Core
    Fcitx5::Module::DBus
    Fcitx5::Module::Clipboard
    Fcitx5::Module::Notifications
    ${SDBUS_LIBRARIES}
)

add_test(NAME modifier_key_tests COMMAND modifier_key_tests)
set_tests_properties(modifier_key_tests PROPERTIES
    ENVIRONMENT "XDG_CONFIG_HOME=/tmp/vinput_test_cfg"
)
