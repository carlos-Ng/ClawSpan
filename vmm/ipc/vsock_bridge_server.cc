#include "vmm/vsock_bridge_server.h"
#include "hvsocket_defs.h"

#include "common/log.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace clawspan {
namespace vmm {

namespace {

static bool relaySocketData(SOCKET src, SOCKET dst)
{
	char buffer[8192];
	for (;;) {
		int n = ::recv(src, buffer, sizeof(buffer), 0);
		if (n <= 0) {
			::shutdown(dst, SD_SEND);
			return false;
		}

		int sent = 0;
		while (sent < n) {
			int written = ::send(dst, buffer + sent, n - sent, 0);
			if (written <= 0) {
				::shutdown(src, SD_RECEIVE);
				::shutdown(dst, SD_SEND);
				return false;
			}
			sent += written;
		}
	}
}

} // namespace

class WindowsVsockBridgeServer : public VsockBridgeServerInterface
{
public:
	explicit WindowsVsockBridgeServer(BridgeConnectionHandler conn_handler)
		: conn_handler_(std::move(conn_handler))
	{}

	~WindowsVsockBridgeServer() override
	{
		stop();
	}

	void setVmId(const GUID* vm_id) override
	{
		use_specific_vm_id_ = (vm_id != nullptr);
		if (vm_id != nullptr) {
			vm_id_ = *vm_id;
		}
	}

	Status start(uint32_t         vsock_port,
	             std::string_view target_host,
	             uint16_t         target_port) override;

	void stop() override;

	bool isRunning() const override
	{
		return running_.load();
	}

private:
	bool initWinsock();
	bool createListenSocket(uint32_t vsock_port);
	SOCKET connectTarget();
	void acceptLoop();
	void bridgeConnection(SOCKET client_sock);
	void registerSocket(SOCKET sock);
	void unregisterSocket(SOCKET sock);
	void closeActiveSockets();

	BridgeConnectionHandler conn_handler_;
	GUID                    vm_id_{};
	bool                    use_specific_vm_id_ = false;
	std::string             target_host_ = "127.0.0.1";
	uint16_t                target_port_ = 0;
	SOCKET                  listen_sock_ = INVALID_SOCKET;
	std::atomic<bool>       running_{false};
	std::thread             accept_thread_;
	std::vector<std::thread> conn_threads_;
	std::mutex              threads_mutex_;
	std::unordered_set<SOCKET> active_sockets_;
	std::mutex              sockets_mutex_;
};

bool WindowsVsockBridgeServer::initWinsock()
{
	WSADATA wsa{};
	return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

bool WindowsVsockBridgeServer::createListenSocket(uint32_t vsock_port)
{
	listen_sock_ = ::socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
	if (listen_sock_ == INVALID_SOCKET) {
		LOG_ERROR("vm_channel grpc bridge: socket(AF_HYPERV) failed, WSAError={}",
		          ::WSAGetLastError());
		return false;
	}

	SOCKADDR_HV addr{};
	addr.Family = AF_HYPERV;
	addr.Reserved = 0;
	addr.VmId = use_specific_vm_id_ ? vm_id_ : HV_GUID_WILDCARD;
	addr.ServiceId = vsockPortToServiceId(vsock_port);

	if (::bind(listen_sock_,
	           reinterpret_cast<SOCKADDR*>(&addr),
	           sizeof(addr)) == SOCKET_ERROR) {
		LOG_ERROR("vm_channel grpc bridge: bind failed, WSAError={}", ::WSAGetLastError());
		::closesocket(listen_sock_);
		listen_sock_ = INVALID_SOCKET;
		return false;
	}

	if (::listen(listen_sock_, SOMAXCONN) == SOCKET_ERROR) {
		LOG_ERROR("vm_channel grpc bridge: listen failed, WSAError={}", ::WSAGetLastError());
		::closesocket(listen_sock_);
		listen_sock_ = INVALID_SOCKET;
		return false;
	}

	return true;
}

Status WindowsVsockBridgeServer::start(uint32_t         vsock_port,
                                       std::string_view target_host,
                                       uint16_t         target_port)
{
	if (running_.load()) {
		return Status(Status::INTERNAL_ERROR, "vm_channel grpc bridge is already running");
	}
	if (target_port == 0) {
		return Status(Status::INVALID_ARGUMENT, "vm_channel grpc bridge target port is invalid");
	}

	target_host_ = std::string(target_host);
	target_port_ = target_port;

	if (!initWinsock()) {
		return Status(Status::IO_ERROR, "WSAStartup failed");
	}
	if (!createListenSocket(vsock_port)) {
		::WSACleanup();
		return Status(Status::IO_ERROR, "failed to create vm_channel grpc bridge listen socket");
	}

	running_.store(true);
	accept_thread_ = std::thread(&WindowsVsockBridgeServer::acceptLoop, this);

	LOG_INFO("vm_channel grpc bridge: listening on vsock port {} -> {}:{}",
	         vsock_port, target_host_, target_port_);
	return Status::Ok();
}

void WindowsVsockBridgeServer::stop()
{
	if (!running_.exchange(false)) {
		return;
	}

	if (listen_sock_ != INVALID_SOCKET) {
		::closesocket(listen_sock_);
		listen_sock_ = INVALID_SOCKET;
	}

	closeActiveSockets();

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
	LOG_INFO("vm_channel grpc bridge: stopped");
}

SOCKET WindowsVsockBridgeServer::connectTarget()
{
	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	addrinfo* results = nullptr;
	const std::string port = std::to_string(target_port_);
	if (::getaddrinfo(target_host_.c_str(), port.c_str(), &hints, &results) != 0) {
		return INVALID_SOCKET;
	}

	SOCKET target = INVALID_SOCKET;
	for (addrinfo* it = results; it != nullptr; it = it->ai_next) {
		target = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
		if (target == INVALID_SOCKET) {
			continue;
		}
		if (::connect(target, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
			break;
		}
		::closesocket(target);
		target = INVALID_SOCKET;
	}

	::freeaddrinfo(results);
	return target;
}

void WindowsVsockBridgeServer::acceptLoop()
{
	while (running_.load()) {
		SOCKADDR_HV client_addr{};
		int         addr_len = sizeof(client_addr);
		SOCKET client = ::accept(listen_sock_,
		                         reinterpret_cast<SOCKADDR*>(&client_addr),
		                         &addr_len);
		if (client == INVALID_SOCKET) {
			break;
		}

		std::lock_guard<std::mutex> lk(threads_mutex_);
		conn_threads_.emplace_back(&WindowsVsockBridgeServer::bridgeConnection, this, client);
	}
}

void WindowsVsockBridgeServer::bridgeConnection(SOCKET client_sock)
{
	SOCKET target_sock = connectTarget();
	if (target_sock == INVALID_SOCKET) {
		LOG_WARN("vm_channel grpc bridge: failed to connect target {}:{}",
		         target_host_, target_port_);
		::closesocket(client_sock);
		return;
	}

	registerSocket(client_sock);
	registerSocket(target_sock);

	if (conn_handler_) {
		conn_handler_(true);
	}

	std::thread uplink([client_sock, target_sock]() {
		relaySocketData(client_sock, target_sock);
	});
	std::thread downlink([client_sock, target_sock]() {
		relaySocketData(target_sock, client_sock);
	});

	if (uplink.joinable()) {
		uplink.join();
	}
	if (downlink.joinable()) {
		downlink.join();
	}

	unregisterSocket(client_sock);
	unregisterSocket(target_sock);
	::closesocket(target_sock);
	::closesocket(client_sock);

	if (conn_handler_) {
		conn_handler_(false);
	}
}

void WindowsVsockBridgeServer::registerSocket(SOCKET sock)
{
	std::lock_guard<std::mutex> lk(sockets_mutex_);
	active_sockets_.insert(sock);
}

void WindowsVsockBridgeServer::unregisterSocket(SOCKET sock)
{
	std::lock_guard<std::mutex> lk(sockets_mutex_);
	active_sockets_.erase(sock);
}

void WindowsVsockBridgeServer::closeActiveSockets()
{
	std::lock_guard<std::mutex> lk(sockets_mutex_);
	for (SOCKET sock : active_sockets_) {
		::shutdown(sock, SD_BOTH);
		::closesocket(sock);
	}
	active_sockets_.clear();
}

std::unique_ptr<VsockBridgeServerInterface> createVsockBridgeServer(
	BridgeConnectionHandler conn_handler)
{
	return std::make_unique<WindowsVsockBridgeServer>(std::move(conn_handler));
}

} // namespace vmm
} // namespace clawspan
