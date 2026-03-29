#pragma once

#include "common/error.h"

#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <string_view>

namespace clawspan {
namespace ipc {

// ─────────────────────────────────────────────────────────────────────────────
// VM 控制通道处理器类型定义
//
// VM 控制通道协议使用 type-based 消息帧（非 JSON-RPC 2.0）：
//   beginTask        (VM → Daemon) — 任务开始，Daemon 分配 task_id
//   endTask          (VM → Daemon) — 任务结束，无需响应（通知语义）
//   capability       (VM → Daemon) — 能力调用，携带 task_id
//   capability_result(Daemon → VM) — 能力调用响应
// ─────────────────────────────────────────────────────────────────────────────

// CapabilityHandler：能力调用处理器。
//
// 入参：
// - task_id:   发起本次调用的任务 ID（来自 capability 消息的 task_id 字段）。
//              空字符串表示无关联任务（向后兼容）。
// - operation: 操作名称，例如 "list_windows"。
// - params:    调用参数 JSON 对象。
//
// 返回：
// - Result::Ok(json) — 操作成功，包含结果数据。
// - Result::Error(status) — 操作失败，status 描述原因。
using CapabilityHandler = std::function<Result<nlohmann::json>(
	const std::string&    task_id,
	const std::string&    operation,
	const nlohmann::json& params)>;

// TaskBeginHandler：任务开始处理器。
//
// 由 Daemon 注册，IpcServer 在收到 beginTask 消息时调用。
// 返回 Daemon 为本次任务分配的唯一 task_id（如 "task-1"）。
//
// 入参：
// - description:      当前任务意图描述（来自 before_prompt_build hook）。
// - root_description: 用户原始意图；顶层任务时非空，子任务时传空字符串（自动继承）。
// - parent_task_id:   父任务 ID，空字符串表示顶层任务。
// - session_id:       用户会话标识（来自 OpenClaw sessionKey）。
using TaskBeginHandler = std::function<std::string(
	const std::string& description,
	const std::string& root_description,
	const std::string& parent_task_id,
	const std::string& session_id)>;

// TaskEndHandler：任务结束通知处理器。
//
// 由 Daemon 注册，IpcServer 在收到 endTask 消息时调用（无响应，纯通知语义）。
//
// 入参：
// - task_id: 结束的任务 ID。
// - success: true = 任务正常完成；false = 任务失败或被中止。
using TaskEndHandler = std::function<void(
	const std::string& task_id,
	bool               success)>;

// ─────────────────────────────────────────────────────────────────────────────
// IpcServerInterface：VM 控制通道 IPC 服务端抽象接口
// ─────────────────────────────────────────────────────────────────────────────
class IpcServerInterface
{
public:
	virtual ~IpcServerInterface() = default;

	// registerCapability 注册一个能力模块的调用处理器。
	//
	// 须在 start() 之前完成注册。
	// capability_name 对应 capability 消息的 capability 字段。
	//
	// 入参:
	// - capability_name: 能力标识，例如 "capability_ax"。
	// - handler:         能力调用处理器（含 task_id）。
	virtual void registerCapability(std::string_view   capability_name,
	                                CapabilityHandler  handler) = 0;

	// setTaskBeginHandler 设置任务开始消息的处理器。
	//
	// 须在 start() 之前完成注册。未设置时收到 beginTask 消息返回空 task_id。
	//
	// 入参:
	// - handler: 任务开始处理器，返回分配的 task_id。
	virtual void setTaskBeginHandler(TaskBeginHandler handler) = 0;

	// setTaskEndHandler 设置任务结束通知的处理器。
	//
	// 须在 start() 之前完成注册。未设置时收到 endTask 消息静默忽略。
	//
	// 入参:
	// - handler: 任务结束通知处理器。
	virtual void setTaskEndHandler(TaskEndHandler handler) = 0;

	// start 启动服务端，绑定并监听指定本地 IPC 通道。
	//
	// 入参:
	// - pipe_path:       通道路径（Windows: \\.\pipe\<name>）。
	// - thread_pool_size: 工作线程数量，默认 8。
	//
	// 出参/返回:
	// - Status::Ok()：启动成功。
	// - Status(IO_ERROR)：绑定或监听失败。
	virtual Status start(std::string_view pipe_path, int thread_pool_size = 8) = 0;

	// stop 优雅停止服务端，等待所有工作线程安全退出。
	virtual void stop() = 0;

	// activeConnectionCount 返回当前已建立连接的 VM 控制通道客户端（Agent）数量。
	// 线程安全，可从任意线程调用。
	virtual int activeConnectionCount() const = 0;

	// setOnConnectionChanged 注册连接数变化回调，在连接建立或断开时调用。
	// 回调参数为变化后的连接数（0 = 无 Agent 连接）。
	// 回调运行在 worker 线程上，实现需保证线程安全。
	virtual void setOnConnectionChanged(std::function<void(int)> callback) = 0;
};

} // namespace ipc
} // namespace clawspan
