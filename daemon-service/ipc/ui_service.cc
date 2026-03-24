#include "ipc/ui_service.h"
#include "frame.h"
#include "pipe_security.h"

#include "common/log.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>

namespace clawspan {
namespace ipc {

// ─── Implement ──────────────────────────────────────────────────────────────

struct UIService::Implement
{
	// ── 配置参数 ──────────────────────────────────────────────────────────────
	std::string                  pipe_path_;
	ConfirmTimeoutMode           timeout_mode_     = ConfirmTimeoutMode::WAIT_FOREVER;
	std::chrono::seconds         timeout_secs_     = std::chrono::seconds(30);
	std::function<std::string()> status_provider_;

	// ── 运行状态 ──────────────────────────────────────────────────────────────
	std::atomic<bool> running_{false};
	std::atomic<bool> connected_{false};
	std::thread       service_thread_;

	// ── 管道句柄 ──────────────────────────────────────────────────────────────
	// 当前活跃的管道句柄（ConnectNamedPipe 等待阶段或已连接读取阶段均有效）。
	// stop() 通过 pipe_mutex_ 读取此句柄，并调用 CancelIoEx 中断阻塞。
	HANDLE     pipe_handle_ = INVALID_HANDLE_VALUE;
	std::mutex pipe_mutex_;

	// ── 写锁 ──────────────────────────────────────────────────────────────────
	// 序列化来自 push()、askConfirm() 和 serviceLoop() 连接回调的写操作。
	// 加锁顺序规范：write_mutex_ -> pipe_mutex_（严格遵守，防止 ABBA 死锁）。
	std::mutex write_mutex_;

	// ── OVERLAPPED 写事件 ────────────────────────────────────────────────────
	// 所有写操作（push / askConfirm / 初始状态推送）共享同一写事件句柄，
	// 写操作由 write_mutex_ 序列化，保证同一时刻只有一个写操作使用此事件。
	HANDLE write_event_ = INVALID_HANDLE_VALUE;

	// ── 待处理确认 ────────────────────────────────────────────────────────────
	using ConfirmPromise = std::promise<UIConfirmResult>;
	std::map<std::string, ConfirmPromise> pending_confirms_;
	std::mutex                            pending_mutex_;

	// ── 确认 ID 生成器 ────────────────────────────────────────────────────────
	std::atomic<uint64_t> next_id_{1};

	// ── 内部方法 ──────────────────────────────────────────────────────────────

	// serviceLoop 在独立线程中持续监听 UI 客户端连接，处理完整连接生命周期。
	// UI 断线后自动重新创建管道实例，等待下次连接（running_ 为 false 时退出）。
	void serviceLoop();

	// handleRead 解析并处理来自 UI 的一条帧数据。
	// 返回 false 表示读取循环应当退出（协议错误等不可恢复情形当前不触发此路径）。
	void handleRead(const std::string& frame);

	// writeUnlocked 向当前管道写入一帧 JSON（调用方须已持有 write_mutex_）。
	// 使用 OVERLAPPED 模式写入，不会被 serviceLoop 的 ReadFile 阻塞。
	// 写入失败时返回 false（管道未连接或 IO 错误）。
	bool writeUnlocked(const std::string& json);

	// cancelAllPendingConfirms 将所有待处理确认请求以 {false, false} 拒绝。
	// 在 UI 断线或 stop() 时调用，保证安全优先（不应持有 write_mutex_ 进入）。
	void cancelAllPendingConfirms();

	// generateId 返回单调递增的唯一确认请求 ID 字符串。
	std::string generateId();
};

// ─── Implement 方法实现 ──────────────────────────────────────────────────────

// serviceLoop：接受连接 → 推送状态 → 读取响应 → 断线清理 → 循环。
//
// 管道以 FILE_FLAG_OVERLAPPED 模式创建，读取使用 serviceLoop 专属的 read_event，
// 写入使用共享的 write_event_（由 write_mutex_ 序列化）。
// 两者使用各自独立的 OVERLAPPED 结构 + Event，互不阻塞。
void UIService::Implement::serviceLoop()
{
	// serviceLoop 专属读事件——只在此线程使用，无需加锁
	HANDLE read_event = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (read_event == nullptr) {
		LOG_ERROR("ui service: CreateEvent for read failed, error {}", ::GetLastError());
		return;
	}

	while (running_.load()) {

		// 为当前连接轮次构建专属安全属性
		PipeSecurityContext sec;
		if (!sec.init()) {
			LOG_WARN("ui service: failed to build pipe DACL, falling back to default security");
		}

		HANDLE handle = ::CreateNamedPipeA(
		    pipe_path_.c_str(),
		    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
		    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		    PIPE_UNLIMITED_INSTANCES,
		    65536, 65536,
		    0,
		    sec.ptr()
		);
		if (handle == INVALID_HANDLE_VALUE) {
			if (running_.load()) {
				LOG_ERROR("ui service: CreateNamedPipe failed, error {}", ::GetLastError());
			}
			break;
		}

		// 注册当前句柄，使 stop() 可通过 CancelIoEx 中断 ConnectNamedPipe 或 readFrame
		{
			std::lock_guard<std::mutex> lk(pipe_mutex_);
			pipe_handle_ = handle;
		}

		LOG_DEBUG("ui service: waiting for UI client on {}", pipe_path_);

		// OVERLAPPED ConnectNamedPipe
		OVERLAPPED connect_ov = {};
		connect_ov.hEvent = read_event;
		::ResetEvent(read_event);

		BOOL ok = ::ConnectNamedPipe(handle, &connect_ov);
		if (!ok) {
			DWORD err = ::GetLastError();
			if (err == ERROR_IO_PENDING) {
				// 等待客户端连接
				DWORD unused = 0;
				if (!::GetOverlappedResult(handle, &connect_ov, &unused, TRUE)) {
					DWORD ov_err = ::GetLastError();
					if (ov_err != ERROR_PIPE_CONNECTED) {
						if (ov_err != ERROR_OPERATION_ABORTED && ov_err != ERROR_BROKEN_PIPE) {
							LOG_ERROR("ui service: ConnectNamedPipe overlapped failed, error {}", ov_err);
						}
						{
							std::lock_guard<std::mutex> lk(pipe_mutex_);
							pipe_handle_ = INVALID_HANDLE_VALUE;
						}
						::CloseHandle(handle);
						if (!running_.load()) break;
						continue; // 重试
					}
				}
			} else if (err == ERROR_PIPE_CONNECTED) {
				// 客户端在 ConnectNamedPipe 之前已完成连接，视为成功
			} else {
				if (err != ERROR_OPERATION_ABORTED && err != ERROR_BROKEN_PIPE) {
					LOG_ERROR("ui service: ConnectNamedPipe failed, error {}", err);
				}
				{
					std::lock_guard<std::mutex> lk(pipe_mutex_);
					pipe_handle_ = INVALID_HANDLE_VALUE;
				}
				::CloseHandle(handle);
				break; // stop() 已调用，退出 serviceLoop
			}
		}

		if (!running_.load()) {
			// stop() 在 ConnectNamedPipe 返回后才将 running_ 置 false
			{
				std::lock_guard<std::mutex> lk(pipe_mutex_);
				pipe_handle_ = INVALID_HANDLE_VALUE;
			}
			::DisconnectNamedPipe(handle);
			::CloseHandle(handle);
			break;
		}

		// ── 连接建立 ──────────────────────────────────────────────────────────
		LOG_INFO("ui service: UI client connected");
		connected_.store(true);

		// 向 UI 推送当前状态快照（UI 连接时的握手消息）
		if (status_provider_) {
			std::string status_json = status_provider_();
			std::lock_guard<std::mutex> lk(write_mutex_);
			writeUnlocked(status_json);
		}

		// ── 读取循环（使用 OVERLAPPED 读，不阻塞其他线程的写）────────────────
		while (running_.load()) {
			auto result = FrameCodec::readFrameAsync(handle, read_event);
			if (result.failure()) {
				break; // 客户端断线或 stop() 触发 CancelIoEx
			}
			handleRead(result.value());
		}

		// ── 断线清理 ──────────────────────────────────────────────────────────
		LOG_INFO("ui service: UI client disconnected");
		connected_.store(false);

		// 安全优先：UI 缺失代表缺少安全观察员，所有挂起确认均以拒绝结束
		cancelAllPendingConfirms();

		{
			std::lock_guard<std::mutex> lk(pipe_mutex_);
			pipe_handle_ = INVALID_HANDLE_VALUE;
		}
		::DisconnectNamedPipe(handle);
		::CloseHandle(handle);
	}

	::CloseHandle(read_event);
	LOG_DEBUG("ui service: serviceLoop exited");
}

// handleRead 解析 confirm_response 消息并兑现对应的 promise。
void UIService::Implement::handleRead(const std::string& frame)
{
	try {
		auto j = nlohmann::json::parse(frame);
		if (!j.is_object()) {
			LOG_WARN("ui service: received non-object JSON, ignored");
			return;
		}

		auto type = j.value("type", std::string{});
		if (type != "confirm_response") {
			LOG_WARN("ui service: unknown message type '{}' from UI, ignored", type);
			return;
		}

		auto confirm_id  = j.value("confirm_id", std::string{});
		bool user_allowed = j.value("confirmed", false);
		bool trust_fp    = j.value("trust_fingerprint", false);

		std::lock_guard<std::mutex> lk(pending_mutex_);
		auto it = pending_confirms_.find(confirm_id);
		if (it == pending_confirms_.end()) {
			// 超时路径已先于此处将 entry 移除，忽略即可
			LOG_WARN("ui service: confirm_response for unknown id '{}', ignored", confirm_id);
			return;
		}
		it->second.set_value(UIConfirmResult{user_allowed, trust_fp});
		pending_confirms_.erase(it);

	} catch (const std::exception& e) {
		LOG_WARN("ui service: failed to parse message from UI: {}", e.what());
	}
}

// writeUnlocked：调用方须已持有 write_mutex_。
// 使用 OVERLAPPED 模式写入，通过 write_event_ 等待完成，不会被 ReadFile 阻塞。
// 内部短暂获取 pipe_mutex_ 以读取 pipe_handle_（加锁顺序：write -> pipe）。
bool UIService::Implement::writeUnlocked(const std::string& json)
{
	HANDLE h;
	{
		std::lock_guard<std::mutex> lk(pipe_mutex_);
		h = pipe_handle_;
	}
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	return FrameCodec::writeFrameAsync(h, json, write_event_).ok();
}

// cancelAllPendingConfirms：以 {false, false} 兑现所有挂起 promise，然后清空映射。
void UIService::Implement::cancelAllPendingConfirms()
{
	std::lock_guard<std::mutex> lk(pending_mutex_);
	for (auto& [id, promise] : pending_confirms_) {
		promise.set_value(UIConfirmResult{false, false});
	}
	pending_confirms_.clear();
}

// generateId：返回单调递增 ID 字符串（自 1 开始）。
std::string UIService::Implement::generateId()
{
	return std::to_string(next_id_.fetch_add(1, std::memory_order_relaxed));
}

// ─── UIMessageFactory 实现 ───────────────────────────────────────────────────

// createStatus 构造系统状态消息（UI 连接时推送一次，后续状态变更时再推送）。
std::string UIMessageFactory::createStatus(const std::string& vm,
                                           const std::string& openclaw,
                                           const std::string& channel)
{
	return nlohmann::json{
		{"type",     "status"},
		{"vm",       vm},
		{"openclaw", openclaw},
		{"channel",  channel},
	}.dump();
}

// createTaskBegin 构造任务开始消息。
std::string UIMessageFactory::createTaskBegin(const std::string& task_id,
                                              const std::string& root_description)
{
	return nlohmann::json{
		{"type",             "task_begin"},
		{"task_id",          task_id},
		{"root_description", root_description},
	}.dump();
}

// createTaskEnd 构造任务结束消息。
std::string UIMessageFactory::createTaskEnd(const std::string& task_id)
{
	return nlohmann::json{
		{"type",    "task_end"},
		{"task_id", task_id},
	}.dump();
}

// createOpLog 构造操作日志消息。
std::string UIMessageFactory::createOpLog(const std::string& task_id,
                                          const std::string& operation,
                                          const std::string& result,
                                          const std::string& source,
                                          const std::string& detail,
                                          int64_t            timestamp)
{
	return nlohmann::json{
		{"type",      "op_log"},
		{"task_id",   task_id},
		{"operation", operation},
		{"result",    result},
		{"source",    source},
		{"detail",    detail},
		{"timestamp", timestamp},
	}.dump();
}

// ─── UIService 公开接口 ──────────────────────────────────────────────────────

UIService::UIService()
	: impl_(std::make_unique<Implement>())
{}

UIService::~UIService()
{
	stop();
}

// start 启动 Named Pipe 服务线程，等待 UI 客户端接入。
Status UIService::start(std::string_view                  pipe_path,
                        ConfirmTimeoutMode                timeout_mode,
                        std::chrono::seconds              timeout_secs,
                        std::function<std::string()>      status_provider)
{
	if (impl_->running_.load()) {
		return Status(Status::INTERNAL_ERROR, "UIService is already running");
	}

	impl_->pipe_path_       = std::string(pipe_path);
	impl_->timeout_mode_    = timeout_mode;
	impl_->timeout_secs_    = timeout_secs;
	impl_->status_provider_ = std::move(status_provider);

	// 创建写操作专用事件（write_mutex_ 保证同一时刻只有一个写操作使用）
	impl_->write_event_ = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (impl_->write_event_ == nullptr) {
		return Status(Status::IO_ERROR, "failed to create write event for UIService");
	}

	impl_->running_.store(true);
	impl_->service_thread_ = std::thread([this] { impl_->serviceLoop(); });

	LOG_INFO("ui service: started on {}", pipe_path);
	return Status::Ok();
}

// stop 停止服务：中断阻塞 IO，取消所有待处理确认请求，等待服务线程退出。
void UIService::stop()
{
	if (!impl_->running_.exchange(false)) {
		return; // 已停止或从未启动
	}

	// 中断 serviceLoop 当前阻塞的 ConnectNamedPipe 或 readFrame
	{
		std::lock_guard<std::mutex> lk(impl_->pipe_mutex_);
		if (impl_->pipe_handle_ != INVALID_HANDLE_VALUE) {
			::CancelIoEx(impl_->pipe_handle_, nullptr);
		}
	}

	// 安全优先：拒绝所有待处理的确认请求，使 askConfirm() 调用方尽快返回
	impl_->cancelAllPendingConfirms();

	if (impl_->service_thread_.joinable()) {
		impl_->service_thread_.join();
	}

	// 清理写事件句柄
	if (impl_->write_event_ != INVALID_HANDLE_VALUE) {
		::CloseHandle(impl_->write_event_);
		impl_->write_event_ = INVALID_HANDLE_VALUE;
	}

	impl_->connected_.store(false);
	LOG_INFO("ui service: stopped");
}

// push 非阻断地向 UI 推送一条 JSON 消息；UI 未连接时静默忽略。
void UIService::push(const std::string& json)
{
	if (!impl_->connected_.load()) {
		LOG_INFO("ui service: push() skipped (not connected)");
		return;
	}
	std::lock_guard<std::mutex> lk(impl_->write_mutex_);
	if (!impl_->writeUnlocked(json)) {
		LOG_WARN("ui service: push() write failed (client may have just disconnected)");
	} else {
		LOG_INFO("ui service: push() delivered {} bytes", json.size());
	}
}

// askConfirm 向 UI 发送确认请求并阻断等待用户响应。
// 行为由构造时传入的 timeout_mode 控制：
//   WAIT_FOREVER  — 无限等待用户响应。
//   TIMEOUT_DENY  — 超时后自动拒绝（安全优先）。
//   TIMEOUT_ALLOW — 超时后自动允许（应用优先）。
// UI 未连接、stop() 被调用或写入失败时均立即返回 {false, false}。
UIConfirmResult UIService::askConfirm(const UIConfirmRequest& req)
{
	// UI 未连接：安全优先，直接拒绝
	if (!impl_->connected_.load()) {
		return {false, false};
	}

	std::string confirm_id = impl_->generateId();

	// 构建 confirm 推送消息
	std::string msg = nlohmann::json{
		{"type",             "confirm"},
		{"confirm_id",       confirm_id},
		{"task_id",          req.task_id},
		{"root_description", req.root_description},
		{"capability",       req.capability},
		{"operation",        req.operation},
		{"params",           req.params},
		{"reason",           req.reason},
		{"fingerprint",      req.fingerprint},
	}.dump();

	// 注册 promise（须在写入前注册，防止 handleRead 在写入后、注册前即到达）
	std::future<UIConfirmResult> future;
	{
		std::lock_guard<std::mutex> lk(impl_->pending_mutex_);
		auto [it, inserted] = impl_->pending_confirms_.emplace(
		    confirm_id, std::promise<UIConfirmResult>{});
		(void)inserted;
		future = it->second.get_future();
	}

	// 发送 confirm 消息
	{
		std::lock_guard<std::mutex> lk(impl_->write_mutex_);
		if (!impl_->writeUnlocked(msg)) {
			// 写入失败（断线等）：移除 promise，拒绝请求
			std::lock_guard<std::mutex> plk(impl_->pending_mutex_);
			impl_->pending_confirms_.erase(confirm_id);
			return {false, false};
		}
	}

	// ── 等待用户响应 ──────────────────────────────────────────────────────────
	switch (impl_->timeout_mode_) {

		case ConfirmTimeoutMode::WAIT_FOREVER: {
			// cancelAllPendingConfirms() 或 handleRead() 负责兑现 promise
			future.wait();
			return future.get();
		}

		case ConfirmTimeoutMode::TIMEOUT_DENY: {
			auto s = future.wait_for(impl_->timeout_secs_);
			if (s == std::future_status::timeout) {
				// 超时自动拒绝；移除 promise 防止 handleRead 稍后再次兑现（无害但避免日志噪声）
				{
					std::lock_guard<std::mutex> lk(impl_->pending_mutex_);
					impl_->pending_confirms_.erase(confirm_id);
				}
				LOG_WARN("ui service: confirm '{}' timed out, auto-deny", confirm_id);
				return {false, false};
			}
			return future.get();
		}

		case ConfirmTimeoutMode::TIMEOUT_ALLOW: {
			auto s = future.wait_for(impl_->timeout_secs_);
			if (s == std::future_status::timeout) {
				{
					std::lock_guard<std::mutex> lk(impl_->pending_mutex_);
					impl_->pending_confirms_.erase(confirm_id);
				}
				LOG_WARN("ui service: confirm '{}' timed out, auto-allow", confirm_id);
				return {true, false};
			}
			return future.get();
		}
	}

	// 不可达路径（覆盖所有枚举值），安全兜底
	return {false, false};
}

// isConnected 返回 UI 客户端当前是否已连接。
bool UIService::isConnected() const
{
	return impl_->connected_.load();
}

} // namespace ipc
} // namespace clawspan
