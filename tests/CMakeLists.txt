# tests/CMakeLists.txt Unit tests powered by vinput_add_test helper and reusable core libraries.

# 1. PipeWire device tests
vinput_add_test(test_pipewire_target LIBS pipewire_device)

# 1. Addon integration & modifier key tests
vinput_add_test(modifier_key_tests LIBS addon_core ENV "XDG_CONFIG_HOME=/tmp/vinput_test_cfg")
vinput_add_test(test_status_poll_gate)
target_include_directories(test_status_poll_gate PRIVATE ${CMAKE_SOURCE_DIR}/src/addon)

vinput_add_test(test_async_context_writer LIBS addon_core)

# 1. Daemon core component tests (linked against daemon_core static library)
vinput_add_test(test_interpolation LIBS daemon_core)
vinput_add_test(test_command_batch LIBS daemon_core)
vinput_add_test(test_command_streaming LIBS daemon_core)
vinput_add_test(test_command_streaming_time)
vinput_add_test(test_desktop_notification_sender LIBS daemon_core)
vinput_add_test(test_unity_input_gain LIBS daemon_core)
vinput_add_test(test_async_recognition_session LIBS daemon_core)
vinput_add_test(test_audio_recording_buffer LIBS daemon_core)
vinput_add_test(test_session_lifecycle_manager LIBS daemon_core)
vinput_add_test(test_runtime_state_machine LIBS daemon_core)
vinput_add_test(test_audio_input_gain LIBS daemon_core)
vinput_add_test(test_post_processor LIBS daemon_core)
vinput_add_test(test_candidate_parser LIBS daemon_core)
vinput_add_test(test_recognition_pipeline LIBS daemon_core)
vinput_add_test(test_output_ducker LIBS daemon_core)
if (VINPUT_ENABLE_LOCAL_ASR)
  vinput_add_test(test_vad_segmentation LIBS daemon_core)
  vinput_add_test(test_hotword_utils LIBS daemon_core)
endif ()

# 1. Common & Config core tests
vinput_add_test(test_installed_manifest)
vinput_add_test(test_process_utils)
vinput_add_test(test_process_session)
vinput_add_test(test_client_manager)
vinput_add_test(test_client_command)
vinput_add_test(test_client_event)
vinput_add_test(test_notification_bridge)
vinput_add_test(test_http_client)
vinput_add_test(test_downloader)
vinput_add_test(test_string_utils)
vinput_add_test(test_scope_guard)
vinput_add_test(test_expected)
vinput_add_test(test_recognition_result)
vinput_add_test(test_offline_error_format)
vinput_add_test(test_core_config_view)
vinput_add_test(test_json_access)
vinput_add_test(test_time_utils)
vinput_add_test(test_file_utils)
vinput_add_test(test_sandbox)
vinput_add_test(test_bpe_vocab_exporter)
vinput_add_test(test_model_manager)
vinput_add_test(test_registry_archive)
vinput_add_test(test_locale_utils)
vinput_add_test(test_logging)
vinput_add_test(test_audio_realtime_queue LIBS readerwriterqueue::readerwriterqueue)

# 1. CLI utility tests
vinput_add_test(
  test_editor_utils
  SOURCES
  test_editor_utils.cpp
  ${CMAKE_SOURCE_DIR}/src/cli/utils/editor_utils.cpp
  LIBS
  subprocess::subprocess
  CLI11::CLI11
)
target_include_directories(test_editor_utils PRIVATE ${CMAKE_SOURCE_DIR}/src/cli)

vinput_add_test(
  test_formatter
  SOURCES
  test_formatter.cpp
  ${CMAKE_SOURCE_DIR}/src/cli/utils/formatter.cpp
  LIBS
  unicodelib::unicodelib
  common
)
target_include_directories(test_formatter PRIVATE ${CMAKE_SOURCE_DIR}/src/cli)

# 1. Config migration test (needs CMAKE_SOURCE_DIR definition)
vinput_add_test(test_config_migration)
target_compile_definitions(test_config_migration PRIVATE CMAKE_SOURCE_DIR_STR="${CMAKE_SOURCE_DIR}")

# 1. GUI utility tests (optional on Qt6)
find_package(
  Qt6
  COMPONENTS Core
  QUIET
)
if (Qt6_FOUND)
  vinput_add_test(
    test_download_worker SOURCES test_download_worker.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/utils/download_worker.cpp LIBS Qt6::Core
  )
  target_include_directories(test_download_worker PRIVATE ${CMAKE_SOURCE_DIR}/src/gui)
  set_target_properties(test_download_worker PROPERTIES AUTOMOC ON)

  vinput_add_test(
    test_config_manager SOURCES test_config_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/utils/config_manager.cpp LIBS Qt6::Core
  )
  target_include_directories(test_config_manager PRIVATE ${CMAKE_SOURCE_DIR}/src/gui)
  set_target_properties(test_config_manager PROPERTIES AUTOMOC ON)
endif ()
