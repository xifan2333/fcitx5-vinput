add_library(vinput-common STATIC
    core_config.cpp
    vinput_config.cpp
    recognition_result.cpp
    postprocess_scene.cpp
    model_manager.cpp
    model_repository.cpp
    path_utils.cpp
    file_utils.cpp
    config_path.cpp
)

set_target_properties(vinput-common PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(vinput-common PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(vinput-common PUBLIC
    Fcitx5::Config
    nlohmann_json::nlohmann_json
    CURL::libcurl
    LibArchive::LibArchive
    OpenSSL::Crypto
)
