#include "vmm/vsock_server.h"
#include "hvsocket_defs.h"

#include "common/log.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")

namespace clawspan {
namespace vmm {

// ── socket I/O 辅助函数 ─────────────────────────────────────────────────────

// sendExact 发送恰好 n 字节。
static bool sendExact(SOCKET sock, const char* data, size_t n)
{
	size_t sent = 0;
	while (sent < n) {
		int ret = ::send(sock, data + sent, static_cast<int>(n - sent), 0);
		if (ret == SOCKET_ERROR || ret == 0) {
			return false;
		}
		sent += static_cast<size_t>(ret);
	}
	return true;
}

// recvExact 接收恰好 n 字节。
static bool recvExact(SOCKET sock, uint8_t* buf, size_t n)
{
	size_t received = 0;
	while (received < n) {
		int ret = ::recv(sock,
		                 reinterpret_cast<char*>(buf + received),
		                 static_cast<int>(n - received),
		                 0);
		if (ret <= 0) {
			return false;
		}
		received += static_cast<size_t>(ret);
	}
	return true;
}

// ── VsockConnectionImpl ───────────────────────────────────────────────────────

struct VsockConnectionImpl
{
	SOCKET            sock;
	std::atomic<bool> alive;
	std::mutex        send_mutex; // 保护并发 sendFrame 调用

	explicit VsockConnectionImpl(SOCKET s)
	    : sock(s)
	    , alive(s != INVALID_SOCKET)
	{}

	~VsockConnectionImpl()
	{
		if (alive.exchange(false) && sock != INVALID_SOCKET) {
			::closesocket(sock);
			sock = INVALID_SOCKET;
		}
	}
};

// ── VsockConnection ───────────────────────────────────────────────────────────

VsockConnection::VsockConnection(std::shared_ptr<VsockConnectionImpl> impl)
    : impl_(std::move(impl))
{}

// sendFrame 以 ClawSpan FrameCodec 格式（4B 大端长度前缀 + UTF-8 JSON body）发送帧。
Status VsockConnection::sendFrame(const std::string& json)
{
	if (!impl_ || !impl_->alive) {
		return Status(Status::IO_ERROR, "vsock connection is closed");
	}

	const uint32_t payload_len = static_cast<uint32_t>(json.size());
	uint8_t        header[4];
	header[0] = static_cast<uint8_t>((payload_len >> 24) & 0xFF);
	header[1] = static_cast<uint8_t>((payload_len >> 16) & 0xFF);
	header[2] = static_cast<uint8_t>((payload_len >>  8) & 0xFF);
	header[3] = static_cast<uint8_t>( payload_len        & 0xFF);

	std::lock_guard<std::mutex> lk(impl_->send_mutex);

	// 发送 4 字节头部
	if (!sendExact(impl_->sock, reinterpret_cast<const char*>(header), 4)) {
		impl_->alive = false;
		return Status(Status::IO_ERROR, "vsock send header failed");
	}
	// 发送 payload
	if (payload_len > 0 && !sendExact(impl_->sock, json.data(), payload_len)) {
		impl_->alive = false;
		return Status(Status::IO_ERROR, "vsock send payload failed");
	}
	return Status::Ok();
}

bool VsockConnection::isAlive() const
{
	return impl_ && impl_->alive.load();
}

void VsockConnection::close()
{
	if (impl_) {
		impl_->alive = false;
		if (impl_->sock != INVALID_SOCKET) {
			::closesocket(impl_->sock);
			impl_->sock = INVALID_SOCKET;
		}
	}
}

// ── WindowsVsockServer ────────────────────────────────────────────────────────

class WindowsVsockServer : public VsockServerInterface
{
public:
	explicit WindowsVsockServer(FrameHandler handler, ConnectionHandler conn_handler)
	    : handler_(std::move(handler))
	    , conn_handler_(std::move(conn_handler))
	{}

	~WindowsVsockServer() override
	{
		stop();
	}

	void   setVmId(const GUID* vm_id) override;
	Status start(uint32_t vsock_port) override;
	void   stop() override;
	bool   isRunning() const override { return running_.load(); }

private:
	bool initWinsock();
	bool createListenSocket(uint32_t vsock_port);
	void acceptLoop();
	void handleConnection(SOCKET client_sock);

	FrameHandler          handler_;
	ConnectionHandler     conn_handler_;
	GUID                  vm_id_{};
	bool                  use_specific_vm_id_ = false;
	SOCKET                listen_sock_ = INVALID_SOCKET;
	std::atomic<bool>     running_{false};
	std::thread           accept_thread_;
	std::vector<std::thread> conn_threads_;
	std::mutex            threads_mutex_;
};

// ── WindowsVsockServer 实现 ───────────────────────────────────────────────────

void WindowsVsockServer::setVmId(const GUID* vm_id)
{
	use_specific_vm_id_ = (vm_id != nullptr);
	if (vm_id) {
		vm_id_ = *vm_id;
	}
}

bool WindowsVsockServer::initWinsock()
{
	WSADATA wsa{};
	return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

bool WindowsVsockServer::createListenSocket(uint32_t vsock_port)
{
	listen_sock_ = ::socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
	if (listen_sock_ == INVALID_SOCKET) {
		LOG_ERROR("vsock server: socket(AF_HYPERV) failed, WSAError={}", ::WSAGetLastError());
		return false;
	}

	SOCKADDR_HV addr{};
	addr.Family    = AF_HYPERV;
	addr.Reserved  = 0;
	addr.VmId      = use_specific_vm_id_ ? vm_id_ : HV_GUID_WILDCARD;
	addr.ServiceId = vsockPortToServiceId(vsock_port);

	if (use_specific_vm_id_) {
		LOG_INFO("vsock server: binding to specific VM RuntimeId");
	} else {
		LOG_WARN("vsock server: binding to HV_GUID_WILDCARD (WSL2 connections will NOT work)");
	}

	if (::bind(listen_sock_,
	           reinterpret_cast<SOCKADDR*>(&addr),
	           sizeof(addr)) == SOCKET_ERROR) {
		LOG_ERROR("vsock server: bind failed, WSAError={}", ::WSAGetLastError());
		::closesocket(listen_sock_);
		listen_sock_ = INVALID_SOCKET;
		return false;
	}

	if (::listen(listen_sock_, SOMAXCONN) == SOCKET_ERROR) {
		LOG_ERROR("vsock server: listen failed, WSAError={}", ::WSAGetLastError());
		::closesocket(listen_sock_);
		listen_sock_ = INVALID_SOCKET;
		return false;
	}
	return true;
}

Status WindowsVsockServer::start(uint32_t vsock_port)
{
	if (running_.load()) {
		return Status(Status::INTERNAL_ERROR, "vsock server is already running");
	}

	if (!initWinsock()) {
		return Status(Status::IO_ERROR, "WSAStartup failed");
	}
	if (!createListenSocket(vsock_port)) {
		::WSACleanup();
		return Status(Status::IO_ERROR, "failed to create AF_HYPERV listen socket");
	}

	running_ = true;
	accept_thread_ = std::thread(&WindowsVsockServer::acceptLoop, this);
	LOG_INFO("vsock server: listening on AF_HYPERV port {}", vsock_port);
	return Status::Ok();
}

void WindowsVsockServer::stop()
{
	if (!running_.exchange(false)) {
		return;
	}

	// 关闭监听 socket，使 accept() 立即返回错误退出循环
	if (listen_sock_ != INVALID_SOCKET) {
		::closesocket(listen_sock_);
		listen_sock_ = INVALID_SOCKET;
	}

	if (accept_thread_.joinable()) {
		accept_thread_.join();
	}

	{
		std::lock_guard<std::mutex> lk(threads_mutex_);
		for (auto& t : conn_threads_) {
			if (t.joinable()) {
				t.join();
			}
		}
		conn_threads_.clear();
	}

	::WSACleanup();
	LOG_INFO("vsock server: stopped");
}

void WindowsVsockServer::acceptLoop()
{
	while (running_.load()) {
		SOCKADDR_HV client_addr{};
		int         addr_len = sizeof(client_addr);
		SOCKET client = ::accept(listen_sock_,
		                         reinterpret_cast<SOCKADDR*>(&client_addr),
		                         &addr_len);
		if (client == INVALID_SOCKET) {
			break; // 监听 socket 已关闭或出错
		}
		LOG_INFO("vsock server: new VM connection accepted");

		std::lock_guard<std::mutex> lk(threads_mutex_);
		conn_threads_.emplace_back(&WindowsVsockServer::handleConnection,
		                           this, client);
	}
}

// handleConnection 在独立线程中处理单个 VM 连接。
// 使用 FrameCodec 协议：先读 4 字节大端长度，再读 payload。
void WindowsVsockServer::handleConnection(SOCKET client_sock)
{
	auto impl = std::make_shared<VsockConnectionImpl>(client_sock);
	VsockConnection conn(impl);

	// 通知上层：VM 连接已建立
	if (conn_handler_) {
		conn_handler_(true);
	}

	while (impl->alive.load() && running_.load()) {
		// 阶段 1：读 4 字节长度头
		uint8_t header[4]{};
		if (!recvExact(client_sock, header, 4)) {
			break;
		}

		const uint32_t payload_len =
		    (static_cast<uint32_t>(header[0]) << 24) |
		    (static_cast<uint32_t>(header[1]) << 16) |
		    (static_cast<uint32_t>(header[2]) <<  8) |
		     static_cast<uint32_t>(header[3]);

		constexpr uint32_t MAX_PAYLOAD = 4u * 1024u * 1024u; // 4 MiB
		if (payload_len > MAX_PAYLOAD) {
			LOG_WARN("vsock server: oversized payload {} bytes, closing connection", payload_len);
			break;
		}

		// 阶段 2：读 payload
		std::string json(payload_len, '\0');
		if (payload_len > 0) {
			if (!recvExact(client_sock,
			               reinterpret_cast<uint8_t*>(json.data()),
			               payload_len)) {
				break;
			}
		}

		// 调用业务回调（阻塞直至处理完成）
		handler_(conn, json);
	}

	conn.close();

	// 通知上层：VM 连接已断开
	if (conn_handler_) {
		conn_handler_(false);
	}

	LOG_INFO("vsock server: VM connection closed");
}

// ── discoverWsl2VmId ─────────────────────────────────────────────────────────

bool discoverWsl2VmId(GUID* out)
{
	if (!out) { return false; }

	HMODULE hcs = ::LoadLibraryW(L"ComputeCore.dll");
	if (!hcs) {
		LOG_WARN("vsock: ComputeCore.dll not available (requires Windows 10 2004+)");
		return false;
	}

	using HcsCreateOperation_t = void* (*)(void*, void*);
	using HcsEnumerate_t       = long (*)(const wchar_t*, void*);
	using HcsWaitResult_t      = long (*)(void*, unsigned long, wchar_t**);
	using HcsCloseOperation_t  = void (*)(void*);

	auto* pCreate = reinterpret_cast<HcsCreateOperation_t>(
	    ::GetProcAddress(hcs, "HcsCreateOperation"));
	auto* pEnum = reinterpret_cast<HcsEnumerate_t>(
	    ::GetProcAddress(hcs, "HcsEnumerateComputeSystems"));
	auto* pWait = reinterpret_cast<HcsWaitResult_t>(
	    ::GetProcAddress(hcs, "HcsWaitForOperationResult"));
	auto* pClose = reinterpret_cast<HcsCloseOperation_t>(
	    ::GetProcAddress(hcs, "HcsCloseOperation"));

	if (!pCreate || !pEnum || !pWait || !pClose) {
		LOG_WARN("vsock: HCS API functions not found in ComputeCore.dll");
		::FreeLibrary(hcs);
		return false;
	}

	void* op = pCreate(nullptr, nullptr);
	if (!op) {
		::FreeLibrary(hcs);
		return false;
	}

	long hr = pEnum(nullptr, op);
	if (hr != 0) {
		LOG_WARN("vsock: HcsEnumerateComputeSystems failed, hr=0x{:08X}", static_cast<unsigned long>(hr));
		pClose(op);
		::FreeLibrary(hcs);
		return false;
	}

	wchar_t* result_doc = nullptr;
	hr = pWait(op, 5000, &result_doc);
	pClose(op);
	if (hr != 0 || !result_doc) {
		LOG_WARN("vsock: HcsWaitForOperationResult failed or empty, hr=0x{:08X}", static_cast<unsigned long>(hr));
		::FreeLibrary(hcs);
		return false;
	}

	// wchar_t* → UTF-8 std::string
	int utf8_len = ::WideCharToMultiByte(CP_UTF8, 0, result_doc, -1, nullptr, 0, nullptr, nullptr);
	std::string json_utf8(static_cast<size_t>(utf8_len), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, result_doc, -1, json_utf8.data(), utf8_len, nullptr, nullptr);
	::LocalFree(result_doc);

	nlohmann::json systems;
	try {
		systems = nlohmann::json::parse(json_utf8);
	} catch (const nlohmann::json::parse_error& e) {
		LOG_WARN("vsock: failed to parse HCS JSON: {}", e.what());
		::FreeLibrary(hcs);
		return false;
	}

	if (!systems.is_array()) {
		LOG_WARN("vsock: HCS result is not a JSON array");
		::FreeLibrary(hcs);
		return false;
	}

	// 查找 Owner == "WSL" 且 State == "Running" 的 VM
	std::string runtime_id_str;
	for (const auto& vm : systems) {
		auto owner = vm.value("Owner", "");
		auto state = vm.value("State", "");
		if (owner == "WSL" && state == "Running") {
			runtime_id_str = vm.value("RuntimeId", "");
			break;
		}
	}

	if (runtime_id_str.empty()) {
		LOG_WARN("vsock: no running WSL VM found in HCS enumeration ({} systems total)",
		         systems.size());
		::FreeLibrary(hcs);
		return false;
	}

	// 解析 RuntimeId GUID 字符串 "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
	unsigned long  d1;
	unsigned short d2, d3;
	unsigned char  d4[8];
	bool found = (sscanf_s(runtime_id_str.c_str(),
	    "%8lx-%4hx-%4hx-%2hhx%2hhx-%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
	    &d1, &d2, &d3,
	    &d4[0], &d4[1], &d4[2], &d4[3], &d4[4], &d4[5], &d4[6], &d4[7]) == 11);

	if (found) {
		out->Data1 = d1;
		out->Data2 = d2;
		out->Data3 = d3;
		for (int i = 0; i < 8; ++i) { out->Data4[i] = d4[i]; }
		LOG_INFO("vsock: discovered WSL2 VM RuntimeId: {}", runtime_id_str);
	} else {
		LOG_WARN("vsock: failed to parse RuntimeId GUID: '{}'", runtime_id_str);
	}

	::FreeLibrary(hcs);
	return found;
}

// ── 工厂函数 ─────────────────────────────────────────────────────────────────

std::unique_ptr<VsockServerInterface> createVsockServer(
	FrameHandler handler,
	ConnectionHandler conn_handler)
{
	return std::make_unique<WindowsVsockServer>(std::move(handler),
	                                             std::move(conn_handler));
}

} // namespace vmm
} // namespace clawspan
