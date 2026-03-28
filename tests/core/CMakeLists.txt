# tests/core/CMakeLists.txt — Core module tests (TaskRegistry, SecurityChain, ModuleConfig)

add_executable(core_tests
    audit_sink_contract_test.cc
    gateway_host_server_test.cc
    task_registry_test.cc
    security_chain_test.cc
    module_config_test.cc
)

target_link_libraries(core_tests PRIVATE
    clawspan_vm_channel
    clawspan_core
    clawspan_headers
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(core_tests)
