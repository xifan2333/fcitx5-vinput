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
    ${CMAKE_SOURCE_DIR}/src/addon/core/context_writer.cpp
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
    config
    Fcitx5::Core
    Fcitx5::Module::DBus
    Fcitx5::Module::Clipboard
    Fcitx5::Module::Notifications
    ${SDBUS_LIBRARIES}
    pthread
)

add_test(NAME modifier_key_tests COMMAND modifier_key_tests)
set_tests_properties(modifier_key_tests PROPERTIES
    ENVIRONMENT "XDG_CONFIG_HOME=/tmp/vinput_test_cfg"
)

add_executable(test_interpolation
    test_interpolation.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/postprocess/prompt_template.cpp
)

target_include_directories(test_interpolation PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_interpolation PRIVATE
    common
)

add_test(NAME test_interpolation COMMAND test_interpolation)

add_executable(test_command_streaming
    test_command_streaming.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/asr/backends/command_streaming_backend.cpp
)

target_include_directories(test_command_streaming PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_command_streaming PRIVATE
    common
)

add_test(NAME test_command_streaming COMMAND test_command_streaming)

add_executable(test_command_streaming_time test_command_streaming_time.cpp)
target_include_directories(test_command_streaming_time PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_command_streaming_time COMMAND test_command_streaming_time)

add_executable(test_installed_manifest
    test_installed_manifest.cpp
)

target_include_directories(test_installed_manifest PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_installed_manifest PRIVATE
    common
)

add_test(NAME test_installed_manifest COMMAND test_installed_manifest)

add_executable(test_process_utils
    test_process_utils.cpp
)

target_include_directories(test_process_utils PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_process_utils PRIVATE
    common
)

add_test(NAME test_process_utils COMMAND test_process_utils)

add_executable(test_config_migration
    test_config_migration.cpp
)

target_include_directories(test_config_migration PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(test_config_migration PRIVATE
    CMAKE_SOURCE_DIR_STR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_config_migration PRIVATE
    common
)

add_test(NAME test_config_migration COMMAND test_config_migration)

add_executable(test_unity_input_gain
    test_unity_input_gain.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/audio/audio_utils.cpp
)
target_include_directories(test_unity_input_gain PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_unity_input_gain COMMAND test_unity_input_gain)

add_executable(test_async_recognition_session
    test_async_recognition_session.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/asr/runtime/async_recognition_session.cpp
)
target_include_directories(test_async_recognition_session PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_async_recognition_session COMMAND test_async_recognition_session)

add_executable(test_startup_pcm_buffer
    test_startup_pcm_buffer.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/audio/audio_utils.cpp
)
target_include_directories(test_startup_pcm_buffer PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_startup_pcm_buffer COMMAND test_startup_pcm_buffer)

add_executable(test_audio_input_gain
    test_audio_input_gain.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/audio/audio_utils.cpp
)
target_include_directories(test_audio_input_gain PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_audio_input_gain COMMAND test_audio_input_gain)

add_executable(test_status_poll_gate test_status_poll_gate.cpp)
target_include_directories(test_status_poll_gate PRIVATE ${CMAKE_SOURCE_DIR}/src/addon)
add_test(NAME test_status_poll_gate COMMAND test_status_poll_gate)

add_executable(test_async_context_writer
    test_async_context_writer.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/core/context_writer.cpp
)
target_include_directories(test_async_context_writer PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
)
target_link_libraries(test_async_context_writer PRIVATE pthread)
add_test(NAME test_async_context_writer COMMAND test_async_context_writer)

add_executable(test_curl_connection_cache test_curl_connection_cache.cpp)
target_include_directories(test_curl_connection_cache PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_curl_connection_cache PRIVATE CURL::libcurl pthread)
add_test(NAME test_curl_connection_cache COMMAND test_curl_connection_cache)

add_executable(test_http_client test_http_client.cpp)
target_include_directories(test_http_client PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_http_client PRIVATE common pthread)
add_test(NAME test_http_client COMMAND test_http_client)

add_executable(test_downloader test_downloader.cpp)
target_include_directories(test_downloader PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_downloader PRIVATE common pthread)
add_test(NAME test_downloader COMMAND test_downloader)

add_executable(test_string_utils test_string_utils.cpp)
target_include_directories(test_string_utils PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_string_utils COMMAND test_string_utils)

add_executable(test_time_utils test_time_utils.cpp)
target_include_directories(test_time_utils PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_time_utils COMMAND test_time_utils)

add_executable(test_locale_utils test_locale_utils.cpp)
target_include_directories(test_locale_utils PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_locale_utils COMMAND test_locale_utils)
