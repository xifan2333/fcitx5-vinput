add_executable(modifier-gesture-tests
    modifier_gesture_tests.cpp
    ../src/addon/input/modifier_gesture.cpp
)
target_include_directories(modifier-gesture-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/addon)
target_link_libraries(modifier-gesture-tests PRIVATE Fcitx5::Utils)
foreach(group IN ITEMS scenarios permutations)
    add_test(NAME modifier-gestures.${group} COMMAND modifier-gesture-tests ${group})
endforeach()

add_executable(hotkey-config-tests
    hotkey_config_tests.cpp
    ../src/common/config/vinput_config.cpp
)
target_include_directories(hotkey-config-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(hotkey-config-tests PRIVATE Fcitx5::Config Fcitx5::Utils ${GETTEXT_LIBRARIES})
add_test(NAME hotkey-config.defaults-bounds-and-ini-round-trip COMMAND hotkey-config-tests)

# Exercise the production addon and daemon together on a private bus, without
# opening a microphone or loading an ASR model. Reuse their build dependencies.
add_executable(cancellation-tests cancellation_tests.cpp)
foreach(component IN ITEMS addon daemon)
    get_target_property(component_sources ${component} SOURCES)
    get_target_property(component_dir ${component} SOURCE_DIR)
    foreach(source IN LISTS component_sources)
        if(NOT source STREQUAL "main.cpp")
            target_sources(cancellation-tests PRIVATE "${component_dir}/${source}")
        endif()
    endforeach()
    target_include_directories(cancellation-tests PRIVATE
        $<TARGET_PROPERTY:${component},INCLUDE_DIRECTORIES>)
    target_compile_definitions(cancellation-tests PRIVATE
        $<TARGET_PROPERTY:${component},COMPILE_DEFINITIONS>)
    target_link_libraries(cancellation-tests PRIVATE
        $<TARGET_PROPERTY:${component},LINK_LIBRARIES>)
endforeach()
find_program(DBUS_RUN_SESSION dbus-run-session)
if(NOT DBUS_RUN_SESSION)
    message(STATUS "dbus-run-session not found; skipping D-Bus cancellation tests")
    return()
endif()
foreach(scenario IN ITEMS
    recording-discard
    delayed-start
    delayed-command-start
    rejected-start
    failed-cancellation
    shortcut-scope
    modifier-routing
    hold-repeat
    postprocessing
)
    add_test(NAME cancellation.${scenario}
        COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:cancellation-tests> ${scenario})
    set_tests_properties(cancellation.${scenario} PROPERTIES TIMEOUT 15)
endforeach()
