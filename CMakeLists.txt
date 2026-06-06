cmake_minimum_required(VERSION 3.20)
project(hyperliquid-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00)  # Windows 10
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

find_package(slick-net 2.1.0 CONFIG QUIET)
if (NOT slick-net_FOUND)
    message(STATUS "Fetching slick-net...")
    FetchContent_Declare(
        slick-net
        GIT_REPOSITORY https://github.com/SlickQuant/slick-net.git
        GIT_TAG v2.1.0
    )
    FetchContent_MakeAvailable(slick-net)
else()
    message(STATUS "Found slick-net: ${slick-net_DIR}")
endif()

add_library(hyperliquid
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

target_link_libraries(hyperliquid PUBLIC
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    slick::net
)

add_executable(basic_order examples/basic_order.cpp)
target_link_libraries(basic_order PRIVATE hyperliquid)

# ── Unit tests ────────────────────────────────────────────────────────────────

find_package(GTest CONFIG REQUIRED)

add_executable(hyperliquid_tests
    tests/test_keccak.cpp
    tests/test_types.cpp
    tests/test_signing.cpp
    tests/test_websocket_utils.cpp
)

target_link_libraries(hyperliquid_tests PRIVATE
    hyperliquid
    GTest::gtest
    GTest::gtest_main
)

enable_testing()
add_test(NAME hyperliquid_tests COMMAND hyperliquid_tests)

# ── Integration tests (require network access to testnet) ─────────────────────

add_executable(hyperliquid_integration_tests
    tests/test_info_integration.cpp
    tests/test_websocket_integration.cpp
)

target_link_libraries(hyperliquid_integration_tests PRIVATE
    hyperliquid
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME hyperliquid_integration_tests COMMAND hyperliquid_integration_tests)
