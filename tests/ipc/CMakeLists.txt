# tests/ipc/CMakeLists.txt — IPC 模块测试 (UIMessageFactory, FrameCodec)
#
# UIMessageFactory: 纯静态方法，从 ui_service.cc 中编译
# FrameCodec: 使用真实 Named Pipe 进行往返测试

add_executable(ipc_tests
    ui_message_factory_test.cc
    frame_codec_test.cc
    ${CMAKE_SOURCE_DIR}/daemon-service/ipc/ui_service.cc
    ${CMAKE_SOURCE_DIR}/daemon-service/ipc/frame.cc
)

target_include_directories(ipc_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/daemon-service/ipc
)

target_link_libraries(ipc_tests PRIVATE
    clawspan_headers
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(ipc_tests)
