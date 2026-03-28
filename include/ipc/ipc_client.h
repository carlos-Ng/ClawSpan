#pragma once

#include "common/error.h"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace clawspan {
namespace ipc {

// IpcClientInterface：VM 控制通道 IPC 客户端抽象接口（供 OpenClaw 插件调用）。
//
// 协议：type-based 消息帧，详见 ipc_server.h 中的消息格式说明。
// 传输：Windows Named Pipe（开发阶段），未来迁移至 vsock（由另一开发者完成）。
//
// 线程安全：单连接串行模型，多线程并发调用须在外部加锁，
// 或改用多连接（每线程独立 connect）模式。
class IpcClientInterface
{
public:
	virtual ~IpcClientInterface() = default;

	// connect 连接到指定本地 IPC 通道。
	//
	// 具有幂等语义：已连接时直接返回 Status::Ok()，不重复建立连接。
	//
	// 入参:
	// - pipe_path: 服务端监听的通道路径（Windows: \\.\pipe\<name>）。
	//
	// 出参/返回:
	// - Status::Ok()：连接成功，或已处于连接状态。
	// - Status(IO_ERROR)：连接失败。
	virtual Status connect(std::string_view pipe_path) = 0;

	// disconnect 断开连接并释放资源。
	virtual void disconnect() = 0;

	// beginTask 向 Daemon 注册一个新任务，返回 Daemon 分配的 task_id。
	//
	// OpenClaw before_prompt_build hook 在 React 循环开始前调用此方法。
	// 对于顶层任务，root_description 是用户的原始意图（信任锚点）；
	// 子任务的 root_description 传空字符串（Daemon 自动继承顶层任务的描述）。
	//
	// 入参:
	// - description:      当前任务意图描述。
	// - root_description: 用户原始意图（顶层任务时传入，子任务传 ""）。
	// - parent_task_id:   父任务 ID，空字符串表示顶层任务。
	// - session_id:       用户会话标识（OpenClaw sessionKey）。
	//
	// 出参/返回:
	// - Result::Ok(task_id)：注册成功，task_id 为本任务的唯一标识。
	// - Result::Error(status)：连接失败或协议错误。
	virtual Result<std::string> beginTask(const std::string& description,
	                                      const std::string& root_description,
	                                      const std::string& parent_task_id,
	                                      const std::string& session_id) = 0;

	// endTask 通知 Daemon 任务已结束（通知语义，无响应等待）。
	//
	// OpenClaw agent_end hook 在 React 循环结束后调用此方法。
	// 发送后立即返回，不等待 Daemon 处理完成。
	//
	// 入参:
	// - task_id: 要结束的任务 ID（由 beginTask 返回）。
	// - success: true = 任务正常完成；false = 失败或被中止。
	//
	// 出参/返回:
	// - Status::Ok()：帧发送成功（不代表 Daemon 已处理完成）。
	// - Status(IO_ERROR)：发送失败。
	virtual Status endTask(const std::string& task_id, bool success) = 0;

	// callCapability 发起一次同步能力调用，阻塞直到收到 capability_result 响应。
	//
	// 入参:
	// - task_id:    当前操作所属任务 ID（可为空字符串，表示无关联任务）。
	// - capability: 目标能力模块名称，例如 "capability_ax"。
	// - operation:  操作名称，例如 "list_windows"。
	// - params:     操作参数 JSON 对象，无参数时传空对象 {}。
	//
	// 出参/返回:
	// - Result::Ok(json)：操作成功，json 为 result 字段的值。
	// - Result::Error(status)：操作失败，status 描述原因（包含被安全策略拒绝等）。
	virtual Result<nlohmann::json> callCapability(const std::string&    task_id,
	                                              const std::string&    capability,
	                                              const std::string&    operation,
	                                              const nlohmann::json& params) = 0;
};

} // namespace ipc
} // namespace clawspan
