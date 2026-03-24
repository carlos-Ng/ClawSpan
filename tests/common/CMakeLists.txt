# tests/common/CMakeLists.txt — Common 基础类型测试 (Status, Result, SafeHandle)

add_executable(common_tests
    status_test.cc
    safe_handle_test.cc
)

target_link_libraries(common_tests PRIVATE
    clawspan_headers
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(common_tests)
