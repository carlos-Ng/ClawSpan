#pragma once

#include "common/error.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <memory>
#include <string_view>

namespace clawspan {
namespace vmm {

// BridgeConnectionHandler 在桥接连接建立或断开时回调。
// daemon 用它来维护 UI 上的 Channel 3 状态。
using BridgeConnectionHandler = std::function<void(bool connected)>;

// VsockBridgeServerInterface 是“字节流转发型” Channel 3 bridge 抽象。
//
// 与 legacy VsockServer 不同，这个接口不解析业务帧，
// 只负责把 VM 侧 Hyper-V socket 流量转发到宿主机本地 TCP 目标。
class VsockBridgeServerInterface
{
public:
	virtual ~VsockBridgeServerInterface() = default;

	// setVmId 设置要绑定的 WSL2 RuntimeId。
	// 对 WSL2 场景，通常必须在 start() 前绑定正确的 VM RuntimeId 才能收到连接。
	virtual void setVmId(const GUID* vm_id) = 0;

	// start 启动 bridge，并把指定 vsock 端口转发到 target_host:target_port。
	virtual Status start(uint32_t         vsock_port,
	                     std::string_view target_host,
	                     uint16_t         target_port) = 0;

	virtual void stop() = 0;
	virtual bool isRunning() const = 0;
};

// createVsockBridgeServer 创建 Windows AF_HYPERV bridge 实现实例。
std::unique_ptr<VsockBridgeServerInterface> createVsockBridgeServer(
	BridgeConnectionHandler conn_handler = nullptr);

} // namespace vmm
} // namespace clawspan
