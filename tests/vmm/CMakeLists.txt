# tests/vmm/CMakeLists.txt — VMM 模块测试

add_executable(vmm_tests
    vm_manager_test.cc
    vsock_server_test.cc
)

target_include_directories(vmm_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/vmm          # 私有头（wsl_com_interfaces.h, hvsocket_defs.h）
)

target_link_libraries(vmm_tests PRIVATE
    clawspan_vmm
    clawspan_headers
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(vmm_tests)
