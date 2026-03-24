#include "ipc/windows_ipc_server.h"
#include "frame.h"

#include "common/log.h"
#include "common/status.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <objbase.h>

#include "pipe_security.h"

namespace clawspan {
namespace ipc {

// ─── 协议辅助 ────────────────────────────────────────────────────────────────
//
// Channel 1 type-based 消息格式（非 JSON-RPC 2.0）：
//
//   beginTask (VM→Daemon):
//     {"type":"beginTask","description":"...","root_description":"...",
//      "parent_task_id":"","session_id":"..."}
//   beginTask_response (Daemon→VM):
//     {"type":"beginTask_response","task_id":"task-1"}
//
//   endTask (VM→Daemon, 无响应):
//     {"type":"endTask","task_id":"task-1","success":true}
//
//   capability (VM→Daemon):
//     {"type":"capability","id":42,"task_id":"task-1",
//      "capability":"capability_ax","operation":"list_windows","params":{}}
//   capability_result (Daemon→VM):
//     {"type":"capability_result","id":42,"success":true,"result":{...}}
//     {"type":"capability_result","id":42,"success":false,
//      "error_code":43,"error_message":"..."}

namespace {

// makeCapabilityResultOk 构造成功的 capability_result 响应帧。
std::string makeCapabilityResultOk(int id, const nlohmann::json& result)
{
	return nlohmann::json{
		{"type",    "capability_result"},
		{"id",      id},
		{"success", true},
		{"result",  result},
	}.dump();
}

// makeCapabilityResultError 构造失败的 capability_result 响应帧。
std::string makeCapabilityResultError(int                id,
                                      Status::Code       code,
                                      const std::string& message)
{
	return nlohmann::json{
		{"type",          "capability_result"},
		{"id",            id},
		{"success",       false},
		{"error_code",    static_cast<int>(code)},
		{"error_message", message},
	}.dump();
}

} // anonymous namespace

// ─── Implement ──────────────────────────────────────────────────────────────

struct WindowsIpcServer::Implement
{
	// ── 已注册的处理器 ────────────────────────────────────────────────────────
	std::unordered_map<std::string, CapabilityHandler> capability_handlers_;
	std::mutex                                         handlers_mutex_;

	TaskBeginHandler begin_task_handler_;
	TaskEndHandler   end_task_handler_;

	// ── 管道基础设施 ──────────────────────────────────────────────────────────
	HANDLE      listen_handle_ = INVALID_HANDLE_VALUE;
	std::string pipe_name_;

	std::atomic<bool> running_{false};

	std::thread              accept_thread_;
	std::vector<std::thread> workers_;

	std::queue<HANDLE>      work_queue_;
	std::mutex              queue_mutex_;
	std::condition_variable queue_cv_;

	// current_accept_handle_：acceptLoop 当前正在 ConnectNamedPipe 等待的句柄。
	// stop() 通过此字段调用 CancelIoEx 中断，任意迭代轮次均有效。
	HANDLE     current_accept_handle_ = INVALID_HANDLE_VALUE;
	std::mutex current_accept_mutex_;

	// 活跃连接集合：stop() 通过 CancelIoEx 中断正在 readFrame 的 worker
	std::set<HANDLE> active_handles_;
	std::mutex       active_handles_mutex_;

	// 活跃 Agent 连接计数（原子，worker 线程进出 processConnection 时维护）
	std::atomic<int>          active_connection_count_{0};
	std::function<void(int)>  on_connection_changed_;
	std::mutex                on_connection_changed_mutex_;

	// ── 内部方法 ──────────────────────────────────────────────────────────────

	Status bindAndListen(const std::string& pipe_name);
	void   acceptLoop();
	void   workerLoop();
	void   processConnection(HANDLE handle);

	// handleMessage 解析一条帧数据，按 type 路由到对应处理器，返回响应字符串。
	// endTask 等通知类消息返回空字符串（无需回写）。
	std::string handleMessage(const std::string& frame_str);

	// handleCapability 处理 capability 消息，调用已注册的 CapabilityHandler。
	std::string handleCapability(const nlohmann::json& msg);
};

// ─── Implement 方法实现 ──────────────────────────────────────────────────────

Status WindowsIpcServer::Implement::bindAndListen(const std::string& pipe_name)
{
	pipe_name_ = pipe_name;

	PipeSecurityContext sec;
	if (!sec.init()) {
		LOG_WARN("ipc server: failed to build pipe DACL, falling back to default security");
	}

	listen_handle_ = ::CreateNamedPipeA(
	    pipe_name_.c_str(),
	    PIPE_ACCESS_DUPLEX,
	    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
	    PIPE_UNLIMITED_INSTANCES,
	    65536, 65536,
	    0,
	    sec.ptr()
	);
	if (listen_handle_ == INVALID_HANDLE_VALUE) {
		return Status(Status::IO_ERROR, "failed to create named pipe");
	}
	return Status::Ok();
}

// acceptLoop 与 WindowsIpcServer 原版相同的 accept 架构（见原版注释）。
void WindowsIpcServer::Implement::acceptLoop()
{
	HANDLE current          = listen_handle_;
	bool   current_consumed = true;

	while (running_.load()) {
		{
			std::lock_guard<std::mutex> lk(current_accept_mutex_);
			current_accept_handle_ = current;
		}

		BOOL connected = ::ConnectNamedPipe(current, nullptr);

		{
			std::lock_guard<std::mutex> lk(current_accept_mutex_);
			current_accept_handle_ = INVALID_HANDLE_VALUE;
		}

		if (!connected) {
			DWORD err = ::GetLastError();
			if (err == ERROR_PIPE_CONNECTED) {
				// 客户端在 ConnectNamedPipe 之前已连接，正常继续
			} else {
				if (err != ERROR_OPERATION_ABORTED && err != ERROR_BROKEN_PIPE) {
					LOG_ERROR("acceptLoop: ConnectNamedPipe failed with error {}", err);
				}
				if (!current_consumed) {
					::CloseHandle(current);
				}
				break;
			}
		}

		{
			std::lock_guard<std::mutex> lock(queue_mutex_);
			work_queue_.push(current);
		}
		queue_cv_.notify_one();
		current_consumed = true;

		if (!running_.load()) {
			break;
		}

		PipeSecurityContext sec;
		if (!sec.init()) {
			LOG_WARN("acceptLoop: failed to build pipe DACL, falling back to default security");
		}
		current = ::CreateNamedPipeA(
		    pipe_name_.c_str(),
		    PIPE_ACCESS_DUPLEX,
		    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		    PIPE_UNLIMITED_INSTANCES,
		    65536, 65536,
		    0, sec.ptr());
		if (current == INVALID_HANDLE_VALUE) {
			if (running_.load()) {
				LOG_ERROR("acceptLoop: CreateNamedPipe failed with error {}", ::GetLastError());
			}
			break;
		}
		current_consumed = false;
	}
}

void WindowsIpcServer::Implement::workerLoop()
{
	HRESULT com_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	while (true) {
		HANDLE handle = INVALID_HANDLE_VALUE;
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			queue_cv_.wait(lock, [this] { return !work_queue_.empty(); });
			handle = work_queue_.front();
			work_queue_.pop();
		}
		if (handle == INVALID_HANDLE_VALUE) {
			break; // 哨兵
		}
		processConnection(handle);
	}

	if (SUCCEEDED(com_hr) || com_hr == S_FALSE) {
		::CoUninitialize();
	}
}

void WindowsIpcServer::Implement::processConnection(HANDLE handle)
{
	{
		std::lock_guard<std::mutex> lock(active_handles_mutex_);
		active_handles_.insert(handle);
	}

	// 连接建立：计数 +1，通知外部
	int count = active_connection_count_.fetch_add(1) + 1;
	{
		std::lock_guard<std::mutex> lk(on_connection_changed_mutex_);
		if (on_connection_changed_) on_connection_changed_(count);
	}

	while (running_.load()) {
		auto result = FrameCodec::readFrame(handle);
		if (result.failure()) {
			break;
		}
		std::string response = handleMessage(result.value());
		if (response.empty()) {
			// 通知类消息（如 endTask）无需回写
			continue;
		}
		if (!FrameCodec::writeFrame(handle, response).ok()) {
			break;
		}
	}

	{
		std::lock_guard<std::mutex> lock(active_handles_mutex_);
		active_handles_.erase(handle);
	}

	// 连接断开：计数 -1，通知外部
	count = active_connection_count_.fetch_sub(1) - 1;
	{
		std::lock_guard<std::mutex> lk(on_connection_changed_mutex_);
		if (on_connection_changed_) on_connection_changed_(count);
	}

	::DisconnectNamedPipe(handle);
	::CloseHandle(handle);
}

// handleMessage 根据 type 字段将消息路由到对应处理逻辑。
std::string WindowsIpcServer::Implement::handleMessage(const std::string& frame_str)
{
	nlohmann::json msg;
	try {
		msg = nlohmann::json::parse(frame_str);
	} catch (const nlohmann::json::parse_error&) {
		LOG_WARN("ipc server: failed to parse message JSON, ignored");
		return "";
	}

	if (!msg.is_object()) {
		LOG_WARN("ipc server: received non-object message, ignored");
		return "";
	}

	const std::string type = msg.value("type", std::string{});

	// ── beginTask ──────────────────────────────────────────────────────────────
	if (type == "beginTask") {
		std::string task_id;
		if (begin_task_handler_) {
			try {
				task_id = begin_task_handler_(
				    msg.value("description",      std::string{}),
				    msg.value("root_description", std::string{}),
				    msg.value("parent_task_id",   std::string{}),
				    msg.value("session_id",        std::string{}));
			} catch (const std::exception& e) {
				LOG_ERROR("ipc server: beginTask handler threw: {}", e.what());
			}
		} else {
			LOG_WARN("ipc server: no TaskBeginHandler registered, returning empty task_id");
		}
		return nlohmann::json{
			{"type",    "beginTask_response"},
			{"task_id", task_id},
		}.dump();
	}

	// ── endTask ────────────────────────────────────────────────────────────────
	if (type == "endTask") {
		if (end_task_handler_) {
			try {
				end_task_handler_(
				    msg.value("task_id", std::string{}),
				    msg.value("success", false));
			} catch (const std::exception& e) {
				LOG_ERROR("ipc server: endTask handler threw: {}", e.what());
			}
		}
		return ""; // 通知语义，无响应
	}

	// ── capability ────────────────────────────────────────────────────────────
	if (type == "capability") {
		return handleCapability(msg);
	}

	LOG_WARN("ipc server: unknown message type '{}', ignored", type);
	return "";
}

// handleCapability 处理 capability 消息：路由到注册的 CapabilityHandler，构造响应。
std::string WindowsIpcServer::Implement::handleCapability(const nlohmann::json& msg)
{
	const int         id         = msg.value("id",         0);
	const std::string task_id    = msg.value("task_id",    std::string{});
	const std::string capability = msg.value("capability", std::string{});
	const std::string operation  = msg.value("operation",  std::string{});
	const nlohmann::json params  = msg.value("params",     nlohmann::json::object());

	if (capability.empty() || operation.empty()) {
		return makeCapabilityResultError(id,
		    Status::INVALID_ARGUMENT,
		    "capability and operation fields are required");
	}

	CapabilityHandler handler;
	{
		std::lock_guard<std::mutex> lock(handlers_mutex_);
		auto it = capability_handlers_.find(capability);
		if (it == capability_handlers_.end()) {
			return makeCapabilityResultError(id,
			    Status::CAPABILITY_NOT_FOUND,
			    "capability not registered: " + capability);
		}
		handler = it->second;
	}

	// handler 可能长时间阻塞（等待用户确认），此期间 write_mutex_ 已释放
	Result<nlohmann::json> result = handler(task_id, operation, params);

	if (result.success()) {
		return makeCapabilityResultOk(id, result.value());
	}
	return makeCapabilityResultError(id,
	    result.error().code,
	    result.error().message ? result.error().message : "");
}

// ─── WindowsIpcServer 公开接口 ───────────────────────────────────────────────

WindowsIpcServer::WindowsIpcServer()
	: implement_(std::make_unique<Implement>())
{}

WindowsIpcServer::~WindowsIpcServer()
{
	stop();
}

void WindowsIpcServer::registerCapability(std::string_view  capability_name,
                                          CapabilityHandler handler)
{
	std::lock_guard<std::mutex> lock(implement_->handlers_mutex_);
	implement_->capability_handlers_[std::string(capability_name)] = std::move(handler);
}

void WindowsIpcServer::setTaskBeginHandler(TaskBeginHandler handler)
{
	implement_->begin_task_handler_ = std::move(handler);
}

void WindowsIpcServer::setTaskEndHandler(TaskEndHandler handler)
{
	implement_->end_task_handler_ = std::move(handler);
}

int WindowsIpcServer::activeConnectionCount() const
{
	return implement_->active_connection_count_.load();
}

void WindowsIpcServer::setOnConnectionChanged(std::function<void(int)> callback)
{
	std::lock_guard<std::mutex> lk(implement_->on_connection_changed_mutex_);
	implement_->on_connection_changed_ = std::move(callback);
}

Status WindowsIpcServer::start(std::string_view pipe_path, int thread_pool_size)
{
	if (implement_->running_.load()) {
		return Status(Status::INTERNAL_ERROR, "server is already running");
	}
	if (thread_pool_size < 1) {
		return Status(Status::INVALID_ARGUMENT, "thread_pool_size must be at least 1");
	}
	auto status = implement_->bindAndListen(std::string(pipe_path));
	if (!status.ok()) {
		return status;
	}
	implement_->running_.store(true);
	implement_->accept_thread_ = std::thread([this] { implement_->acceptLoop(); });
	implement_->workers_.reserve(static_cast<size_t>(thread_pool_size));
	for (int i = 0; i < thread_pool_size; ++i) {
		implement_->workers_.emplace_back([this] { implement_->workerLoop(); });
	}
	LOG_INFO("ipc server listening on {}, {} workers", pipe_path, thread_pool_size);
	return Status::Ok();
}

void WindowsIpcServer::stop()
{
	if (!implement_->running_.exchange(false)) {
		return;
	}

	{
		std::lock_guard<std::mutex> lk(implement_->current_accept_mutex_);
		if (implement_->current_accept_handle_ != INVALID_HANDLE_VALUE) {
			::CancelIoEx(implement_->current_accept_handle_, nullptr);
		}
	}

	if (implement_->listen_handle_ != INVALID_HANDLE_VALUE) {
		::CloseHandle(implement_->listen_handle_);
		implement_->listen_handle_ = INVALID_HANDLE_VALUE;
	}

	{
		std::lock_guard<std::mutex> lock(implement_->active_handles_mutex_);
		for (HANDLE h : implement_->active_handles_) {
			::CancelIoEx(h, nullptr);
		}
	}

	{
		std::lock_guard<std::mutex> lock(implement_->queue_mutex_);
		while (!implement_->work_queue_.empty()) {
			HANDLE h = implement_->work_queue_.front();
			implement_->work_queue_.pop();
			if (h != INVALID_HANDLE_VALUE) {
				::DisconnectNamedPipe(h);
				::CloseHandle(h);
			}
		}
		for (size_t i = 0; i < implement_->workers_.size(); ++i) {
			implement_->work_queue_.push(INVALID_HANDLE_VALUE);
		}
	}
	implement_->queue_cv_.notify_all();

	if (implement_->accept_thread_.joinable()) {
		implement_->accept_thread_.join();
	}
	for (auto& worker : implement_->workers_) {
		if (worker.joinable()) {
			worker.join();
		}
	}
	implement_->workers_.clear();

	LOG_INFO("ipc server stopped");
}

} // namespace ipc
} // namespace clawspan
