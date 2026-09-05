cmake_minimum_required(VERSION 3.16)

find_package(Qt6 COMPONENTS Core Gui Widgets Network REQUIRED)
find_package(Qt6LinguistTools QUIET)
set(QT_LINGUIST_FOUND ${Qt6LinguistTools_FOUND})

pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(VINPUT_GUI_SOURCES
    app/main.cpp
    app/mainwindow.cpp
    utils/gui_helpers.cpp
    utils/config_manager.cpp
    utils/download_worker.cpp
    utils/i18n_cache.cpp
    dialogs/asr_provider_dialog.cpp
    dialogs/adapter_dialog.cpp
    pages/control/control_page.cpp
    pages/resources/resource_page.cpp
    pages/llm/llm_page.cpp
    ${CMAKE_SOURCE_DIR}/src/common/audio/pipewire_device.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/runtime/dbus_client.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/runtime/systemd_client.cpp
)

set(VINPUT_GUI_HEADERS
    app/mainwindow.h
    utils/gui_helpers.h
    utils/config_manager.h
    utils/download_worker.h
    utils/i18n_cache.h
    pages/control/control_page.h
    pages/resources/resource_page.h
    pages/llm/llm_page.h
)

if(VINPUT_ENABLE_LOCAL_ASR)
    list(APPEND VINPUT_GUI_SOURCES pages/hotwords/hotword_page.cpp)
    list(APPEND VINPUT_GUI_HEADERS pages/hotwords/hotword_page.h)
endif()

add_executable(gui
    ${VINPUT_GUI_SOURCES}
    ${VINPUT_GUI_HEADERS}
)

set_target_properties(gui PROPERTIES OUTPUT_NAME "vinput-gui")

set(VINPUT_GUI_DESKTOP_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/vinput-gui.desktop")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/vinput-gui.desktop.in"
    "${VINPUT_GUI_DESKTOP_FILE}"
    @ONLY)

set(VINPUT_GUI_TS
    ${CMAKE_SOURCE_DIR}/i18n/vinput-gui_zh_CN.ts
)

if(QT_LINGUIST_FOUND)
    qt_add_translation(VINPUT_GUI_QM ${VINPUT_GUI_TS})
else()
    find_program(VINPUT_LRELEASE_EXECUTABLE
        NAMES lrelease-qt6 lrelease
    )
    if(VINPUT_LRELEASE_EXECUTABLE)
        set(VINPUT_GUI_QM
            ${CMAKE_CURRENT_BINARY_DIR}/vinput-gui_zh_CN.qm
        )
        add_custom_command(
            OUTPUT ${VINPUT_GUI_QM}
            COMMAND ${VINPUT_LRELEASE_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/i18n/vinput-gui_zh_CN.ts
                -qm ${VINPUT_GUI_QM}
            DEPENDS ${CMAKE_SOURCE_DIR}/i18n/vinput-gui_zh_CN.ts
            VERBATIM
        )
    endif()
endif()

if(VINPUT_GUI_QM)
    add_custom_target(qt-translations ALL DEPENDS ${VINPUT_GUI_QM})
endif()

target_include_directories(gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${PIPEWIRE_INCLUDE_DIRS}
    ${SDBUS_INCLUDE_DIRS}
)

target_link_libraries(gui
    PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    common
    ${PIPEWIRE_LIBRARIES}
    ${SDBUS_LIBRARIES}
)

install(TARGETS gui
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES ${VINPUT_GUI_DESKTOP_FILE}
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
)

foreach(SIZE 16 22 24 32 48 64 128 256 512)
    install(FILES
        ${CMAKE_SOURCE_DIR}/data/icons/hicolor/${SIZE}x${SIZE}/apps/vinput-gui.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${SIZE}x${SIZE}/apps
    )
endforeach()

if(VINPUT_GUI_QM)
    install(FILES ${VINPUT_GUI_QM}
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/fcitx5-vinput/i18n
    )
endif()
