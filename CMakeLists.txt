cmake_minimum_required(VERSION 3.21)
project(hyperliquid-cpp VERSION 0.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HYPERLIQUID_BUILD_TESTS "Build hyperliquid unit and integration tests" ${PROJECT_IS_TOP_LEVEL})
option(HYPERLIQUID_BUILD_EXAMPLES "Build hyperliquid example programs" ${PROJECT_IS_TOP_LEVEL})

if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00)  # Windows 10
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

find_package(slick-net 2.1.0 CONFIG QUIET)
if (NOT slick-net_FOUND)
    message(STATUS "Fetching slick-net...")
    include(FetchContent)
    FetchContent_Declare(
        slick-net
        GIT_REPOSITORY https://github.com/SlickQuant/slick-net.git
        GIT_TAG v2.1.0
    )
    FetchContent_MakeAvailable(slick-net)
else()
    message(STATUS "Found slick-net: ${slick-net_DIR}")
endif()

add_library(hyperliquid STATIC
    src/api.cpp
    src/info.cpp
    src/exchange.cpp
    src/websocket_manager.cpp
    src/utils/signing.cpp
)

target_include_directories(hyperliquid PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(hyperliquid PUBLIC cxx_std_20)

target_link_libraries(hyperliquid PUBLIC
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    slick::net
)

if(HYPERLIQUID_BUILD_EXAMPLES)
    add_executable(basic_order examples/basic_order.cpp)
    target_link_libraries(basic_order PRIVATE hyperliquid)

    add_executable(market_data_websocket examples/market_data_websocket.cpp)
    target_link_libraries(market_data_websocket PRIVATE hyperliquid)

    add_executable(market_data_websocket_per_coin examples/market_data_websocket_per_coin.cpp)
    target_link_libraries(market_data_websocket_per_coin PRIVATE hyperliquid)
endif()

if(HYPERLIQUID_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Install ────────────────────────────────────────────────────────────────────

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS hyperliquid
    EXPORT hyperliquid-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT hyperliquid-targets
    FILE hyperliquid-targets.cmake
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/hyperliquid
)

configure_package_config_file(
    cmake/hyperliquid-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/hyperliquid-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/hyperliquid
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/hyperliquid-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/hyperliquid-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/hyperliquid-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/hyperliquid
)
