find_library(SHERPA_ONNX_CXX_LIB sherpa-onnx-cxx-api REQUIRED)
find_library(SHERPA_ONNX_C_LIB sherpa-onnx-c-api REQUIRED)
find_library(ONNXRUNTIME_LIB onnxruntime REQUIRED)
find_path(SHERPA_ONNX_INCLUDE_DIR sherpa-onnx/c-api/c-api.h REQUIRED)

pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
set(VINPUT_SYSTEMD_SERVICE_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/vinput-daemon.service")
set(VINPUT_DBUS_SERVICE_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/org.fcitx.Vinput.service")

get_filename_component(SHERPA_ONNX_RUNTIME_DIR "${SHERPA_ONNX_CXX_LIB}" DIRECTORY)
if(VINPUT_RUNTIME_MODE STREQUAL "bundled")
    if(IS_ABSOLUTE "${VINPUT_RUNTIME_LIBDIR}")
        set(VINPUT_DAEMON_INSTALL_RPATH "${VINPUT_RUNTIME_LIBDIR}")
    else()
        set(VINPUT_DAEMON_INSTALL_RPATH "\$ORIGIN/../${VINPUT_RUNTIME_LIBDIR}")
    endif()
endif()

set(VINPUT_VAD_MODEL_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/fcitx5-vinput/vad")

add_executable(daemon
    main.cpp
    asr/runtime/backend_factory.cpp
    asr/runtime/recognition_session_manager.cpp
    asr/backends/sherpa_offline_backend.cpp
    asr/backends/sherpa_streaming_backend.cpp
    asr/backends/command_batch_backend.cpp
    asr/backends/command_streaming_backend.cpp
    audio/audio_capture.cpp
    audio/audio_utils.cpp
    audio/output_ducker.cpp
    asr/vad_trimmer.cpp
    remote/remote_text_service.cpp
    runtime/dbus_service.cpp
    runtime/daemon_runtime_controller.cpp
    runtime/recognition_pipeline.cpp
    postprocess/post_processor.cpp
    postprocess/prompt_template.cpp
)

set_target_properties(daemon PROPERTIES OUTPUT_NAME "vinput-daemon")

target_compile_definitions(daemon PRIVATE
    VINPUT_VAD_MODEL_PATH="${VINPUT_VAD_MODEL_DIR}/silero_vad.onnx"
)

target_include_directories(daemon PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PIPEWIRE_INCLUDE_DIRS}
    ${SHERPA_ONNX_INCLUDE_DIR}
)

target_link_libraries(daemon PRIVATE
    common
    ${PIPEWIRE_LIBRARIES}
    ${SDBUS_LIBRARIES}
    CURL::libcurl
    ${SHERPA_ONNX_CXX_LIB}
    ${SHERPA_ONNX_C_LIB}
    pthread
)

set_target_properties(daemon PROPERTIES
    BUILD_RPATH "${SHERPA_ONNX_RUNTIME_DIR}"
)
if(VINPUT_RUNTIME_MODE STREQUAL "bundled")
    set_target_properties(daemon PROPERTIES
        INSTALL_RPATH "${VINPUT_DAEMON_INSTALL_RPATH}"
    )
endif()

configure_file("${CMAKE_SOURCE_DIR}/systemd/vinput-daemon.service.in"
               "${VINPUT_SYSTEMD_SERVICE_FILE}"
               @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/data/org.fcitx.Vinput.service.in"
               "${VINPUT_DBUS_SERVICE_FILE}"
               @ONLY)

install(TARGETS daemon DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
    "${VINPUT_SYSTEMD_SERVICE_FILE}"
    DESTINATION "${VINPUT_SYSTEMD_USER_DIR}"
)
install(FILES
    "${VINPUT_DBUS_SERVICE_FILE}"
    DESTINATION "${VINPUT_DBUS_SERVICE_DIR}"
)
if(VINPUT_RUNTIME_MODE STREQUAL "bundled")
    install(FILES
        "${SHERPA_ONNX_CXX_LIB}"
        "${SHERPA_ONNX_C_LIB}"
        "${ONNXRUNTIME_LIB}"
        DESTINATION "${VINPUT_RUNTIME_LIBDIR}"
    )
endif()
# VAD model (optional — daemon gracefully degrades if missing)
if(EXISTS "${CMAKE_SOURCE_DIR}/data/vad/silero_vad.onnx")
    install(FILES "${CMAKE_SOURCE_DIR}/data/vad/silero_vad.onnx"
            DESTINATION "${VINPUT_VAD_MODEL_DIR}")
endif()
