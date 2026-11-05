# cmake/ThirdParty.cmake
# Defines vendored header-only libraries under 3rdparty/ with SYSTEM INTERFACE
# to prevent external compiler warnings from polluting our build.

# CLI11
add_library(3rdparty_cli11 INTERFACE)
target_include_directories(3rdparty_cli11 SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/cli11/include
)
add_library(CLI11::CLI11 ALIAS 3rdparty_cli11)

# cpp-httplib
add_library(3rdparty_httplib INTERFACE)
target_include_directories(3rdparty_httplib SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/httplib
)
target_compile_definitions(3rdparty_httplib INTERFACE
    CPPHTTPLIB_OPENSSL_SUPPORT=1
    CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO=1
)
target_link_libraries(3rdparty_httplib INTERFACE
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
)
add_library(httplib::httplib ALIAS 3rdparty_httplib)

# subprocess
add_library(3rdparty_subprocess INTERFACE)
target_include_directories(3rdparty_subprocess SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/subprocess
)
add_library(subprocess::subprocess ALIAS 3rdparty_subprocess)

# nonstd::scope
add_library(3rdparty_scope INTERFACE)
target_include_directories(3rdparty_scope SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/scope
)
add_library(nonstd::scope ALIAS 3rdparty_scope)

# base64
add_library(3rdparty_base64 INTERFACE)
target_include_directories(3rdparty_base64 SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/base64
)
add_library(base64::base64 ALIAS 3rdparty_base64)

# unicodelib
add_library(3rdparty_unicodelib INTERFACE)
target_include_directories(3rdparty_unicodelib SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/unicodelib
)
add_library(unicodelib::unicodelib ALIAS 3rdparty_unicodelib)

# readerwriterqueue
add_library(3rdparty_readerwriterqueue INTERFACE)
target_include_directories(3rdparty_readerwriterqueue SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/readerwriterqueue
)
add_library(readerwriterqueue::readerwriterqueue ALIAS 3rdparty_readerwriterqueue)

# mio
add_library(3rdparty_mio INTERFACE)
target_include_directories(3rdparty_mio SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/mio/include
)
add_library(mio::mio ALIAS 3rdparty_mio)

# glaze
add_library(3rdparty_glaze INTERFACE)
target_include_directories(3rdparty_glaze SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/glaze/include
)
add_library(glaze::glaze ALIAS 3rdparty_glaze)

# tabulate
add_library(3rdparty_tabulate INTERFACE)
target_include_directories(3rdparty_tabulate SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/tabulate/include
)
add_library(tabulate::tabulate ALIAS 3rdparty_tabulate)

# indicators
add_library(3rdparty_indicators INTERFACE)
target_include_directories(3rdparty_indicators SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/indicators/include
)
add_library(indicators::indicators ALIAS 3rdparty_indicators)

# tl::expected
add_library(3rdparty_tl_expected INTERFACE)
target_include_directories(3rdparty_tl_expected SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/tl-expected/include
)
add_library(tl::expected ALIAS 3rdparty_tl_expected)

# fmt
add_library(3rdparty_fmt INTERFACE)
target_include_directories(3rdparty_fmt SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/fmt/include
)
target_compile_definitions(3rdparty_fmt INTERFACE FMT_HEADER_ONLY=1)
if(NOT TARGET fmt::fmt)
    add_library(fmt::fmt ALIAS 3rdparty_fmt)
endif()
if(NOT TARGET fmt::fmt-header-only)
    add_library(fmt::fmt-header-only ALIAS 3rdparty_fmt)
endif()

# spdlog
add_library(3rdparty_spdlog INTERFACE)
target_include_directories(3rdparty_spdlog SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/3rdparty/spdlog/include
)
target_compile_definitions(3rdparty_spdlog INTERFACE
    SPDLOG_HEADER_ONLY=1
    SPDLOG_FMT_EXTERNAL=1
)
target_link_libraries(3rdparty_spdlog INTERFACE 3rdparty_fmt)
add_library(spdlog::spdlog_header_only ALIAS 3rdparty_spdlog)
add_library(spdlog::spdlog ALIAS 3rdparty_spdlog)
