pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

add_library(client_runtime STATIC
    runtime/dbus_client.cpp
    runtime/systemd_client.cpp
)

set_target_properties(client_runtime PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "vinput-client-runtime"
)

target_include_directories(client_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${SDBUS_INCLUDE_DIRS}
)

target_link_libraries(client_runtime PUBLIC
    config
    ${SDBUS_LIBRARIES}
)

add_executable(cli
    main.cpp
    utils/formatter.cpp
    utils/editor_utils.cpp
    utils/resource_utils.cpp
    control/register.cpp
    control/register_daemon.cpp
    control/daemon_actions.cpp
    control/register_recording.cpp
    control/recording_actions.cpp
    config/asr_actions.cpp
    config/config_actions.cpp
    config/device_actions.cpp
    config/init_actions.cpp
    config/llm_actions.cpp
    config/register.cpp
    config/register_asr.cpp
    config/register_config.cpp
    config/register_device.cpp
    config/register_init.cpp
    config/register_llm.cpp
    config/register_scene.cpp
    config/scene_actions.cpp
)

set_target_properties(cli PROPERTIES OUTPUT_NAME "vinput")

target_include_directories(cli PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PIPEWIRE_INCLUDE_DIRS}
    ${SDBUS_INCLUDE_DIRS}
)

target_link_libraries(cli PRIVATE
    common
    pipewire_device
    client_runtime
    CLI11::CLI11
    tabulate::tabulate
    ${PIPEWIRE_LIBRARIES}
    ${SDBUS_LIBRARIES}
)

install(TARGETS cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
