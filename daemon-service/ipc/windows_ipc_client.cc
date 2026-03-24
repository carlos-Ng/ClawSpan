#include "ipc/windows_ipc_client.h"
#include "frame.h"

#include "common/status.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <string>

#include <windows.h>

namespace clawspan {
namespace ipc {

// ─── Implement ──────────────────────────────────────────────────────────────

struct WindowsIpcClient::Implement
{
	HANDLE           pipe_      = INVALID_HANDLE_VALUE;
	std::mutex       call_mutex_;          // 序列化并发调用，保证帧的发送/接收配对
	std::atomic<int> next_id_{1};          // capability 消息的请求 ID（单调递增）
};

// ─── WindowsIpcClient 公开接口 ───────────────────────────────────────────────

WindowsIpcClient::WindowsIpcClient()
	: implement_(std::make_unique<Implement>())
{}

WindowsIpcClient::~WindowsIpcClient()
{
	disconnect();
}

// connect 连接到指定的 Named Pipe（幂等：已连接时直接返回 Ok）。
Status WindowsIpcClient::connect(std::string_view pipe_path)
{
	if (implement_->pipe_ != INVALID_HANDLE_VALUE) {
		return Status::Ok();
	}

	std::string path(pipe_path);

	// 等待管道可用（服务端可能尚未启动）
	while (true) {
		implement_->pipe_ = ::CreateFileA(
		    path.c_str(),
		    GENERIC_READ | GENERIC_WRITE,
		    0,
		    nullptr,
		    OPEN_EXISTING,
		    0,
		    nullptr);
		if (implement_->pipe_ != INVALID_HANDLE_VALUE) {
			break;
		}
		DWORD err = ::GetLastError();
		if (err != ERROR_PIPE_BUSY) {
			return Status(Status::IO_ERROR, "failed to connect to named pipe");
		}
		if (!::WaitNamedPipeA(path.c_str(), 1000)) {
			return Status(Status::IO_ERROR, "named pipe wait timeout");
		}
	}

	DWORD mode = PIPE_READMODE_BYTE;
	if (!::SetNamedPipeHandleState(implement_->pipe_, &mode, nullptr, nullptr)) {
		::CloseHandle(implement_->pipe_);
		implement_->pipe_ = INVALID_HANDLE_VALUE;
		return Status(Status::IO_ERROR, "failed to set pipe mode");
	}
	return Status::Ok();
}

// disconnect 关闭管道连接并释放资源。
void WindowsIpcClient::disconnect()
{
	if (implement_->pipe_ != INVALID_HANDLE_VALUE) {
		::CloseHandle(implement_->pipe_);
		implement_->pipe_ = INVALID_HANDLE_VALUE;
	}
}

// beginTask 发送 beginTask 消息，阻塞等待 beginTask_response，返回分配的 task_id。
Result<std::string> WindowsIpcClient::beginTask(const std::string& description,
                                                const std::string& root_description,
                                                const std::string& parent_task_id,
                                                const std::string& session_id)
{
	nlohmann::json msg = {
		{"type",             "beginTask"},
		{"description",      description},
		{"root_description", root_description},
		{"parent_task_id",   parent_task_id},
		{"session_id",       session_id},
	};

	std::lock_guard<std::mutex> lock(implement_->call_mutex_);
	if (implement_->pipe_ == INVALID_HANDLE_VALUE) {
		return Result<std::string>::Error(Status::NOT_INITIALIZED, "not connected");
	}

	if (!FrameCodec::writeFrame(implement_->pipe_, msg.dump()).ok()) {
		return Result<std::string>::Error(Status::IO_ERROR, "beginTask: write failed");
	}

	auto read_result = FrameCodec::readFrame(implement_->pipe_);
	if (read_result.failure()) {
		return Result<std::string>::Error(read_result.error());
	}

	try {
		auto resp = nlohmann::json::parse(read_result.value());
		if (resp.value("type", std::string{}) != "beginTask_response") {
			return Result<std::string>::Error(Status::INTERNAL_ERROR,
			    "beginTask: unexpected response type");
		}
		return Result<std::string>::Ok(resp.value("task_id", std::string{}));
	} catch (const std::exception&) {
		return Result<std::string>::Error(Status::INTERNAL_ERROR,
		    "beginTask: invalid response JSON");
	}
}

// endTask 发送 endTask 消息（通知语义，不等待响应）。
Status WindowsIpcClient::endTask(const std::string& task_id, bool success)
{
	nlohmann::json msg = {
		{"type",    "endTask"},
		{"task_id", task_id},
		{"success", success},
	};

	std::lock_guard<std::mutex> lock(implement_->call_mutex_);
	if (implement_->pipe_ == INVALID_HANDLE_VALUE) {
		return Status(Status::NOT_INITIALIZED, "not connected");
	}

	return FrameCodec::writeFrame(implement_->pipe_, msg.dump());
}

// callCapability 发送 capability 消息，阻塞等待 capability_result 响应。
Result<nlohmann::json> WindowsIpcClient::callCapability(const std::string&    task_id,
                                                        const std::string&    capability,
                                                        const std::string&    operation,
                                                        const nlohmann::json& params)
{
	const int id = implement_->next_id_.fetch_add(1, std::memory_order_relaxed);
	nlohmann::json msg = {
		{"type",       "capability"},
		{"id",         id},
		{"task_id",    task_id},
		{"capability", capability},
		{"operation",  operation},
		{"params",     params},
	};

	std::lock_guard<std::mutex> lock(implement_->call_mutex_);
	if (implement_->pipe_ == INVALID_HANDLE_VALUE) {
		return Result<nlohmann::json>::Error(Status::NOT_INITIALIZED, "not connected");
	}

	if (!FrameCodec::writeFrame(implement_->pipe_, msg.dump()).ok()) {
		return Result<nlohmann::json>::Error(Status::IO_ERROR, "callCapability: write failed");
	}

	auto read_result = FrameCodec::readFrame(implement_->pipe_);
	if (read_result.failure()) {
		return Result<nlohmann::json>::Error(read_result.error());
	}

	try {
		auto resp = nlohmann::json::parse(read_result.value());
		if (resp.value("type", std::string{}) != "capability_result") {
			return Result<nlohmann::json>::Error(Status::INTERNAL_ERROR,
			    "callCapability: unexpected response type");
		}

		if (resp.value("success", false)) {
			return Result<nlohmann::json>::Ok(
			    resp.value("result", nlohmann::json::object()));
		}

		// 从响应中还原 Status
		const auto error_code    = static_cast<Status::Code>(
		    resp.value("error_code", static_cast<int>(Status::INTERNAL_ERROR)));
		const std::string error_msg = resp.value("error_message", std::string{});
		return Result<nlohmann::json>::Error(Status(error_code, error_msg));

	} catch (const std::exception&) {
		return Result<nlohmann::json>::Error(Status::INTERNAL_ERROR,
		    "callCapability: invalid response JSON");
	}
}

} // namespace ipc
} // namespace clawspan
