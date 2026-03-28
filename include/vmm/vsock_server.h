#pragma once

// vsock_server.h — Host 侧 Hyper-V socket（AF_HYPERV）服务端（VM Gateway 传输层）
//
// 监听来自 WSL2 VM 内 mcp_server.py 的 AF_VSOCK 连接。
//
// WSL2 MicroVM 的 AF_VSOCK 连接不会到达以 HV_GUID_WILDCARD / HV_GUID_CHILDREN
// 绑定的监听器。宿主机必须绑定到 WSL2 VM 的实际 RuntimeId（GUID）才能收到连接。
// 通过 setVmId() 在 start() 之前设置，若未设置则回退到 HV_GUID_WILDCARD。
//
// 上层协议：ClawSpan FrameCodec（4B 大端长度前缀 + UTF-8 JSON body）。
// 每收到完整帧，回调注册的 FrameHandler。
//
// 架构：
//   - 1 个 accept 线程：循环 accept，将新连接投入工作线程。
//   - 每个连接独立 1 个工作线程（连接数通常极少，per-thread 模型足够）。

#include "common/error.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <memory>
#include <string>

namespace clawspan {
namespace vmm {

// ── 前向声明 ──────────────────────────────────────────────────────────────────

struct VsockConnectionImpl;

// ── VsockConnection ───────────────────────────────────────────────────────────

// VsockConnection 代表一条已建立的 VM→Host vsock 连接。
// 由 FrameHandler 持有，用于向 VM 发送响应帧。
class VsockConnection
{
public:
	explicit VsockConnection(std::shared_ptr<VsockConnectionImpl> impl);
	~VsockConnection() = default;

	// sendFrame 将 JSON 字符串以 FrameCodec 格式（4B 大端长度前缀 + body）发送。
	//
	// 入参:
	// - json: 响应 JSON 字符串（UTF-8）
	//
	// 出参/返回:
	// - Status::Ok():      发送成功
	// - Status(IO_ERROR):  连接已断开或 send 失败
	Status sendFrame(const std::string& json);

	// isAlive 返回连接是否仍处于活跃状态。
	bool isAlive() const;

	// close 主动关闭连接。
	void close();

private:
	std::shared_ptr<VsockConnectionImpl> impl_;
};

// ── FrameHandler ──────────────────────────────────────────────────────────────

// FrameHandler — 每收到一个完整 FrameCodec 帧时被调用。
//
// 入参:
// - conn: 当前连接（可通过 sendFrame 发送响应）
// - json: 已解码的 JSON 字符串（UTF-8）
//
// 调用线程：工作线程（每个连接独立线程），需保证线程安全。
using FrameHandler = std::function<void(VsockConnection& conn,
                                        const std::string& json)>;

// ConnectionHandler — 连接建立/断开时被调用。
//
// 入参:
// - connected: true 表示新连接已建立，false 表示连接已断开。
//
// 调用线程：工作线程，需保证线程安全。
using ConnectionHandler = std::function<void(bool connected)>;

// ── VsockServerInterface ──────────────────────────────────────────────────────

// VsockServerInterface 定义 VM Gateway vsock 服务端的抽象接口。
// 通过 createVsockServer() 工厂函数获取实现实例。
class VsockServerInterface
{
public:
	virtual ~VsockServerInterface() = default;

	// setVmId 设置要绑定的 WSL2 VM RuntimeId。
	//
	// WSL2 MicroVM 的 AF_VSOCK 连接不会到达以 HV_GUID_WILDCARD 绑定的监听器，
	// 必须绑定到 VM 的实际 RuntimeId 才能收到连接。
	//
	// 须在 start() 之前调用；传入 nullptr 则使用 HV_GUID_WILDCARD（仅对非 WSL2 有效）。
	virtual void setVmId(const GUID* vm_id) = 0;

	// start 初始化 Winsock，创建 AF_HYPERV 监听 socket，
	// 并在后台线程启动 accept 循环。
	//
	// 入参:
	// - vsock_port: vsock 端口号（计划值 100，由 daemon config 配置）
	//
	// 出参/返回:
	// - Status::Ok():      成功启动
	// - Status(IO_ERROR):  Winsock 初始化或 socket 绑定失败
	virtual Status start(uint32_t vsock_port) = 0;

	// stop 停止 accept 循环，关闭所有活跃连接，等待工作线程退出。
	virtual void stop() = 0;

	// isRunning 返回服务端当前是否正在运行。
	virtual bool isRunning() const = 0;
};

// discoverWsl2VmId 通过 HCS API 查找当前运行中的 WSL2 VM 的 RuntimeId。
//
// WSL2 每次启动时分配不同的 RuntimeId，此函数从 HcsEnumerateComputeSystems
// 返回的 JSON 中查找含 "WSL" 的计算系统并提取其 RuntimeId。
//
// 出参:
// - out: 成功时填充 VM 的 RuntimeId GUID
//
// 返回:
// - true:  成功找到
// - false: WSL2 VM 未运行、HCS API 不可用或需管理员权限
bool discoverWsl2VmId(GUID* out);

// createVsockServer 工厂函数，返回 Windows AF_HYPERV 实现实例。
//
// 入参:
// - handler:     每收到完整帧时的回调（FrameCodec 解帧后调用）
// - conn_handler: 连接建立/断开时的回调（可选，默认无）
//
// 出参/返回: VsockServerInterface 实例
std::unique_ptr<VsockServerInterface> createVsockServer(
	FrameHandler handler,
	ConnectionHandler conn_handler = nullptr);

} // namespace vmm
} // namespace clawspan
