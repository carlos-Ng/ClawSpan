# tests/security/filter/CMakeLists.txt — SecurityFilter unit tests
#
# SecurityFilter is normally built as a shared library (DLL).
# For testing, we compile security_filter.cc directly into the test executable.
# The CLAWSPAN_MODULE_EXPORT macro at the end of the .cc file generates a
# GetModuleInstance factory function, which is harmless in a static link context.

add_executable(security_filter_tests
    security_filter_test.cc
    ${CMAKE_SOURCE_DIR}/daemon-service/security/filter/security_filter.cc
)

target_include_directories(security_filter_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/daemon-service/security/filter
)

target_link_libraries(security_filter_tests PRIVATE
    clawspan_core
    clawspan_headers
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(security_filter_tests)
