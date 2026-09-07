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
