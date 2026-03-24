#include "vmm/vsock_server.h"
#include "ipc/hvsocket_defs.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

enum class BindMode
{
	Children,
	RuntimeId,
	Wildcard,
};

GUID makeServiceId(uint32_t port)
{
	return vsockPortToServiceId(port);
}

bool startWinsock()
{
	WSADATA wsa{};
	return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

bool hasGcsRegistryEntry(const GUID& service_id)
{
	char guid[64] = {};
	std::snprintf(
	    guid, sizeof(guid),
	    "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	    service_id.Data1, service_id.Data2, service_id.Data3,
	    service_id.Data4[0], service_id.Data4[1], service_id.Data4[2], service_id.Data4[3],
	    service_id.Data4[4], service_id.Data4[5], service_id.Data4[6], service_id.Data4[7]);

	std::string path =
	    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Virtualization\\GuestCommunicationServices\\";
	path += guid;

	HKEY key = nullptr;
	LSTATUS st = ::RegOpenKeyExA(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key);
	if (st == ERROR_SUCCESS) {
		::RegCloseKey(key);
		return true;
	}
	return false;
}

bool launchWslClient(const std::string& distro, uint32_t port)
{
	// 通过 WSL 触发一次 AF_VSOCK 客户端连接到 Host(CID=2) 指定端口。
	// 这里优先使用 python3，便于与当前项目运行环境一致。
	char cmd[2048] = {};
	std::snprintf(
	    cmd, sizeof(cmd),
	    "wsl.exe -d \"%s\" -- python3 -c \"import socket; s=socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM); "
	    "s.settimeout(5); s.connect((2,%u)); s.sendall(b'clawspan-vsock-probe'); s.close()\"",
	    distro.c_str(), port);

	STARTUPINFOA si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	BOOL ok = ::CreateProcessA(
	    nullptr, cmd, nullptr, nullptr, FALSE,
	    CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	if (!ok) {
		spdlog::error("failed to launch WSL client, GetLastError={}", ::GetLastError());
		return false;
	}

	::CloseHandle(pi.hThread);
	DWORD wait = ::WaitForSingleObject(pi.hProcess, 20000);
	DWORD code = 1;
	::GetExitCodeProcess(pi.hProcess, &code);
	::CloseHandle(pi.hProcess);

	if (wait == WAIT_TIMEOUT) {
		spdlog::error("WSL client timed out");
		return false;
	}
	if (code != 0) {
		spdlog::error("WSL client exited with code {}", code);
		return false;
	}
	return true;
}

int runProbe(BindMode mode, uint32_t port, uint32_t timeout_ms,
             const std::string& distro, bool run_wsl_client)
{
	if (!startWinsock()) {
		spdlog::error("WSAStartup failed");
		return 2;
	}

	SOCKET listen_sock = ::socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
	if (listen_sock == INVALID_SOCKET) {
		spdlog::error("socket(AF_HYPERV) failed, WSAError={}", ::WSAGetLastError());
		::WSACleanup();
		return 3;
	}

	SOCKADDR_HV addr{};
	addr.Family = AF_HYPERV;
	addr.Reserved = 0;
	addr.ServiceId = makeServiceId(port);

	if (mode == BindMode::Children) {
		addr.VmId = HV_GUID_CHILDREN;
		spdlog::info("bind mode: HV_GUID_CHILDREN");
	} else if (mode == BindMode::Wildcard) {
		addr.VmId = HV_GUID_WILDCARD;
		spdlog::info("bind mode: HV_GUID_WILDCARD");
	} else {
		GUID runtime_id{};
		if (!clawspan::vmm::discoverWsl2VmId(&runtime_id)) {
			spdlog::error("discoverWsl2VmId failed (this usually requires admin privilege)");
			::closesocket(listen_sock);
			::WSACleanup();
			return 4;
		}
		addr.VmId = runtime_id;
		spdlog::info("bind mode: RuntimeId");
	}

	if (::bind(listen_sock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
		spdlog::error("bind failed, WSAError={}", ::WSAGetLastError());
		::closesocket(listen_sock);
		::WSACleanup();
		return 5;
	}
	if (::listen(listen_sock, 1) == SOCKET_ERROR) {
		spdlog::error("listen failed, WSAError={}", ::WSAGetLastError());
		::closesocket(listen_sock);
		::WSACleanup();
		return 6;
	}

	spdlog::info("listening on vsock port {}", port);

	if (run_wsl_client) {
		if (!launchWslClient(distro, port)) {
			::closesocket(listen_sock);
			::WSACleanup();
			return 7;
		}
	}

	fd_set read_fds;
	FD_ZERO(&read_fds);
	FD_SET(listen_sock, &read_fds);
	timeval tv{};
	tv.tv_sec = static_cast<long>(timeout_ms / 1000);
	tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);

	int sel = ::select(0, &read_fds, nullptr, nullptr, &tv);
	if (sel <= 0) {
		spdlog::error("timeout waiting connection ({} ms)", timeout_ms);
		::closesocket(listen_sock);
		::WSACleanup();
		return 8;
	}

	SOCKADDR_HV remote{};
	int remote_len = sizeof(remote);
	SOCKET client = ::accept(listen_sock, reinterpret_cast<SOCKADDR*>(&remote), &remote_len);
	if (client == INVALID_SOCKET) {
		spdlog::error("accept failed, WSAError={}", ::WSAGetLastError());
		::closesocket(listen_sock);
		::WSACleanup();
		return 9;
	}

	std::array<char, 128> buf{};
	int n = ::recv(client, buf.data(), static_cast<int>(buf.size()), 0);
	if (n > 0) {
		spdlog::info("probe success, received {} bytes: '{}'", n, std::string(buf.data(), buf.data() + n));
	} else {
		spdlog::warn("accepted connection but no payload read");
	}

	::closesocket(client);
	::closesocket(listen_sock);
	::WSACleanup();
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	cxxopts::Options opts("clawspan_vsock_probe", "Validate ClawSpan scheme D vsock binding");
	opts.add_options()
	    ("mode", "bind mode: children | runtimeid | wildcard",
	     cxxopts::value<std::string>()->default_value("children"))
	    ("port", "vsock port", cxxopts::value<uint32_t>()->default_value("100"))
	    ("timeout-ms", "accept timeout ms", cxxopts::value<uint32_t>()->default_value("10000"))
	    ("distro", "WSL distro used by auto probe client",
	     cxxopts::value<std::string>()->default_value("ClawSpan"))
	    ("no-wsl-client", "do not auto launch WSL client")
	    ("check-registry", "check GCS registry entry before bind")
	    ("h,help", "show help");

	auto result = opts.parse(argc, argv);
	if (result.count("help")) {
		std::printf("%s\n", opts.help().c_str());
		return 0;
	}

	const std::string mode_str = result["mode"].as<std::string>();
	const uint32_t port = result["port"].as<uint32_t>();
	const uint32_t timeout_ms = result["timeout-ms"].as<uint32_t>();
	const std::string distro = result["distro"].as<std::string>();
	const bool run_wsl_client = !result.count("no-wsl-client");

	BindMode mode = BindMode::Children;
	if (mode_str == "children") {
		mode = BindMode::Children;
	} else if (mode_str == "runtimeid") {
		mode = BindMode::RuntimeId;
	} else if (mode_str == "wildcard") {
		mode = BindMode::Wildcard;
	} else {
		spdlog::error("invalid --mode '{}', expected children|runtimeid|wildcard", mode_str);
		return 1;
	}

	if (result.count("check-registry")) {
		const GUID service_id = makeServiceId(port);
		const bool ok = hasGcsRegistryEntry(service_id);
		if (ok) {
			spdlog::info("GCS service registry entry exists for port {}", port);
		} else {
			spdlog::warn("GCS service registry entry NOT found for port {}", port);
		}
	}

	return runProbe(mode, port, timeout_ms, distro, run_wsl_client);
}
