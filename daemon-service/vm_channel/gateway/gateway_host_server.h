#pragma once

#include "audit/audit_service.h"
#include "common/error.h"

#include <cstdint>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace clawspan {
namespace vm_channel {
namespace gateway {

struct HandshakeAck
{
	bool        accepted = false;
	std::string session_id;
	std::string message;
};

struct GatewayCommand
{
	std::string session_id;
	std::string trace_id;
	std::string task_id;
	std::string command_id;
	std::string capability;
	std::string operation;
	std::string params_json;
	int64_t     enqueued_at_ms = 0;
};

struct GatewayCommandResult
{
	std::string session_id;
	std::string trace_id;
	std::string task_id;
	std::string command_id;
	bool        success = false;
	int32_t     error_code = 0;
	std::string error_message;
	std::string result_json;
	int64_t     completed_at_ms = 0;
};

struct GatewayLogEvent
{
	std::string session_id;
	std::string trace_id;
	std::string task_id;
	std::string event_id;
	std::string level;
	std::string source;
	std::string message;
	std::string fields_json;
	int64_t     reported_at_ms = 0;
};

struct GatewayRiskEvent
{
	std::string session_id;
	std::string trace_id;
	std::string task_id;
	std::string event_id;
	std::string severity;
	std::string behavior_type;
	std::string detail;
	std::string extra_json;
	int64_t     reported_at_ms = 0;
};

struct IdempotencyCachePolicy
{
	size_t max_result_entries = 2048;
	int64_t result_ttl_ms = 10 * 60 * 1000; // 10 min
};

struct FlowControlPolicy
{
	size_t max_pending_log_events = 1024;
	size_t max_pending_risk_events = 512;
	size_t per_session_log_rate_per_sec = 200;
	size_t per_session_log_burst = 200;
};

// GatewayHostServer 负责 Host 侧 gateway 的生命周期管理。
//
// 第一阶段目标是提供稳定的启停骨架，后续逐步填充：
// - vsock 会话管理
// - Envelope 编解码
// - 命令路由与事件上报
class GatewayHostServer
{
public:
	static constexpr size_t MAX_PENDING_LOG_EVENTS = 1024;
	static constexpr size_t MAX_PENDING_RISK_EVENTS = 512;

	GatewayHostServer() = default;
	~GatewayHostServer();

	GatewayHostServer(const GatewayHostServer&)            = delete;
	GatewayHostServer& operator=(const GatewayHostServer&) = delete;

	// start 启动 Host gateway。
	//
	// 入参:
	// - listen_target: 监听目标描述（第一阶段仅用于配置记录，后续可映射到具体地址/端口）。
	//
	// 出参/返回:
	// - Status::Ok()：启动成功。
	// - Status(error)：启动失败（重复启动或参数非法）。
	Status start(std::string_view listen_target);

	// stop 停止 Host gateway。
	void stop();

	// isRunning 返回当前 gateway 是否处于运行状态。
	bool isRunning() const;

	// listenTarget 返回当前生效的监听目标。
	std::string listenTarget() const;

	// registerSession 注册新会话或覆盖已有会话的最后活跃时间。
	//
	// 入参:
	// - session_id: 会话 ID，不能为空。
	// - now_ms: 当前时间戳（毫秒）。
	//
	// 出参/返回:
	// - Status::Ok()：注册成功。
	// - Status(error)：参数非法或服务未运行。
	Status registerSession(std::string_view session_id, int64_t now_ms);

	// refreshSessionHeartbeat 刷新会话心跳时间。
	//
	// 入参:
	// - session_id: 会话 ID。
	// - now_ms: 当前时间戳（毫秒）。
	//
	// 出参/返回:
	// - Status::Ok()：刷新成功。
	// - Status::NOT_FOUND：会话不存在。
	Status refreshSessionHeartbeat(std::string_view session_id, int64_t now_ms);

	// sweepExpiredSessions 清理超时会话并返回清理数量。
	//
	// 入参:
	// - now_ms: 当前时间戳（毫秒）。
	// - timeout_ms: 会话超时阈值（毫秒）。
	//
	// 出参/返回:
	// - 清理掉的会话数量。
	size_t sweepExpiredSessions(int64_t now_ms, int64_t timeout_ms);

	// sessionCount 返回当前存活会话数。
	size_t sessionCount() const;

	// handleHandshake 处理 VM 的握手请求，完成 register + ack 基础路径。
	//
	// 入参:
	// - session_id: VM 声明的会话 ID（可为空；为空时由 Host 生成）。
	// - now_ms: 当前时间戳（毫秒）。
	//
	// 出参/返回:
	// - Result<HandshakeAck>::Ok：握手成功，返回 ack 负载。
	// - Result<HandshakeAck>::Error：握手失败（服务未启动等）。
	Result<HandshakeAck> handleHandshake(std::string_view session_id, int64_t now_ms);

	// validateEnvelopeVersion 校验 Envelope 版本是否受支持。
	//
	// 当前仅支持 v1。为空或未知版本时返回错误。
	Status validateEnvelopeVersion(std::string_view version) const;

	// enqueueCommand 将 Host 侧命令放入待下发队列。
	//
	// 入参:
	// - command: 待下发命令，command_id/capability/operation 不能为空。
	//
	// 出参/返回:
	// - Status::Ok()：入队成功。
	// - Status(error)：参数非法或服务未运行。
	Status enqueueCommand(const GatewayCommand& command);

	// dequeueCommand 弹出一条待下发命令。
	//
	// 出参/返回:
	// - Result<GatewayCommand>::Ok：取到命令。
	// - Result<GatewayCommand>::Error(Status::NOT_FOUND)：当前无待下发命令。
	Result<GatewayCommand> dequeueCommand();

	// submitCommandResult 记录 VM 回传的命令结果。
	//
	// 入参:
	// - result: 命令执行结果，command_id 不能为空。
	Status submitCommandResult(const GatewayCommandResult& result);

	// takeCommandResult 读取并移除指定命令结果。
	//
	// 入参:
	// - command_id: 命令 ID。
	//
	// 出参/返回:
	// - Result<GatewayCommandResult>::Ok：返回结果并从缓存移除。
	// - Result<GatewayCommandResult>::Error(Status::NOT_FOUND)：结果不存在。
	Result<GatewayCommandResult> takeCommandResult(std::string_view command_id);

	// setIdempotencyCachePolicy 设置命令结果幂等缓存策略（容量 + TTL）。
	Status setIdempotencyCachePolicy(const IdempotencyCachePolicy& policy);

	// idempotencyCachePolicy 返回当前幂等缓存策略。
	IdempotencyCachePolicy idempotencyCachePolicy() const;

	// sweepExpiredCommandResults 按策略 TTL 清理过期命令结果并返回清理数量。
	size_t sweepExpiredCommandResults(int64_t now_ms);

	// idempotencyResultCount 返回当前缓存的命令结果数量。
	size_t idempotencyResultCount() const;

	// submitLogEvent 记录 VM 上报日志事件。
	Status submitLogEvent(const GatewayLogEvent& event);

	// dequeueLogEvent 弹出一条日志事件。
	Result<GatewayLogEvent> dequeueLogEvent();

	// droppedLogEventCount 返回因背压被丢弃的日志事件数量。
	size_t droppedLogEventCount() const;
	size_t rateLimitedLogEventCount() const;
	size_t droppedRiskEventCount() const;

	// setFlowControlPolicy 设置背压与限流策略。
	Status setFlowControlPolicy(const FlowControlPolicy& policy);

	// flowControlPolicy 返回当前背压与限流策略。
	FlowControlPolicy flowControlPolicy() const;

	// setAuditWriter 注入审计写入器；gateway 只负责事件发射，不负责存储查询。
	void setAuditWriter(std::shared_ptr<audit::AuditWriterInterface> writer);

	// submitRiskEvent 记录 VM 上报风险事件。
	Status submitRiskEvent(const GatewayRiskEvent& event);

	// dequeueRiskEvent 弹出一条风险事件。
	Result<GatewayRiskEvent> dequeueRiskEvent();

private:
	static std::string generateSessionId();
	void pruneResultCacheToCapacityLocked();
	struct RateLimitBucket
	{
		double tokens = 0.0;
		int64_t last_refill_ms = 0;
	};
	bool consumeLogRateTokenLocked(std::string_view session_id, int64_t now_ms);

	mutable std::mutex state_mutex_;
	std::string        listen_target_;
	std::unordered_map<std::string, int64_t> sessions_;
	std::deque<GatewayCommand> command_queue_;
	std::unordered_set<std::string> inflight_command_ids_;
	std::unordered_map<std::string, GatewayCommandResult> command_results_;
	IdempotencyCachePolicy idempotency_policy_{};
	FlowControlPolicy flow_control_policy_{};
	std::shared_ptr<audit::AuditWriterInterface> audit_writer_{std::make_shared<audit::AuditService>()};
	std::deque<GatewayLogEvent> log_events_;
	size_t dropped_log_events_ = 0;
	size_t rate_limited_log_events_ = 0;
	std::deque<GatewayRiskEvent> risk_events_;
	size_t dropped_risk_events_ = 0;
	std::unordered_map<std::string, RateLimitBucket> log_rate_buckets_;
	std::atomic<bool>  running_{false};
};

} // namespace gateway
} // namespace vm_channel
} // namespace clawspan

