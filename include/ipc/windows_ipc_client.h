#pragma once

#include "ipc/ipc_client.h"

#include <memory>

namespace clawspan {
namespace ipc {

// WindowsIpcClient 是 IpcClientInterface 的 Windows Named Pipe 实现。
// 使用 Length-prefix 帧格式（[uint32 BE len][JSON body]）传输 Channel 1 消息。
//
// 线程安全：call_mutex_ 序列化并发调用，保证单连接上同一时刻只有一条消息在发送。
class WindowsIpcClient : public IpcClientInterface
{
public:
	WindowsIpcClient();
	~WindowsIpcClient() override;

	WindowsIpcClient(const WindowsIpcClient&)            = delete;
	WindowsIpcClient& operator=(const WindowsIpcClient&) = delete;

	Status connect(std::string_view pipe_path) override;
	void disconnect() override;

	Result<std::string>    beginTask(const std::string& description,
	                                 const std::string& root_description,
	                                 const std::string& parent_task_id,
	                                 const std::string& session_id) override;

	Status endTask(const std::string& task_id, bool success) override;

	Result<nlohmann::json> callCapability(const std::string&    task_id,
	                                      const std::string&    capability,
	                                      const std::string&    operation,
	                                      const nlohmann::json& params) override;

private:
	struct Implement;
	std::unique_ptr<Implement> implement_;
};

} // namespace ipc
} // namespace clawspan
