cmake_minimum_required(VERSION 3.16)

vinput_find_qt(
  VINPUT_QT_VERSION_MAJOR
  COMPONENTS Core Gui Widgets Network
  REQUIRED
)
set(VINPUT_GUI_QT_MAJOR
    "${VINPUT_QT_VERSION_MAJOR}"
    PARENT_SCOPE
)
message(STATUS "Building GUI with Qt ${VINPUT_QT_VERSION_MAJOR}")

find_package(Qt${VINPUT_QT_VERSION_MAJOR}LinguistTools QUIET)
set(QT_LINGUIST_FOUND ${Qt${VINPUT_QT_VERSION_MAJOR}LinguistTools_FOUND})

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
    dialogs/client_dialog.cpp
    pages/control/control_page.cpp
    pages/resources/resource_page.cpp
    pages/llm/llm_page.cpp
)

set(VINPUT_GUI_HEADERS
    app/mainwindow.h
    utils/gui_helpers.h
    utils/config_manager.h
    utils/download_worker.h
    utils/i18n_cache.h
    dialogs/client_dialog.h
    pages/control/control_page.h
    pages/resources/resource_page.h
    pages/llm/llm_page.h
)

if (VINPUT_ENABLE_LOCAL_ASR)
  list(APPEND VINPUT_GUI_SOURCES pages/hotwords/hotword_page.cpp)
  list(APPEND VINPUT_GUI_HEADERS pages/hotwords/hotword_page.h)
endif ()

add_executable(gui ${VINPUT_GUI_SOURCES} ${VINPUT_GUI_HEADERS})

set_target_properties(gui PROPERTIES OUTPUT_NAME "vinput-gui")

set(VINPUT_GUI_DESKTOP_FILE "${CMAKE_CURRENT_BINARY_DIR}/vinput-gui.desktop")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/vinput-gui.desktop.in" "${VINPUT_GUI_DESKTOP_FILE}" @ONLY
)

set(VINPUT_GUI_TS ${CMAKE_SOURCE_DIR}/translations/zh_CN.ts)

if (QT_LINGUIST_FOUND)
  if (VINPUT_QT_VERSION_MAJOR EQUAL 6)
    qt_add_translation(VINPUT_GUI_QM ${VINPUT_GUI_TS})
  else ()
    qt5_add_translation(VINPUT_GUI_QM ${VINPUT_GUI_TS})
  endif ()
else ()
  find_program(
    VINPUT_LRELEASE_EXECUTABLE NAMES lrelease-qt${VINPUT_QT_VERSION_MAJOR} lrelease-qt6
                                     lrelease-qt5 lrelease
  )
  if (VINPUT_LRELEASE_EXECUTABLE)
    set(VINPUT_GUI_QM ${CMAKE_CURRENT_BINARY_DIR}/zh_CN.qm)
    add_custom_command(
      OUTPUT ${VINPUT_GUI_QM}
      COMMAND ${VINPUT_LRELEASE_EXECUTABLE} ${VINPUT_GUI_TS} -qm ${VINPUT_GUI_QM}
      DEPENDS ${VINPUT_GUI_TS}
      VERBATIM
    )
  else ()
    message(
      WARNING
        "Neither Qt${VINPUT_QT_VERSION_MAJOR}LinguistTools nor lrelease found. GUI translations will not be built."
    )
  endif ()
endif ()

if (VINPUT_GUI_QM)
  add_custom_target(qt-translations ALL DEPENDS ${VINPUT_GUI_QM})
endif ()

target_compile_definitions(gui PRIVATE QT_NO_VERSION_TAGGING)

target_include_directories(gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(
  gui
  PRIVATE Qt${VINPUT_QT_VERSION_MAJOR}::Core
          Qt${VINPUT_QT_VERSION_MAJOR}::Gui
          Qt${VINPUT_QT_VERSION_MAJOR}::Widgets
          Qt${VINPUT_QT_VERSION_MAJOR}::Network
          common
          pulse_device
          client_runtime
)

install(TARGETS gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES ${VINPUT_GUI_DESKTOP_FILE} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

foreach (
  SIZE
  16
  22
  24
  32
  48
  64
  128
  256
  512
)
  install(FILES ${CMAKE_SOURCE_DIR}/data/icons/hicolor/${SIZE}x${SIZE}/apps/vinput-gui.png
          DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${SIZE}x${SIZE}/apps
  )
endforeach ()

if (VINPUT_GUI_QM)
  install(FILES ${VINPUT_GUI_QM}
          DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/fcitx5-vinput/translations
  )
endif ()
