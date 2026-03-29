# ─── clawspan_vmm 静态库 ─────────────────────────────────────────────────────
#
# WSL2 VM 生命周期管理 + Hyper-V vsock 通信层。
# 源文件：
#   vm/vm_manager.cc    — WslVMManager（COM / wslapi / wsl.exe 多路径 distro 管理）
#   ipc/vsock_server.cc — WindowsVsockServer（AF_HYPERV VM Gateway 传输层）

add_library(clawspan_vmm STATIC
    vm/vm_manager.cc
    ipc/vsock_server.cc
    ipc/vsock_bridge_server.cc
)

# vmm/ 内部私有头文件（hvsocket_defs.h, wsl_com_interfaces.h）
target_include_directories(clawspan_vmm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 公共头文件（include/vmm/*.h）+ third-party（spdlog 等）
target_link_libraries(clawspan_vmm PUBLIC clawspan_headers)

# Windows 系统库
if(WIN32)
    target_link_libraries(clawspan_vmm PRIVATE
        ole32       # CoInitializeEx, CoCreateInstance（COM）
        ws2_32      # Winsock2（AF_HYPERV vsock）
        shlwapi     # SHDeleteKeyW（注册表清理）
    )
endif()

# ─── vmm.exe ────────────────────────────────────────────────────────────────
#
# 每个 WSL2 distro 对应一个 vmm.exe 实例：
#   - Watchdog 循环：定期检查 distro 状态，意外停止时自动重启
#   - VM 控制通道 IPC Client：连接 daemon Named Pipe，上报心跳/事件
#   - 接收 daemon 管理命令（启动/停止/快照等）

add_executable(clawspan_vmm_exe
    main.cc
    vmm_app.cc
)
set_target_properties(clawspan_vmm_exe PROPERTIES
    OUTPUT_NAME "claw_span_vmm"
)
target_include_directories(clawspan_vmm_exe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(clawspan_vmm_exe PRIVATE
    clawspan_vmm
    clawspan_ipc
    clawspan_headers
)
target_compile_definitions(clawspan_vmm_exe PRIVATE
    CLAWSPAN_VERSION="${PROJECT_VERSION}"
)

# ─── vsock D 方案验证工具 ─────────────────────────────────────────────────────
#
# 该工具用于独立验证：
# - 预注册 GCS service GUID 后，是否可通过 HV_GUID_CHILDREN 接收 WSL2 AF_VSOCK 连接
# - 若通过，daemon 可无需管理员权限运行（避免 HCS RuntimeId 发现依赖）
#
# 仅用于验证，不参与主服务运行链路。
add_executable(clawspan_vsock_probe
    tools/vsock_probe_main.cc
)
target_include_directories(clawspan_vsock_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(clawspan_vsock_probe PRIVATE
    clawspan_vmm
    clawspan_headers
)
target_compile_definitions(clawspan_vsock_probe PRIVATE
    CLAWSPAN_VERSION="${PROJECT_VERSION}"
)
