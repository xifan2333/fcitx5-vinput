cmake_minimum_required(VERSION 3.16)

find_package(Qt5 COMPONENTS Core Gui Widgets REQUIRED)
find_package(Qt5LinguistTools QUIET)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(VINPUT_GUI_SOURCES
    main.cpp
    mainwindow.cpp
    ${CMAKE_SOURCE_DIR}/src/common/pipewire_device.cpp
)

set(VINPUT_GUI_HEADERS
    mainwindow.h
)

add_executable(vinput-gui
    ${VINPUT_GUI_SOURCES}
    ${VINPUT_GUI_HEADERS}
)

if(Qt5LinguistTools_FOUND)
    set(VINPUT_GUI_TS
        ${CMAKE_SOURCE_DIR}/i18n/vinput-gui_zh_CN.ts
    )

    qt5_add_translation(VINPUT_GUI_QM ${VINPUT_GUI_TS})
    add_custom_target(vinput-gui-translations ALL DEPENDS ${VINPUT_GUI_QM})
endif()

target_include_directories(vinput-gui PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
)

target_link_libraries(vinput-gui
    PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    vinput-common
    ${PIPEWIRE_LIBRARIES}
)

install(TARGETS vinput-gui
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(Qt5LinguistTools_FOUND)
    install(FILES ${VINPUT_GUI_QM}
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/fcitx5-vinput/i18n
    )
endif()
