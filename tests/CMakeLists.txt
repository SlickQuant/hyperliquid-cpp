find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(hyperliquid_tests
    test_keccak.cpp
    test_types.cpp
    test_signing.cpp
    test_exchange.cpp
    test_websocket_utils.cpp
)

target_link_libraries(hyperliquid_tests PRIVATE
    hyperliquid
    GTest::gtest
    GTest::gtest_main
)

gtest_discover_tests(hyperliquid_tests
    TEST_PREFIX unit.
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 60
    PROPERTIES
        LABELS unit
)

add_executable(hyperliquid_integration_tests
    test_info_integration.cpp
    test_websocket_integration.cpp
)

target_link_libraries(hyperliquid_integration_tests PRIVATE
    hyperliquid
    GTest::gtest
    GTest::gtest_main
)

gtest_discover_tests(hyperliquid_integration_tests
    TEST_PREFIX integration.
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 60
    PROPERTIES
        LABELS integration
)
