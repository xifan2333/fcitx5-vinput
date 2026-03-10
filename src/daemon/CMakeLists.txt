set(SHERPA_ONNX_DIR "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx")
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
set(VINPUT_SYSTEMD_SERVICE_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/vinput-daemon.service")
set(VINPUT_DBUS_SERVICE_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/org.fcitx.Vinput.service")

if(IS_ABSOLUTE "${VINPUT_RUNTIME_LIBDIR}")
    set(VINPUT_DAEMON_INSTALL_RPATH "${VINPUT_RUNTIME_LIBDIR}")
else()
    set(VINPUT_DAEMON_INSTALL_RPATH "\$ORIGIN/../${VINPUT_RUNTIME_LIBDIR}")
endif()

add_executable(vinput-daemon
    main.cpp
    audio_capture.cpp
    asr_engine.cpp
    dbus_service.cpp
    post_processor.cpp
)

target_include_directories(vinput-daemon PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PIPEWIRE_INCLUDE_DIRS}
    ${SHERPA_ONNX_DIR}/include
)

target_link_directories(vinput-daemon PRIVATE
    ${SHERPA_ONNX_DIR}/lib
)

target_link_libraries(vinput-daemon PRIVATE
    vinput-common
    ${PIPEWIRE_LIBRARIES}
    ${SDBUS_LIBRARIES}
    CURL::libcurl
    sherpa-onnx-cxx-api
    sherpa-onnx-c-api
    pthread
)

set_target_properties(vinput-daemon PROPERTIES
    INSTALL_RPATH "${VINPUT_DAEMON_INSTALL_RPATH}"
    BUILD_RPATH "${SHERPA_ONNX_DIR}/lib"
)

configure_file("${CMAKE_SOURCE_DIR}/systemd/vinput-daemon.service.in"
               "${VINPUT_SYSTEMD_SERVICE_FILE}"
               @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/data/org.fcitx.Vinput.service.in"
               "${VINPUT_DBUS_SERVICE_FILE}"
               @ONLY)

install(TARGETS vinput-daemon DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
    "${VINPUT_SYSTEMD_SERVICE_FILE}"
    DESTINATION "${VINPUT_SYSTEMD_USER_DIR}"
)
install(FILES
    "${VINPUT_DBUS_SERVICE_FILE}"
    DESTINATION "${VINPUT_DBUS_SERVICE_DIR}"
)
install(FILES
    "${SHERPA_ONNX_DIR}/lib/libsherpa-onnx-cxx-api.so"
    "${SHERPA_ONNX_DIR}/lib/libsherpa-onnx-c-api.so"
    "${SHERPA_ONNX_DIR}/lib/libonnxruntime.so"
    DESTINATION "${VINPUT_RUNTIME_LIBDIR}"
)
