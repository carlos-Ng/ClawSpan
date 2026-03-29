cmake_minimum_required(VERSION 3.20)

# ─── 项目声明 ─────────────────────────────────────────────────────────────────
# Windows 平台无 ObjC++，只需 CXX。
# 未来合并 macOS 代码时，在此添加 if(APPLE) 分支并补充 OBJCXX 语言。
project(ClawSpan VERSION 0.1.1 LANGUAGES CXX)

# ─── Global C++ standard ──────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Third-party: gRPC / Protobuf ────────────────────────────────────────────

set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_CSHARP_EXT OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_CSHARP_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_NODE_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_PHP_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_PYTHON_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_RUBY_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
set(ABSL_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
if(MSVC)
    # Use a shared CRT across EXE and plugin DLLs to avoid cross-module heap issues.
    # Static CRT (/MT,/MTd) gives each module its own heap and can trigger debug_heap
    # assertions when memory crosses DLL boundaries.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /FS")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /FS")
endif()
set(CLAWSPAN_PREV_CXX_STANDARD ${CMAKE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD 17)
add_subdirectory(third_party/grpc EXCLUDE_FROM_ALL)
set(CMAKE_CXX_STANDARD ${CLAWSPAN_PREV_CXX_STANDARD})
if(MSVC AND TARGET grpc)
    set_property(TARGET grpc PROPERTY CXX_STANDARD 17)
    target_compile_options(grpc PRIVATE /std:c++17 /Z7 /FS /MP1)
endif()

set(CLAWSPAN_VM_CHANNEL_PROTO "${CMAKE_SOURCE_DIR}/proto/vm_channel.proto")
set(CLAWSPAN_VM_CHANNEL_PROTO_GEN_DIR "${CMAKE_BINARY_DIR}/generated/vm_channel")
file(MAKE_DIRECTORY "${CLAWSPAN_VM_CHANNEL_PROTO_GEN_DIR}")

set(CLAWSPAN_VM_CHANNEL_PROTO_SRCS
    "${CLAWSPAN_VM_CHANNEL_PROTO_GEN_DIR}/vm_channel.pb.cc"
    "${CLAWSPAN_VM_CHANNEL_PROTO_GEN_DIR}/vm_channel.grpc.pb.cc"
)
set(CLAWSPAN_VM_CHANNEL_PROTO_HDRS
    "${CLAWSPAN_VM_CHANNEL_PROTO_GEN_DIR}/vm_channel.pb.h"
    "${CLAWSPAN_VM_CHANNEL_PROTO_GEN_DIR}/vm_channel.grpc.pb.h"
)

add_custom_command(
    OUTPUT ${CLAWSPAN_VM_CHANNEL_PROTO_SRCS} ${CLAWSPAN_VM_CHANNEL_PROTO_HDRS}
    COMMAND protobuf::protoc
    ARGS
        "--proto_path=${CMAKE_SOURCE_DIR}/proto"
        "--cpp_out=${CLAWSPAN_VM_CHANNEL_PROTO_GEN_DIR}"
        "--grpc_out=${CLAWSPAN_VM_CHANNEL_PROTO_GEN_DIR}"
        "--plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>"
        "${CLAWSPAN_VM_CHANNEL_PROTO}"
    DEPENDS
        "${CLAWSPAN_VM_CHANNEL_PROTO}"
        protobuf::protoc
        grpc_cpp_plugin
)

add_library(clawspan_vm_channel_proto STATIC
    ${CLAWSPAN_VM_CHANNEL_PROTO_SRCS}
)
target_include_directories(clawspan_vm_channel_proto PUBLIC
    "${CLAWSPAN_VM_CHANNEL_PROTO_GEN_DIR}"
)
target_link_libraries(clawspan_vm_channel_proto PUBLIC
    grpc++
    protobuf::libprotobuf
)

# ─── Windows 特定全局设置 ─────────────────────────────────────────────────────

if(WIN32)
    # UNICODE/WIN32_LEAN_AND_MEAN 全局有效，避免各 target 重复定义
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    # Windows SDK 最低版本：Windows 10（Named Pipe + UIA 均需要）
    add_compile_definitions(_WIN32_WINNT=0x0A00)
    # 强制 MSVC 以 UTF-8 解析源文件（解决含中文注释的头文件在 GBK 代码页下的解析错误）
    add_compile_options(/utf-8 /FS)
    # Keep one shared CRT for all modules (exe + plugins + grpc deps).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

    # Bundle VC++ runtime DLLs into dist/bin for zero-install user experience.
    # This keeps the safer shared CRT model while avoiding user-side redist install.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    set(CMAKE_INSTALL_DEBUG_LIBRARIES FALSE)
    include(InstallRequiredSystemLibraries)
endif()

# ─── Third-party: header-only interface targets ───────────────────────────────

add_library(tp_nlohmann INTERFACE)
target_include_directories(tp_nlohmann INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party
)

add_library(tp_expected INTERFACE)
target_include_directories(tp_expected INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/expected/include
)

add_library(tp_spdlog INTERFACE)
target_include_directories(tp_spdlog INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
)
# 编译期日志级别下限：允许 SPDLOG_DEBUG / SPDLOG_TRACE 宏正常展开。
# 默认值 SPDLOG_LEVEL_INFO 会在预处理阶段将 debug/trace 宏置为空操作，
# 导致运行时 log_level="debug" 配置无效。
target_compile_definitions(tp_spdlog INTERFACE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
)

add_library(tp_tomlplusplus INTERFACE)
target_include_directories(tp_tomlplusplus INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/tomlplusplus/include
)

add_library(tp_jsonrpccxx INTERFACE)
target_include_directories(tp_jsonrpccxx INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/json-rpc-cxx/include
)

add_library(tp_cxxopts INTERFACE)
target_include_directories(tp_cxxopts INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/cxxopts/include
)

# Convenience bundle: all third-party headers in one target
add_library(third_party INTERFACE)
target_link_libraries(third_party INTERFACE
    tp_nlohmann
    tp_expected
    tp_spdlog
    tp_tomlplusplus
    tp_jsonrpccxx
    tp_cxxopts
)

# ─── Project public headers ───────────────────────────────────────────────────

# clawspan_headers: project-wide include/ + all third-party headers.
# Link against this target to get everything needed by most modules.
add_library(clawspan_headers INTERFACE)
target_include_directories(clawspan_headers INTERFACE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(clawspan_headers INTERFACE third_party)

add_library(clawspan_audit STATIC
    daemon-service/audit/audit_service.cc
)
target_include_directories(clawspan_audit PUBLIC
    ${CMAKE_SOURCE_DIR}/daemon-service
)
target_link_libraries(clawspan_audit PUBLIC
    clawspan_headers
)

add_library(clawspan_vm_channel STATIC
    daemon-service/vm_channel/gateway/gateway_host_server.cc
    daemon-service/vm_channel/grpc/vm_channel_grpc_server.cc
)
target_include_directories(clawspan_vm_channel PUBLIC
    ${CMAKE_SOURCE_DIR}/daemon-service
)
target_link_libraries(clawspan_vm_channel PUBLIC
    clawspan_audit
    clawspan_vm_channel_proto
    clawspan_headers
)

# ─── Core library ────────────────────────────────────────────────────────────

add_library(clawspan_core STATIC
    daemon-service/core/module_config.cc
    daemon-service/core/security_chain.cc
    daemon-service/core/module_manager.cc
    daemon-service/core/capability_service.cc
    daemon-service/core/task_registry.cc
)
target_link_libraries(clawspan_core PUBLIC clawspan_headers)

# ─── IPC library ─────────────────────────────────────────────────────────────
# 平台无关部分（帧编解码）始终编译。
# 平台相关的 IPC 实现由 if(WIN32)/if(APPLE) 分支选择，对应同一套 IpcServerInterface 抽象。

add_library(clawspan_ipc STATIC
    daemon-service/ipc/frame.cc
    daemon-service/ipc/ui_service.cc
)

if(WIN32)
    target_sources(clawspan_ipc PRIVATE
        daemon-service/ipc/windows_ipc_server.cc
        daemon-service/ipc/windows_ipc_client.cc
    )
# if(APPLE) 分支：未来合并 macOS 代码时在此添加 unix_ipc_server.cc / unix_ipc_client.cc
endif()

target_include_directories(clawspan_ipc PRIVATE ${CMAKE_SOURCE_DIR}/daemon-service/ipc)
target_link_libraries(clawspan_ipc PUBLIC clawspan_headers)

# ─── Capability modules ───────────────────────────────────────────────────────

add_subdirectory(daemon-service/capability/ax)

# ─── VMM library ─────────────────────────────────────────────────────────────

if(WIN32)
    add_subdirectory(vmm)
endif()

# ─── Security modules ─────────────────────────────────────────────────────────

add_library(security_filter SHARED
    daemon-service/security/filter/security_filter.cc
)
target_include_directories(security_filter PRIVATE
    ${CMAKE_SOURCE_DIR}/daemon-service/security/filter
)
target_link_libraries(security_filter PRIVATE clawspan_core clawspan_headers)
set_target_properties(security_filter PROPERTIES
    OUTPUT_NAME  "security_filter"
    # Keep plugin outputs in one directory for dev-mode config loading.
    LIBRARY_OUTPUT_DIRECTORY                "${CMAKE_BINARY_DIR}/daemon-service"
    RUNTIME_OUTPUT_DIRECTORY                "${CMAKE_BINARY_DIR}/daemon-service"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_BINARY_DIR}/daemon-service"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_BINARY_DIR}/daemon-service"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_BINARY_DIR}/daemon-service"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_BINARY_DIR}/daemon-service"
    LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_BINARY_DIR}/daemon-service"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_BINARY_DIR}/daemon-service"
    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/daemon-service"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/daemon-service"
)

# ─── Daemon executable ───────────────────────────────────────────────────────

add_executable(claw_span_service
    daemon-service/daemon/main.cc
    daemon-service/daemon/daemon.cc
    daemon-service/daemon/vmm_launcher.cc
)
target_include_directories(claw_span_service PRIVATE
    ${CMAKE_SOURCE_DIR}/daemon-service/daemon
    ${CMAKE_SOURCE_DIR}/daemon-service
)
target_link_libraries(claw_span_service PRIVATE
    clawspan_core
    clawspan_vm_channel
    clawspan_ipc
    clawspan_vmm
    clawspan_headers
)
target_compile_definitions(claw_span_service PRIVATE
    CLAWSPAN_VERSION="${PROJECT_VERSION}"
)
if(WIN32)
    # 方案 B：daemon 以管理员权限运行，避免 HCS RuntimeId 发现权限不足。
    target_link_options(claw_span_service PRIVATE
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
    )
endif()

# ─── UI (C# WinForms) ────────────────────────────────────────────────────────
# clawspan_ui 随 ALL_BUILD 一并构建，与 daemon 平行执行。
# dist target 会进一步 publish 为自包含单文件 exe，输出到 dist/bin/。

find_program(DOTNET_EXECUTABLE dotnet)
if(NOT DOTNET_EXECUTABLE)
    message(WARNING "dotnet not found — clawspan_ui target and UI packaging skipped")
else()
    # .NET 只接受 Debug / Release 两种配置名。
    # CMake 的 MinSizeRel / RelWithDebInfo 均映射到 Release。
    # DOTNET_CLI_UI_LANGUAGE=en-US：强制 dotnet CLI 输出英文，
    # 避免 Windows GBK 控制台下中文 UTF-8 输出乱码。
    add_custom_target(clawspan_ui ALL
        COMMAND ${CMAKE_COMMAND} -E env DOTNET_CLI_UI_LANGUAGE=en-US
                ${DOTNET_EXECUTABLE} build "${CMAKE_SOURCE_DIR}/ui"
                -c $<IF:$<CONFIG:Debug>,Debug,Release> --nologo -v quiet
        COMMENT "Building ClawSpanUI (.NET 8 WinForms)"
        VERBATIM
    )
endif()

# ─── 发布目录 dist ───────────────────────────────────────────────────────────
#
# 用于手工测试与打包：将 daemon、dll、UI、config 复制到 dist/。
# 结构：dist/bin/{claw_span_service.exe, claw_span_ui.exe}, dist/lib/*.dll, dist/config/*.toml
#
# 构建：cmake --build <build_dir> --target dist
# 运行：cd dist && .\bin\claw_span_service.exe -f -c clawspan.toml

set(CLAWSPAN_DIST_DIR "${CMAKE_SOURCE_DIR}/dist")

# 按平台设置库文件扩展名与前缀
if(WIN32)
    set(CLAWSPAN_LIB_EXT ".dll")
    set(CLAWSPAN_LIB_PREFIX "")
    set(CLAWSPAN_EXE_EXT ".exe")
# if(APPLE) 分支：未来合并 macOS 代码时在此补充 .dylib 配置
endif()

add_custom_target(dist
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CLAWSPAN_DIST_DIR}/bin"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CLAWSPAN_DIST_DIR}/modules"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CLAWSPAN_DIST_DIR}/config"
    # 可执行文件 → dist/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:claw_span_service>
        "${CLAWSPAN_DIST_DIR}/bin/claw_span_service${CLAWSPAN_EXE_EXT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:clawspan_vmm_exe>
        "${CLAWSPAN_DIST_DIR}/bin/claw_span_vmm${CLAWSPAN_EXE_EXT}"
    # 插件 DLL → dist/modules/（clawspan_dist.toml 中 module_dir = "modules"）
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:capability_ax>
        "${CLAWSPAN_DIST_DIR}/modules/${CLAWSPAN_LIB_PREFIX}capability_ax${CLAWSPAN_LIB_EXT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:security_filter>
        "${CLAWSPAN_DIST_DIR}/modules/${CLAWSPAN_LIB_PREFIX}security_filter${CLAWSPAN_LIB_EXT}"
    # 配置文件 → dist/config/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/clawspan_dist.toml"
        "${CLAWSPAN_DIST_DIR}/config/clawspan.toml"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/security_filter_rules.toml"
        "${CLAWSPAN_DIST_DIR}/config/security_filter_rules.toml"
    COMMENT "Populating dist/ (bin/exe, modules/dll, config/toml)"
    DEPENDS claw_span_service clawspan_vmm_exe capability_ax security_filter
)

if(WIN32 AND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    foreach(runtime_dll IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        add_custom_command(TARGET dist POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${runtime_dll}"
                "${CLAWSPAN_DIST_DIR}/bin"
            COMMENT "Copying VC++ runtime: ${runtime_dll}"
            VERBATIM
        )
    endforeach()
endif()

if(DOTNET_EXECUTABLE)
    add_custom_command(TARGET dist POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E env DOTNET_CLI_UI_LANGUAGE=en-US
                ${DOTNET_EXECUTABLE} publish "${CMAKE_SOURCE_DIR}/ui"
                -r win-x64 -c Release
                -p:PublishSingleFile=true -p:SelfContained=true
                --nologo -v quiet
                -o "${CLAWSPAN_DIST_DIR}/bin"
        COMMENT "Publishing ClawSpanUI to dist/bin"
        VERBATIM
    )
    add_dependencies(dist clawspan_ui)
endif()

# ─── Release 打包 ────────────────────────────────────────────────────────────
#
# 将 dist/bin/ 下所有文件打包为 clawspan-windows-<version>.zip。
# 版本约定：Git tag 带 v（如 v0.1.0），包名不带 v（如 clawspan-windows-0.1.0.zip）。
# 用法：cmake --build build --target release
#
# 输出：${CMAKE_SOURCE_DIR}/clawspan-windows-${PROJECT_VERSION}.zip
# 依赖：dist（须先运行 --target dist）

if(WIN32)
    set(CLAWSPAN_WINDOWS_ZIP
        "${CMAKE_SOURCE_DIR}/clawspan-windows-${PROJECT_VERSION}.zip"
    )

    add_custom_target(release
        COMMAND powershell -NoProfile -NonInteractive -Command
            "if ('$<CONFIG>' -ne 'Release') { Write-Host 'ERROR: release target requires --config Release.'; exit 1 }"
        COMMAND ${CMAKE_COMMAND} -E echo "Packaging ${CLAWSPAN_WINDOWS_ZIP} ..."
        COMMAND powershell -NoProfile -NonInteractive -Command
            "Compress-Archive -Path '${CLAWSPAN_DIST_DIR}/*' -DestinationPath '${CLAWSPAN_WINDOWS_ZIP}' -Force"
        COMMAND ${CMAKE_COMMAND} -E echo "Done: ${CLAWSPAN_WINDOWS_ZIP}"
        DEPENDS dist
        COMMENT "Creating clawspan-windows-${PROJECT_VERSION}.zip from dist/ (Release only)"
        VERBATIM
    )
endif()

# ─── Tests ───────────────────────────────────────────────────────────────────

option(BUILD_TESTS "Build test programs" OFF)
if(BUILD_TESTS)
    enable_testing()
    set(CLAWSPAN_LOCAL_GTEST_DIR
        "${CMAKE_SOURCE_DIR}/third_party/googletest")
    set(CLAWSPAN_LOCAL_GTEST_FALLBACK_DIR
        "${CMAKE_SOURCE_DIR}/third_party/grpc/third_party/googletest")
    set(CLAWSPAN_GTEST_READY OFF)

    if(EXISTS "${CLAWSPAN_LOCAL_GTEST_DIR}/CMakeLists.txt")
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        add_subdirectory("${CLAWSPAN_LOCAL_GTEST_DIR}"
                         "${CMAKE_BINARY_DIR}/third_party/googletest"
                         EXCLUDE_FROM_ALL)
        if(TARGET gtest AND NOT TARGET GTest::gtest)
            add_library(GTest::gtest ALIAS gtest)
        endif()
        if(TARGET gtest_main AND NOT TARGET GTest::gtest_main)
            add_library(GTest::gtest_main ALIAS gtest_main)
        endif()
        set(CLAWSPAN_GTEST_READY ON)
    elseif(EXISTS "${CLAWSPAN_LOCAL_GTEST_FALLBACK_DIR}/CMakeLists.txt")
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        add_subdirectory("${CLAWSPAN_LOCAL_GTEST_FALLBACK_DIR}"
                         "${CMAKE_BINARY_DIR}/third_party/googletest"
                         EXCLUDE_FROM_ALL)
        if(TARGET gtest AND NOT TARGET GTest::gtest)
            add_library(GTest::gtest ALIAS gtest)
        endif()
        if(TARGET gtest_main AND NOT TARGET GTest::gtest_main)
            add_library(GTest::gtest_main ALIAS gtest_main)
        endif()
        set(CLAWSPAN_GTEST_READY ON)
    else()
        find_package(GTest CONFIG QUIET)
        if(TARGET GTest::gtest AND TARGET GTest::gtest_main)
            set(CLAWSPAN_GTEST_READY ON)
        endif()
    endif()

    if(CLAWSPAN_GTEST_READY)
        # ── Test targets ─────────────────────────────────────────────────────
        add_subdirectory(tests/vmm)
        add_subdirectory(tests/core)
        add_subdirectory(tests/common)
        add_subdirectory(tests/ipc)
        add_subdirectory(tests/security/filter)
    else()
        message(WARNING
            "GoogleTest not found (and local grpc/third_party/googletest is unavailable); "
            "test targets are skipped. Provide GTestConfig.cmake or vendor googletest.")
    endif()
endif()
