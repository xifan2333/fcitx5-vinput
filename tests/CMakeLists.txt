find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

add_executable(test_pipewire_target
    test_pipewire_target.cpp
    ${CMAKE_SOURCE_DIR}/src/common/audio/pipewire_device.cpp
)

target_include_directories(test_pipewire_target PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PIPEWIRE_INCLUDE_DIRS}
)

target_link_libraries(test_pipewire_target PRIVATE
    ${PIPEWIRE_LIBRARIES}
)

add_test(NAME test_pipewire_target COMMAND test_pipewire_target)
