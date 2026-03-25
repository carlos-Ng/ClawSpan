#include "daemon.h"

#include "channel3/channel3_grpc_server.h"
#include "common/log.h"
#include "core/base/service.h"
#include "core/task_registry.h"
#include "ipc/ui_service.h"
#include "ipc/windows_ipc_server.h"
#include "vmm/vsock_bridge_server.h"
#include "vmm/vsock_server.h"
#include "vmm_launcher.h"

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  error "Platform not supported: add POSIX sys/socket.h/sys/un.h implementation here"
#endif

namespace clawspan {
namespace daemon {

#ifdef _WIN32
namespace {

// TcpEndpoint 用于把 channel3_grpc.host_listen 解析成 host + port。
struct TcpEndpoint
{
	std::string host;
	uint16_t    port = 0;
};

// 控制台退出事件由 Windows CtrlHandler 写入，run() 主循环阻塞等待它。
static HANDLE g_shutdown_event = INVALID_HANDLE_VALUE;

// consoleCtrlHandler 负责把 CTRL+C / 关闭窗口等事件转换为统一退出事件。
static BOOL WINAPI consoleCtrlHandler(DWORD ctrl_type)
{
	switch (ctrl_type) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		LOG_INFO("received ctrl event {}, shutting down", ctrl_type);
		if (g_shutdown_event != INVALID_HANDLE_VALUE) {
			::SetEvent(g_shutdown_event);
		}
		return TRUE;
	default:
		return FALSE;
	}
}

// parseTcpEndpoint 解析 host:port 形式的监听地址，供 gRPC bridge 启动使用。
static Result<TcpEndpoint> parseTcpEndpoint(std::string_view listen_address)
{
	const size_t colon_pos = listen_address.rfind(':');
	if (colon_pos == std::string_view::npos ||
	    colon_pos == 0 ||
	    colon_pos + 1 >= listen_address.size()) {
		return Result<TcpEndpoint>::Error(
			Status::INVALID_ARGUMENT,
			"channel3_grpc.host_listen must be in host:port form");
	}

	const std::string host(listen_address.substr(0, colon_pos));
	const std::string port_text(listen_address.substr(colon_pos + 1));

	try {
		const unsigned long parsed = std::stoul(port_text);
		if (parsed == 0 || parsed > 65535) {
			return Result<TcpEndpoint>::Error(
				Status::INVALID_ARGUMENT,
				"channel3_grpc.host_listen port is out of range");
		}
		return Result<TcpEndpoint>::Ok(TcpEndpoint{
			.host = host,
			.port = static_cast<uint16_t>(parsed),
		});
	} catch (const std::exception&) {
		return Result<TcpEndpoint>::Error(
			Status::INVALID_ARGUMENT,
			"channel3_grpc.host_listen port is invalid");
	}
}

// taskStatusToSuccess 兼容 legacy endTask 的两种写法：
// - success: bool
// - status: "success" | ...
static bool taskStatusToSuccess(const nlohmann::json& msg)
{
	if (msg.contains("success") && msg["success"].is_boolean()) {
		return msg["success"].get<bool>();
	}
	if (msg.contains("status") && msg["status"].is_string()) {
		return msg["status"].get<std::string>() == "success";
	}
	return true;
}

} // namespace
#else
#  error "Platform not supported: add POSIX signal handler implementation here"
#endif

// Implement 聚合 daemon 运行时的全部状态与子系统。
//
// 这里同时维护：
// - Channel 1：WindowsIpcServer
// - Channel 2：UIService
// - Channel 3 legacy：VsockServer
// - Channel 3 grpc：Channel3GrpcServer + VsockBridgeServer
struct Daemon::Implement
{
	core::CapabilityService  service_;
	core::TaskRegistry       task_registry_;
	ipc::WindowsIpcServer    ipc_server_;
	ipc::UIService           ui_service_;
	std::unique_ptr<vmm::VsockServerInterface>       vsock_server_;
	std::unique_ptr<channel3::Channel3GrpcServer>    channel3_grpc_server_;
	std::unique_ptr<vmm::VsockBridgeServerInterface> channel3_grpc_bridge_;
	VmmLauncher              vmm_launcher_;
	DaemonConfig             config_;
	std::atomic<bool>        running_{false};
	std::atomic<int>         channel_connection_count_{0};

	// ── 系统状态（UI status 三维模型）──────────────────────────
	std::mutex               status_mutex_;
	std::string              vm_state_      = "stopped";   // running / stopped / starting
	std::string              openclaw_state_ = "unknown";  // online / offline / unknown
	std::string              channel_state_ = "idle";      // active / idle

	std::thread              health_thread_;
	HANDLE                   health_stop_event_ = INVALID_HANDLE_VALUE;

	// buildStatusJson 构造当前状态 JSON（调用者持有 status_mutex_ 或在单线程上下文）。
	std::string buildStatusJson() const
	{
		return ipc::UIMessageFactory::createStatus(vm_state_, openclaw_state_, channel_state_);
	}

	// pushStatus 向 UI 推送最新系统状态。
	void pushStatus()
	{
		std::string json;
		std::string vm_state;
		std::string openclaw_state;
		std::string channel_state;
		{
			std::lock_guard lock(status_mutex_);
			json = buildStatusJson();
			vm_state = vm_state_;
			openclaw_state = openclaw_state_;
			channel_state = channel_state_;
		}

		LOG_INFO("daemon: pushStatus() vm={} openclaw={} channel={} connected={}",
		         vm_state, openclaw_state, channel_state,
		         ui_service_.isConnected());
		ui_service_.push(json);
	}

	// updateVmState 更新 VM 状态并推送到 UI。
	void updateVmState(const std::string& state)
	{
		{
			std::lock_guard lock(status_mutex_);
			if (vm_state_ == state) return;
			vm_state_ = state;
		}
		LOG_INFO("daemon: vm state changed to '{}'", state);
		pushStatus();
	}

	// updateOpenClawState 更新 OpenClaw 状态并推送到 UI。
	void updateOpenClawState(const std::string& state)
	{
		{
			std::lock_guard lock(status_mutex_);
			if (openclaw_state_ == state) return;
			openclaw_state_ = state;
		}
		LOG_INFO("daemon: openclaw state changed to '{}'", state);
		pushStatus();
	}

	// updateChannelState 更新 Channel 状态并推送到 UI。
	void updateChannelState(const std::string& state)
	{
		{
			std::lock_guard lock(status_mutex_);
			if (channel_state_ == state) return;
			channel_state_ = state;
		}
		LOG_INFO("daemon: channel state changed to '{}'", state);
		pushStatus();
	}

	// onChannelConnectionChanged 聚合 legacy/gRPC 两条 Channel 3 链路的连接数，
	// 避免 UI 只感知其中一条路径。
	void onChannelConnectionChanged(const char* transport_name, bool connected)
	{
		int previous = 0;
		int current = 0;
		if (connected) {
			previous = channel_connection_count_.fetch_add(1);
			current = previous + 1;
		} else {
			previous = channel_connection_count_.fetch_sub(1);
			current = previous - 1;
			if (current < 0) {
				channel_connection_count_.store(0);
				current = 0;
			}
		}

		LOG_INFO("daemon: {} connection {} (count={})",
		         transport_name,
		         connected ? "opened" : "closed",
		         current);
		updateChannelState(current > 0 ? "active" : "idle");
	}

	// probeOpenClaw 通过 TCP 连接探测 OpenClaw Gateway 是否在监听。
	// 利用 WSL2 localhost 转发，直接从 Windows 侧连接 localhost:18789。
	//
	// 返回: true 表示 TCP 连接成功（OpenClaw 在线）
	bool probeOpenClaw()
	{
		SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (sock == INVALID_SOCKET) {
			LOG_INFO("daemon: probeOpenClaw() socket() failed, err={}", ::WSAGetLastError());
			return false;
		}

		// 设置连接超时：3 秒（通过 non-blocking + select 实现）
		u_long nonblock = 1;
		::ioctlsocket(sock, FIONBIO, &nonblock);

		sockaddr_in addr = {};
		addr.sin_family      = AF_INET;
		addr.sin_port        = htons(18789);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		int ret = ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
		if (ret == 0) {
			// 立即连接成功（localhost 上可能发生）
			::closesocket(sock);
			return true;
		}

		int err = ::WSAGetLastError();
		if (err != WSAEWOULDBLOCK) {
			// 立即失败（非 WOULDBLOCK 错误）
			::closesocket(sock);
			return false;
		}

		// 等待连接完成（最多 3 秒）
		// Windows 注意：连接失败时 socket 会出现在 except_fds 中，
		// 不传 except_fds 会导致 select() 一直等到超时。
		fd_set write_fds, except_fds;
		FD_ZERO(&write_fds);
		FD_ZERO(&except_fds);
		FD_SET(sock, &write_fds);
		FD_SET(sock, &except_fds);
		timeval tv = {3, 0};

		ret = ::select(0, nullptr, &write_fds, &except_fds, &tv);
		if (ret <= 0) {
			// 超时或 select 错误
			::closesocket(sock);
			return false;
		}

		// 连接出现在 except_fds 中意味着连接失败
		if (FD_ISSET(sock, &except_fds)) {
			::closesocket(sock);
			return false;
		}

		// 连接出现在 write_fds 中，再检查 SO_ERROR 确认
		if (FD_ISSET(sock, &write_fds)) {
			int so_error = 0;
			int len = sizeof(so_error);
			::getsockopt(sock, SOL_SOCKET, SO_ERROR,
			             reinterpret_cast<char*>(&so_error), &len);
			::closesocket(sock);
			return so_error == 0;
		}

		::closesocket(sock);
		return false;
	}

	// startVmBoundChannel3Servers 在已获取到 WSL2 RuntimeId 后启动 Host 侧 Channel 3。
	// 这里会同时处理：
	// - legacy vsock server
	// - gRPC vsock bridge
	void startVmBoundChannel3Servers()
	{
		const bool need_legacy = (vsock_server_ != nullptr && !vsock_server_->isRunning());
		const bool need_grpc_bridge =
			(channel3_grpc_bridge_ != nullptr && !channel3_grpc_bridge_->isRunning());
		if (!need_legacy && !need_grpc_bridge) {
			return;
		}

		GUID vm_guid{};
		if (!vmm::discoverWsl2VmId(&vm_guid)) {
			LOG_WARN("daemon: could not discover WSL2 VM RuntimeId, channel 3 vsock listeners remain unavailable");
			return;
		}

		if (need_legacy) {
			vsock_server_->setVmId(&vm_guid);
			auto st = vsock_server_->start(config_.vsock_port);
			if (st.ok()) {
				LOG_INFO("daemon: legacy channel3 vsock server started on port {}", config_.vsock_port);
			} else {
				LOG_WARN("daemon: legacy channel3 vsock server start failed: {}", st.message);
			}
		}

		if (need_grpc_bridge) {
			auto endpoint = parseTcpEndpoint(config_.channel3_grpc_host_listen);
			if (endpoint.failure()) {
				LOG_WARN("daemon: channel3 grpc bridge config invalid: {}", endpoint.error().message);
				return;
			}

			channel3_grpc_bridge_->setVmId(&vm_guid);
			auto st = channel3_grpc_bridge_->start(
				config_.channel3_grpc_vsock_port,
				endpoint.value().host,
				endpoint.value().port);
			if (st.ok()) {
				LOG_INFO("daemon: channel3 grpc bridge started on vsock port {} -> {}:{}",
				         config_.channel3_grpc_vsock_port,
				         endpoint.value().host,
				         endpoint.value().port);
			} else {
				LOG_WARN("daemon: channel3 grpc bridge start failed: {}", st.message);
			}
		}
	}

	// startHealthThread 负责延迟确认 VM 状态，并周期性探测 OpenClaw。
	// 之所以把 Channel 3 启动放在这里，是因为 WSL2 RuntimeId 往往需要等 VM 真正起来后才能拿到。
	void startHealthThread(const std::string& /*distro_name*/)
	{
		health_stop_event_ = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);

		health_thread_ = std::thread([this]() {
			LOG_INFO("daemon: health thread started");

			// 第一阶段：等 5 秒后确认 VM 状态
			if (::WaitForSingleObject(health_stop_event_, 5000) == WAIT_OBJECT_0) {
				LOG_INFO("daemon: health thread stopped during VM wait");
				return;
			}
			if (vmm_launcher_.isRunning()) {
				updateVmState("running");
			}

			startVmBoundChannel3Servers();

			// 第二阶段：周期探测 OpenClaw Gateway 端口（每 10 秒）
			for (;;) {
				const bool online = probeOpenClaw();
				LOG_INFO("daemon: openclaw probe result: {}", online ? "online" : "offline");
				updateOpenClawState(online ? "online" : "offline");

				// 可中断的 10 秒等待
				if (::WaitForSingleObject(health_stop_event_, 10000) == WAIT_OBJECT_0) {
					break;
				}
			}

			LOG_INFO("daemon: health check thread exiting");
		});
	}

	// stopHealthThread 停止健康探测线程。
	void stopHealthThread()
	{
		if (health_stop_event_ != INVALID_HANDLE_VALUE) {
			::SetEvent(health_stop_event_);
		}
		if (health_thread_.joinable()) {
			health_thread_.join();
		}
		if (health_stop_event_ != INVALID_HANDLE_VALUE) {
			::CloseHandle(health_stop_event_);
			health_stop_event_ = INVALID_HANDLE_VALUE;
		}
	}

	// beginTask / endTask / callCapability 是 Channel 1、legacy Channel 3、
	// gRPC Channel 3 三条入口共用的业务逻辑，避免行为漂移。
	std::string beginTask(const std::string& description,
	                      const std::string& root_description,
	                      const std::string& parent_task_id,
	                      const std::string& session_id)
	{
		const std::string task_id = task_registry_.beginTask(
			description,
			root_description,
			parent_task_id,
			session_id);

		const auto* task_ctx = task_registry_.findTask(task_id);
		const std::string& ui_root_desc =
			(task_ctx != nullptr && !task_ctx->root_description.empty())
				? task_ctx->root_description
				: root_description;

		ui_service_.push(ipc::UIMessageFactory::createTaskBegin(task_id, ui_root_desc));
		LOG_INFO("task begin: task_id='{}' session='{}' desc='{}'",
		         task_id,
		         session_id,
		         description);
		return task_id;
	}

	void endTask(const std::string& task_id, bool success)
	{
		task_registry_.endTask(task_id);
		ui_service_.push(ipc::UIMessageFactory::createTaskEnd(task_id));
		LOG_INFO("task end: task_id='{}' success={}", task_id, success);
	}

	Result<nlohmann::json> callCapability(const std::string& task_id,
	                                      const std::string& capability,
	                                      const std::string& operation,
	                                      const nlohmann::json& params)
	{
		LOG_DEBUG("channel3: capability '{}' op '{}' task '{}'",
		          capability,
		          operation,
		          task_id);
		return service_.callCapability(capability, operation, params, task_id);
	}

	// 配置解析与模块注册辅助函数
	static Status parseToml(DaemonConfig& config);
	static void   buildModuleConfig(const toml::table& tbl, core::ModuleSpec& spec);

	// registerHandlers 向 IPC server 注册全部处理器：
	//   - 每个 capability 的 CapabilityHandler（带 task_id）
	//   - TaskBeginHandler（创建任务，推送 UIService 事件）
	//   - TaskEndHandler（结束任务，推送 UIService 事件）
	void registerHandlers();

	// onVsockFrame — Channel 3 VsockServer 的帧回调。
	//
	// 收到 VM 发来的 FrameCodec 帧后，解析 JSON，根据 "type" 字段直接路由到
	// CapabilityService / TaskRegistry，响应通过 VsockConnection 发回 VM。
	void onVsockFrame(vmm::VsockConnection& conn, const std::string& json);
};

// ─── Implement 方法实现 ──────────────────────────────────────────────────────

Status Daemon::Implement::parseToml(DaemonConfig& config)
{
	{
		std::ifstream test(config.config_path);
		if (!test.good()) {
			LOG_DEBUG("config file '{}' not found, using defaults/cli values",
			          config.config_path);
			return Status::Ok();
		}
	}

	toml::table tbl;
	try {
		tbl = toml::parse_file(config.config_path);
	} catch (const toml::parse_error& e) {
		LOG_ERROR("TOML parse error in '{}': {}", config.config_path, e.description().data());
		return Status(Status::CONFIG_PARSE_ERROR);
	} catch (const std::exception& e) {
		LOG_ERROR("failed to read config file '{}': {}", config.config_path, e.what());
		return Status(Status::IO_ERROR);
	}

	if (const auto* daemon_tbl = tbl.get_as<toml::table>("daemon")) {
		auto fill_str = [&](const char* key, std::string& field, const char* default_val) {
			if (field == default_val) {
				if (auto v = daemon_tbl->get_as<std::string>(key)) {
					field = **v;
				}
			}
		};
		auto fill_int = [&](const char* key, int& field, int default_val) {
			if (field == default_val) {
				if (auto v = daemon_tbl->get_as<int64_t>(key)) {
					field = static_cast<int>(**v);
				}
			}
		};

		fill_str("socket_path",      config.socket_path,      DaemonConfig::DEFAULT_SOCKET_PATH);
		fill_str("log_level",        config.log_level,        DaemonConfig::DEFAULT_LOG_LEVEL);
		fill_str("module_dir",       config.module_dir,       DaemonConfig::DEFAULT_MODULE_DIR);
		fill_int("thread_pool_size", config.thread_pool_size, DaemonConfig::DEFAULT_THREAD_POOL_SIZE);
	}

	if (const auto* ui_tbl = tbl.get_as<toml::table>("ui")) {
		auto fill_str = [&](const char* key, std::string& field, const char* default_val) {
			if (field == default_val) {
				if (auto v = ui_tbl->get_as<std::string>(key)) {
					field = **v;
				}
			}
		};
		auto fill_int = [&](const char* key, int& field, int default_val) {
			if (field == default_val) {
				if (auto v = ui_tbl->get_as<int64_t>(key)) {
					field = static_cast<int>(**v);
				}
			}
		};
		fill_str("pipe_path",    config.ui_pipe_path,    DaemonConfig::DEFAULT_UI_PIPE_PATH);
		fill_str("timeout_mode", config.ui_timeout_mode, DaemonConfig::DEFAULT_UI_TIMEOUT_MODE);
		fill_int("timeout_secs", config.ui_timeout_secs, DaemonConfig::DEFAULT_UI_TIMEOUT_SECS);
	}

	if (const auto* vsock_tbl = tbl.get_as<toml::table>("vsock")) {
		if (auto v = vsock_tbl->get_as<int64_t>("port")) {
			if (config.vsock_port == DaemonConfig::DEFAULT_VSOCK_PORT) {
				config.vsock_port = static_cast<uint32_t>(**v);
			}
		}
		if (auto v = vsock_tbl->get_as<bool>("enabled")) {
			config.vsock_enabled = **v;
		}
	}

	if (const auto* grpc_tbl = tbl.get_as<toml::table>("channel3_grpc")) {
		if (auto v = grpc_tbl->get_as<bool>("enabled")) {
			config.channel3_grpc_enabled = **v;
		}
		if (auto v = grpc_tbl->get_as<std::string>("host_listen")) {
			if (config.channel3_grpc_host_listen == DaemonConfig::DEFAULT_CHANNEL3_GRPC_HOST_LISTEN) {
				config.channel3_grpc_host_listen = **v;
			}
		}
		if (auto v = grpc_tbl->get_as<int64_t>("vsock_port")) {
			if (config.channel3_grpc_vsock_port == DaemonConfig::DEFAULT_CHANNEL3_GRPC_VSOCK_PORT) {
				config.channel3_grpc_vsock_port = static_cast<uint32_t>(**v);
			}
		}
	}

	if (const auto* vmm_tbl = tbl.get_as<toml::table>("vmm")) {
		auto fill_str = [&](const char* key, std::string& field, const char* default_val) {
			if (field == default_val) {
				if (auto v = vmm_tbl->get_as<std::string>(key)) {
					field = **v;
				}
			}
		};
		fill_str("distro_name", config.vmm_distro_name, DaemonConfig::DEFAULT_VMM_DISTRO_NAME);
		fill_str("rootfs_path", config.vmm_rootfs_path, DaemonConfig::DEFAULT_VMM_ROOTFS_PATH);
		fill_str("exe_path",    config.vmm_exe_path,    DaemonConfig::DEFAULT_VMM_EXE_PATH);

		if (auto v = vmm_tbl->get_as<bool>("auto_start")) {
			config.vmm_auto_start = **v;
		}
	}

	if (const auto* modules_arr = tbl.get_as<toml::array>("modules")) {
		for (const auto& elem : *modules_arr) {
			const auto* mod_tbl = elem.as_table();
			if (mod_tbl == nullptr) {
				continue;
			}
			const auto* name_node = mod_tbl->get_as<std::string>("name");
			if (name_node == nullptr || (*name_node)->empty()) {
				continue;
			}
			core::ModuleSpec spec;
			spec.name = **name_node;
			if (const auto* prio = mod_tbl->get_as<int64_t>("priority")) {
				spec.priority = static_cast<int>(**prio);
			}
			buildModuleConfig(*mod_tbl, spec);
			config.core.modules.push_back(std::move(spec));
		}
	}

	return Status::Ok();
}

// buildModuleConfig 把 [[modules]] 表中的简单标量字段注入 ModuleSpec.params。
void Daemon::Implement::buildModuleConfig(const toml::table& tbl, core::ModuleSpec& spec)
{
	for (const auto& [k, v] : tbl) {
		if (k == "name" || k == "priority") {
			continue;
		}
		std::string key(k.str());
		if (v.is_string())              { spec.params.set(key, std::string(**v.as_string())); }
		else if (v.is_integer())        { spec.params.set(key, **v.as_integer()); }
		else if (v.is_floating_point()) { spec.params.set(key, **v.as_floating_point()); }
		else if (v.is_boolean())        { spec.params.set(key, **v.as_boolean()); }
	}
}

// registerHandlers 把 Channel 1 的 capability/task handler 注册到 WindowsIpcServer。
void Daemon::Implement::registerHandlers()
{
	for (const auto& cap_name : service_.capabilityNames()) {
		ipc_server_.registerCapability(
		    cap_name,
		    [this, cap_name](const std::string&    task_id,
		                     const std::string&    operation,
		                     const nlohmann::json& params) -> Result<nlohmann::json> {
				return callCapability(task_id, cap_name, operation, params);
		    });
		LOG_INFO("registered capability handler: {}", cap_name);
	}

	ipc_server_.setTaskBeginHandler(
	    [this](const std::string& description,
	           const std::string& root_description,
	           const std::string& parent_task_id,
	           const std::string& session_id) -> std::string {
			return beginTask(description, root_description, parent_task_id, session_id);
		});

	ipc_server_.setTaskEndHandler(
	    [this](const std::string& task_id, bool success) {
			endTask(task_id, success);
	    });
}

// onVsockFrame 负责处理 legacy Channel 3 的三类 JSON 消息：
// - capability
// - beginTask
// - endTask
void Daemon::Implement::onVsockFrame(vmm::VsockConnection& conn, const std::string& json)
{
	nlohmann::json msg;
	try {
		msg = nlohmann::json::parse(json);
	} catch (const std::exception& e) {
		LOG_ERROR("vsock: invalid JSON from VM: {}", e.what());
		nlohmann::json err = {
			{"success", false},
			{"error_message", std::string("invalid JSON: ") + e.what()},
		};
		conn.sendFrame(err.dump());
		return;
	}

	const std::string type = msg.value("type", std::string{});

	if (type == "capability") {
		// ── capability 调用 → CapabilityService ────────────────────────────
		const std::string task_id    = msg.value("task_id",    std::string{});
		const std::string capability = msg.value("capability", std::string{});
		const std::string operation  = msg.value("operation",  std::string{});
		const nlohmann::json params  = msg.value("params",     nlohmann::json::object());
		
		LOG_DEBUG("vsock: capability '{}' op '{}' task '{}'", capability, operation, task_id);
		
		auto result = callCapability(task_id, capability, operation, params);

		nlohmann::json resp;
		if (result.success()) {
			resp = {
				{"success", true},
				{"result",  result.value()},
			};
		} else {
			resp = {
				{"success",       false},
				{"error_code",    static_cast<int>(result.error().code)},
				{"error_message", std::string(result.error().message)},
			};
		}
		conn.sendFrame(resp.dump());
		return;
	}

	if (type == "beginTask") {
		// ── 任务开始 → TaskRegistry ────────────────────────────────────────
		const std::string description      = msg.value("description",      std::string{});
		const std::string root_description = msg.value("root_description", std::string{});
		const std::string parent_task_id   = msg.value("parent_task_id",   std::string{});
		const std::string session_id       = msg.value("session_id",       std::string{});

		LOG_DEBUG("vsock: beginTask session '{}' desc '{}'", session_id, description);

		const std::string task_id = beginTask(
			description, root_description, parent_task_id, session_id);

		nlohmann::json resp = {
			{"type",    "beginTask_response"},
			{"task_id", task_id},
		};
		conn.sendFrame(resp.dump());
		return;
	}

	if (type == "endTask") {
		// ── 任务结束 → TaskRegistry ────────────────────────────────────────
		const std::string task_id = msg.value("task_id", std::string{});
		const bool success        = taskStatusToSuccess(msg);

		endTask(task_id, success);

		nlohmann::json resp = {
			{"type",    "endTask_response"},
			{"success", true},
		};
		conn.sendFrame(resp.dump());
		return;
	}

	LOG_WARN("vsock: unknown message type from VM: '{}'", type);
	nlohmann::json err = {
		{"success", false},
		{"error_message", "unknown message type: " + type},
	};
	conn.sendFrame(err.dump());
}

// ─── Daemon 公开接口 ─────────────────────────────────────────────────────────

Daemon::Daemon()
	: implement_(std::make_unique<Implement>())
{}

Daemon::~Daemon()
{
	stop();
}

Status Daemon::init(DaemonConfig config)
{
	// 1. 解析 TOML
	auto parse_status = Implement::parseToml(config);
	if (!parse_status.ok()) {
		return parse_status;
	}

	// 2. 同步 module_dir 到 core config
	config.core.module_dir = config.module_dir;

	// 3. 初始化日志系统
	log::Config log_cfg;
	log_cfg.level       = log::levelFromString(config.log_level);
	log_cfg.output_mode = config.foreground ? log::Mode::CONSOLE : log::Mode::FILE;
	log::init(log_cfg);

	LOG_INFO("daemon starting, config: {}", config.config_path);
	LOG_INFO("socket: {}, thread_pool: {}, module_dir: {}",
	         config.socket_path, config.thread_pool_size, config.module_dir);

	// 4. 初始化 CapabilityService（动态加载所有模块）
	auto init_result = implement_->service_.init(config.core);
	if (init_result.failure()) {
		LOG_ERROR("capability service init failed: {}", init_result.error().message);
		return init_result.error();
	}

	// 5. 将 TaskRegistry 注入 CapabilityService
	implement_->service_.setTaskRegistry(&implement_->task_registry_);

	// 6. 启动 UIService（Channel 2）
	{
		ipc::ConfirmTimeoutMode timeout_mode = ipc::ConfirmTimeoutMode::TIMEOUT_DENY;
		if (config.ui_timeout_mode == "wait_forever") {
			timeout_mode = ipc::ConfirmTimeoutMode::WAIT_FOREVER;
		} else if (config.ui_timeout_mode == "timeout_allow") {
			timeout_mode = ipc::ConfirmTimeoutMode::TIMEOUT_ALLOW;
		}

		auto ui_status = implement_->ui_service_.start(
		    config.ui_pipe_path,
		    timeout_mode,
		    std::chrono::seconds(config.ui_timeout_secs),
		    [this]() -> std::string {
			    std::lock_guard lock(implement_->status_mutex_);
			    return implement_->buildStatusJson();
		    });

		if (!ui_status.ok()) {
			LOG_WARN("ui service start failed: {}, UI features disabled", ui_status.message);
		} else {
			implement_->service_.setUIService(&implement_->ui_service_);
			LOG_INFO("ui service listening on {}", config.ui_pipe_path);

			// Channel 1/3 连接变更时更新 channel 状态并推送到 UI
			implement_->ipc_server_.setOnConnectionChanged([](int count) {
				LOG_INFO("daemon: channel 1 connection count changed to {}", count);
			});
		}
	}

	// 7. 注册所有 Channel 1 处理器（capability + beginTask + endTask）
	implement_->registerHandlers();

	// 8. 启动 Channel 3
	if (config.channel3_grpc_enabled) {
		implement_->channel3_grpc_server_ = std::make_unique<channel3::Channel3GrpcServer>(
			[this](const std::string& description,
			       const std::string& root_description,
			       const std::string& parent_task_id,
			       const std::string& session_id) {
				return implement_->beginTask(
					description,
					root_description,
					parent_task_id,
					session_id);
			},
			[this](const std::string& task_id, bool success) {
				implement_->endTask(task_id, success);
			},
			[this](const std::string&    task_id,
			       const std::string&    capability,
			       const std::string&    operation,
			       const nlohmann::json& params) {
				return implement_->callCapability(task_id, capability, operation, params);
			});

		auto grpc_status = implement_->channel3_grpc_server_->start(config.channel3_grpc_host_listen);
		if (!grpc_status.ok()) {
			LOG_WARN("channel3 grpc: server start failed: {}, grpc stack disabled", grpc_status.message);
			implement_->channel3_grpc_server_.reset();
		} else {
			implement_->channel3_grpc_bridge_ = vmm::createVsockBridgeServer(
				[this](bool connected) {
					implement_->onChannelConnectionChanged("channel3 grpc bridge", connected);
				});
		}
	} else {
		LOG_INFO("channel3 grpc stack disabled by config");
	}

	if (config.vsock_enabled) {
		implement_->vsock_server_ = vmm::createVsockServer(
			[this](vmm::VsockConnection& conn, const std::string& json) {
				implement_->onVsockFrame(conn, json);
			},
			[this](bool connected) {
				implement_->onChannelConnectionChanged("legacy channel3 vsock", connected);
			});
		LOG_INFO("legacy channel3 vsock server instance created");
	} else {
		LOG_INFO("legacy channel3 vsock server disabled by config");
	}

	implement_->config_ = config;
	implement_->startVmBoundChannel3Servers();

	if (config.vmm_auto_start) {
		VmmLauncherConfig vmm_cfg;
		vmm_cfg.exe_path    = config.vmm_exe_path;
		vmm_cfg.distro_name = config.vmm_distro_name;
		vmm_cfg.daemon_pipe = config.socket_path;
		vmm_cfg.log_level   = config.log_level;

		auto vmm_status = implement_->vmm_launcher_.start(std::move(vmm_cfg));
		if (!vmm_status.ok()) {
			LOG_WARN("vmm_launcher start failed: {}, VM management disabled",
			         vmm_status.message);
		} else {
			LOG_INFO("claw_span_vmm.exe started for distro '{}'", config.vmm_distro_name);
			implement_->updateVmState("starting");

			// 启动后台健康探测线程（延迟确认 VM + 周期探测 OpenClaw）
			implement_->startHealthThread(config.vmm_distro_name);
		}
	} else {
		LOG_INFO("vmm auto_start disabled by config");
	}

	return Status::Ok();
}

bool Daemon::run()
{
	const auto& config = implement_->config_;

#ifdef _WIN32
	g_shutdown_event = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (g_shutdown_event == INVALID_HANDLE_VALUE) {
		LOG_ERROR("failed to create shutdown event: {}", ::GetLastError());
		return false;
	}
	if (!::SetConsoleCtrlHandler(consoleCtrlHandler, TRUE)) {
		LOG_ERROR("failed to register console ctrl handler: {}", ::GetLastError());
		::CloseHandle(g_shutdown_event);
		g_shutdown_event = INVALID_HANDLE_VALUE;
		return false;
	}
#else
#  error "Platform not supported: add POSIX sigwait implementation here"
#endif

	auto start_status = implement_->ipc_server_.start(
	    config.socket_path, config.thread_pool_size);
	if (!start_status.ok()) {
		LOG_ERROR("ipc server start failed: {}", start_status.message);
#ifdef _WIN32
		::CloseHandle(g_shutdown_event);
		g_shutdown_event = INVALID_HANDLE_VALUE;
#endif
		return false;
	}
	LOG_INFO("daemon running, listening on {}", config.socket_path);

	implement_->running_.store(true);

#ifdef _WIN32
	::WaitForSingleObject(g_shutdown_event, INFINITE);
	LOG_INFO("shutdown event received, shutting down");
	::SetConsoleCtrlHandler(consoleCtrlHandler, FALSE);
	::CloseHandle(g_shutdown_event);
	g_shutdown_event = INVALID_HANDLE_VALUE;
#else
#  error "Platform not supported: add POSIX sigwait implementation here"
#endif

	stop();
	return true;
}

// stop 按“先后台线程、再 VM、再 Channel 3、再 Channel 1”的顺序停机，
// 尽量保持资源释放顺序清晰，避免残留连接和竞争。
void Daemon::stop()
{
	const bool was_running = implement_->running_.exchange(false);
	implement_->ui_service_.stop();

	if (!was_running &&
	    implement_->health_stop_event_ == INVALID_HANDLE_VALUE &&
	    implement_->vsock_server_ == nullptr &&
	    implement_->channel3_grpc_server_ == nullptr &&
	    implement_->channel3_grpc_bridge_ == nullptr) {
		return;
	}

	LOG_INFO("daemon stopping");

	// 先停止健康探测线程（避免探测与 vmm 停止竞争）
	implement_->stopHealthThread();
	// 停止 vmm.exe（断开对 daemon 的管理连接）
	implement_->vmm_launcher_.stop();

	if (implement_->channel3_grpc_bridge_) {
		implement_->channel3_grpc_bridge_->stop();
	}
	if (implement_->channel3_grpc_server_) {
		implement_->channel3_grpc_server_->stop();
	}
	if (implement_->vsock_server_) {
		implement_->vsock_server_->stop();
	}

	implement_->ipc_server_.stop();
	implement_->service_.release();
	implement_->channel_connection_count_.store(0);
	implement_->updateChannelState("idle");
	LOG_INFO("daemon stopped");
}

} // namespace daemon
} // namespace clawspan
