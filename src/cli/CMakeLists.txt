find_package(CLI11 QUIET)
if(NOT CLI11_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        CLI11
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11
        GIT_TAG v2.4.2
    )
    FetchContent_MakeAvailable(CLI11)
endif()

add_executable(vinput
    main.cpp
    formatter.cpp
    progress.cpp
    editor_utils.cpp
    command_init.cpp
    command_model.cpp
    command_scene.cpp
    command_config.cpp
    command_llm.cpp
    command_daemon.cpp
    command_status.cpp
    dbus_client.cpp
    systemd_client.cpp
)

target_include_directories(vinput PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vinput PRIVATE
    vinput-common
    CLI11::CLI11
    CURL::libcurl
    ${SDBUS_LIBRARIES}
    LibArchive::LibArchive
)

install(TARGETS vinput DESTINATION ${CMAKE_INSTALL_BINDIR})
