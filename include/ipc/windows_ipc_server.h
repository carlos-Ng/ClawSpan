#pragma once

#include "ipc/ipc_server.h"

#include <functional>
#include <memory>

namespace clawspan {
namespace ipc {

// WindowsIpcServer 是 IpcServerInterface 的 Windows Named Pipe 实现。
//
// 内部架构：
//   - 一个 Accept 线程：循环 ConnectNamedPipe，将已连接的管道 HANDLE 投入工作队列。
//   - N 个 Worker 线程（线程池）：从队列取出 HANDLE，处理该连接的完整消息生命周期
//     （单连接内可连续处理多条消息），直至客户端主动断开。
//   - VM 控制通道 type-based 协议由 Worker 线程内联解析，依次路由到对应处理器。
class WindowsIpcServer : public IpcServerInterface
{
public:
	WindowsIpcServer();
	~WindowsIpcServer() override;

	WindowsIpcServer(const WindowsIpcServer&)            = delete;
	WindowsIpcServer& operator=(const WindowsIpcServer&) = delete;

	void registerCapability(std::string_view  capability_name,
	                        CapabilityHandler handler) override;

	void setTaskBeginHandler(TaskBeginHandler handler) override;
	void setTaskEndHandler(TaskEndHandler handler) override;

	Status start(std::string_view pipe_path, int thread_pool_size = 8) override;
	void stop() override;

	int  activeConnectionCount() const override;
	void setOnConnectionChanged(std::function<void(int)> callback) override;

private:
	struct Implement;
	std::unique_ptr<Implement> implement_;
};

} // namespace ipc
} // namespace clawspan
