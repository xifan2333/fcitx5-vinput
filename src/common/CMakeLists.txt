set(VINPUT_CONFIG_SOURCES
    config/core_config_defaults.cpp
    config/core_config_store.cpp
    config/core_config_normalize.cpp
    config/core_config_validate.cpp
    config/core_config_resolve.cpp
    config/core_config_json.cpp
    config/config_router.cpp
    config/config_migration.cpp
    config/vinput_config.cpp
    scene/postprocess_scene.cpp
    llm/provider_model_cache.cpp
    llm/adapter_manager.cpp
    asr/recognition_result.cpp
    asr/model_manager.cpp
    dbus/error_info.cpp
    registry/registry_i18n.cpp
    i18n.cpp
    utils/path_utils.cpp
    utils/sandbox.cpp
    utils/file_utils.cpp
    utils/interpolation.cpp
    utils/version_utils.cpp
    utils/process_utils.cpp
)

add_library(config STATIC
    ${VINPUT_CONFIG_SOURCES}
)

set(VINPUT_REGISTRY_SOURCES
    asr/bpe_vocab_exporter.cpp
    net/http_client.cpp
    registry/registry_fetch.cpp
    registry/registry_models.cpp
    registry/registry_cache.cpp
    registry/registry_scripts.cpp
    registry/installed_manifest.cpp
    utils/download_progress.cpp
    utils/downloader.cpp
)

add_library(common STATIC
    ${VINPUT_REGISTRY_SOURCES}
)

set_target_properties(common PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "vinput-common"
)

set_target_properties(config PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "vinput-config"
)

target_include_directories(common PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_include_directories(config PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(config PUBLIC
    Fcitx5::Config
    nlohmann_json::nlohmann_json
    ${GETTEXT_LIBRARIES}
    OpenSSL::Crypto
    spdlog::spdlog_header_only
    subprocess::subprocess
    nonstd::scope
    unicodelib::unicodelib
    mio::mio
    glaze::glaze
)

target_link_libraries(common PUBLIC
    config
    httplib::httplib
    LibArchive::LibArchive
    OpenSSL::Crypto
    indicators::indicators
)

pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

add_library(pipewire_device STATIC
    audio/pipewire_device.cpp
)

set_target_properties(pipewire_device PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "vinput-pipewire-device"
)

target_include_directories(pipewire_device PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${PIPEWIRE_INCLUDE_DIRS}
)

target_link_libraries(pipewire_device PUBLIC
    config
    ${PIPEWIRE_LIBRARIES}
)
