cmake_minimum_required(VERSION 3.16)

find_package(Qt5 COMPONENTS Core Gui Widgets REQUIRED)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(VINPUT_GUI_SOURCES
    main.cpp
    mainwindow.cpp
)

set(VINPUT_GUI_HEADERS
    mainwindow.h
)

add_executable(vinput-gui
    ${VINPUT_GUI_SOURCES}
    ${VINPUT_GUI_HEADERS}
)

target_link_libraries(vinput-gui
    PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    vinput-common
)

install(TARGETS vinput-gui
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
