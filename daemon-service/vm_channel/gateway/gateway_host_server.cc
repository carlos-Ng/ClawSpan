#include "vm_channel/gateway/gateway_host_server.h"

#include "common/log.h"

#include <algorithm>
#include <atomic>

namespace clawspan {
namespace vm_channel {
namespace gateway {

namespace {

std::atomic<uint64_t> g_session_seq{1};

std::string buildContextTag(std::string_view session_id,
                            std::string_view trace_id,
                            std::string_view task_id)
{
	return "session=" + std::string(session_id) +
	       " trace=" + std::string(trace_id) +
	       " task=" + std::string(task_id);
}

audit::AuditRecord buildAuditRecord(std::string_view event_type,
                                    std::string_view event_id,
                                    std::string_view session_id,
                                    std::string_view trace_id,
                                    std::string_view task_id,
                                    int64_t timestamp_ms,
                                    std::string payload_json)
{
	audit::AuditRecord record;
	record.event_type = std::string(event_type);
	record.event_id = std::string(event_id);
	record.session_id = std::string(session_id);
	record.trace_id = std::string(trace_id);
	record.task_id = std::string(task_id);
	record.timestamp_ms = timestamp_ms;
	record.payload_json = std::move(payload_json);
	return record;
}

} // namespace

GatewayHostServer::~GatewayHostServer()
{
	stop();
}

Status GatewayHostServer::start(std::string_view listen_target)
{
	if (listen_target.empty()) {
		return Status(Status::INVALID_ARGUMENT, "gateway listen target is empty");
	}

	if (running_.exchange(true)) {
		return Status(Status::INTERNAL_ERROR, "gateway host server is already running");
	}

	{
		std::lock_guard<std::mutex> lk(state_mutex_);
		listen_target_ = std::string(listen_target);
		sessions_.clear();
		command_queue_.clear();
		inflight_command_ids_.clear();
		command_results_.clear();
		log_events_.clear();
		dropped_log_events_ = 0;
		rate_limited_log_events_ = 0;
		risk_events_.clear();
		dropped_risk_events_ = 0;
		log_rate_buckets_.clear();
		idempotency_policy_ = IdempotencyCachePolicy{};
		flow_control_policy_ = FlowControlPolicy{};
	}

	LOG_INFO("gateway(host): started on target '{}'", listen_target);
	return Status::Ok();
}

void GatewayHostServer::stop()
{
	if (!running_.exchange(false)) {
		return;
	}

	{
		std::lock_guard<std::mutex> lk(state_mutex_);
		sessions_.clear();
		command_queue_.clear();
		inflight_command_ids_.clear();
		command_results_.clear();
		log_events_.clear();
		dropped_log_events_ = 0;
		rate_limited_log_events_ = 0;
		risk_events_.clear();
		dropped_risk_events_ = 0;
		log_rate_buckets_.clear();
		idempotency_policy_ = IdempotencyCachePolicy{};
		flow_control_policy_ = FlowControlPolicy{};
	}

	LOG_INFO("gateway(host): stopped");
}

bool GatewayHostServer::isRunning() const
{
	return running_.load();
}

std::string GatewayHostServer::listenTarget() const
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return listen_target_;
}

Status GatewayHostServer::validateEnvelopeVersion(std::string_view version) const
{
	if (version.empty()) {
		return Status(Status::INVALID_ARGUMENT, "envelope version is empty");
	}
	if (version != "v1") {
		return Status(Status::NOT_SUPPORTED, "unsupported envelope version");
	}
	return Status::Ok();
}

Status GatewayHostServer::registerSession(std::string_view session_id, int64_t now_ms)
{
	if (session_id.empty()) {
		return Status(Status::INVALID_ARGUMENT, "session_id is empty");
	}
	if (!running_.load()) {
		return Status(Status::NOT_INITIALIZED, "gateway host server is not running");
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	sessions_[std::string(session_id)] = now_ms;
	return Status::Ok();
}

Status GatewayHostServer::refreshSessionHeartbeat(std::string_view session_id, int64_t now_ms)
{
	if (session_id.empty()) {
		return Status(Status::INVALID_ARGUMENT, "session_id is empty");
	}
	if (!running_.load()) {
		return Status(Status::NOT_INITIALIZED, "gateway host server is not running");
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	auto it = sessions_.find(std::string(session_id));
	if (it == sessions_.end()) {
		return Status(Status::NOT_FOUND, "session not found");
	}
	it->second = now_ms;
	return Status::Ok();
}

size_t GatewayHostServer::sweepExpiredSessions(int64_t now_ms, int64_t timeout_ms)
{
	if (!running_.load()) {
		return 0;
	}
	if (timeout_ms <= 0) {
		return 0;
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	size_t removed = 0;
	for (auto it = sessions_.begin(); it != sessions_.end();) {
		const int64_t age_ms = now_ms - it->second;
		if (age_ms > timeout_ms) {
			it = sessions_.erase(it);
			++removed;
		} else {
			++it;
		}
	}
	return removed;
}

size_t GatewayHostServer::sessionCount() const
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return sessions_.size();
}

Result<HandshakeAck> GatewayHostServer::handleHandshake(std::string_view session_id, int64_t now_ms)
{
	if (!running_.load()) {
		return Result<HandshakeAck>::Error(Status::NOT_INITIALIZED,
		                                   "gateway host server is not running");
	}

	std::string resolved_session_id =
		session_id.empty() ? generateSessionId() : std::string(session_id);

	auto st = registerSession(resolved_session_id, now_ms);
	if (!st.ok()) {
		return Result<HandshakeAck>::Error(st);
	}

	HandshakeAck ack{
		.accepted = true,
		.session_id = std::move(resolved_session_id),
		.message = "handshake accepted",
	};
	LOG_INFO("gateway(host): handshake accepted, session={}", ack.session_id);
	return Result<HandshakeAck>::Ok(std::move(ack));
}

Status GatewayHostServer::enqueueCommand(const GatewayCommand& command)
{
	if (!running_.load()) {
		return Status(Status::NOT_INITIALIZED, "gateway host server is not running");
	}
	if (command.command_id.empty() || command.capability.empty() || command.operation.empty()) {
		return Status(Status::INVALID_ARGUMENT, "command fields are invalid");
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	if (command_results_.find(command.command_id) != command_results_.end() ||
	    inflight_command_ids_.find(command.command_id) != inflight_command_ids_.end()) {
		LOG_DEBUG("gateway(host): duplicate command ignored, id={}, {}",
		          command.command_id,
		          buildContextTag(command.session_id, command.trace_id, command.task_id));
		return Status::Ok();
	}
	for (const auto& queued : command_queue_) {
		if (queued.command_id == command.command_id) {
			LOG_DEBUG("gateway(host): duplicate queued command ignored, id={}, {}",
			          command.command_id,
			          buildContextTag(command.session_id, command.trace_id, command.task_id));
			return Status::Ok();
		}
	}
	command_queue_.push_back(command);
	LOG_DEBUG("gateway(host): command enqueued, id={}, {}",
	          command.command_id,
	          buildContextTag(command.session_id, command.trace_id, command.task_id));
	return Status::Ok();
}

Result<GatewayCommand> GatewayHostServer::dequeueCommand()
{
	if (!running_.load()) {
		return Result<GatewayCommand>::Error(Status::NOT_INITIALIZED,
		                                     "gateway host server is not running");
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	if (command_queue_.empty()) {
		return Result<GatewayCommand>::Error(Status::NOT_FOUND, "no pending command");
	}

	GatewayCommand command = std::move(command_queue_.front());
	command_queue_.pop_front();
	inflight_command_ids_.insert(command.command_id);
	LOG_DEBUG("gateway(host): command dequeued, id={}, {}",
	          command.command_id,
	          buildContextTag(command.session_id, command.trace_id, command.task_id));
	return Result<GatewayCommand>::Ok(std::move(command));
}

Status GatewayHostServer::submitCommandResult(const GatewayCommandResult& result)
{
	if (!running_.load()) {
		return Status(Status::NOT_INITIALIZED, "gateway host server is not running");
	}
	if (result.command_id.empty()) {
		return Status(Status::INVALID_ARGUMENT, "command_id is empty");
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	inflight_command_ids_.erase(result.command_id);
	if (command_results_.find(result.command_id) != command_results_.end()) {
		LOG_DEBUG("gateway(host): duplicate command result ignored, id={}, {}",
		          result.command_id,
		          buildContextTag(result.session_id, result.trace_id, result.task_id));
		return Status::Ok();
	}
	command_results_[result.command_id] = result;
	pruneResultCacheToCapacityLocked();
	if (audit_writer_) {
		auto audit_st = audit_writer_->write(buildAuditRecord(
			"command_result",
			result.command_id,
			result.session_id,
			result.trace_id,
			result.task_id,
			result.completed_at_ms,
			result.result_json));
		if (!audit_st.ok()) {
			LOG_WARN("gateway(host): emit command_result audit failed, code={}",
			         static_cast<int>(audit_st.code));
		}
	}
	LOG_DEBUG("gateway(host): command result submitted, id={}, success={}, {}",
	          result.command_id,
	          result.success,
	          buildContextTag(result.session_id, result.trace_id, result.task_id));
	return Status::Ok();
}

Result<GatewayCommandResult> GatewayHostServer::takeCommandResult(std::string_view command_id)
{
	if (!running_.load()) {
		return Result<GatewayCommandResult>::Error(Status::NOT_INITIALIZED,
		                                           "gateway host server is not running");
	}
	if (command_id.empty()) {
		return Result<GatewayCommandResult>::Error(Status::INVALID_ARGUMENT,
		                                           "command_id is empty");
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	auto it = command_results_.find(std::string(command_id));
	if (it == command_results_.end()) {
		return Result<GatewayCommandResult>::Error(Status::NOT_FOUND, "command result not found");
	}

	GatewayCommandResult result = std::move(it->second);
	command_results_.erase(it);
	LOG_DEBUG("gateway(host): command result consumed, id={}, {}",
	          result.command_id,
	          buildContextTag(result.session_id, result.trace_id, result.task_id));
	return Result<GatewayCommandResult>::Ok(std::move(result));
}

Status GatewayHostServer::setIdempotencyCachePolicy(const IdempotencyCachePolicy& policy)
{
	if (policy.max_result_entries == 0 || policy.result_ttl_ms <= 0) {
		return Status(Status::INVALID_ARGUMENT, "invalid idempotency cache policy");
	}
	std::lock_guard<std::mutex> lk(state_mutex_);
	idempotency_policy_ = policy;
	pruneResultCacheToCapacityLocked();
	return Status::Ok();
}

IdempotencyCachePolicy GatewayHostServer::idempotencyCachePolicy() const
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return idempotency_policy_;
}

size_t GatewayHostServer::sweepExpiredCommandResults(int64_t now_ms)
{
	if (!running_.load() || now_ms <= 0) {
		return 0;
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	size_t removed = 0;
	for (auto it = command_results_.begin(); it != command_results_.end();) {
		const int64_t completed_at_ms = it->second.completed_at_ms;
		if (completed_at_ms > 0 &&
		    now_ms - completed_at_ms > idempotency_policy_.result_ttl_ms) {
			it = command_results_.erase(it);
			++removed;
		} else {
			++it;
		}
	}
	return removed;
}

size_t GatewayHostServer::idempotencyResultCount() const
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return command_results_.size();
}

Status GatewayHostServer::submitLogEvent(const GatewayLogEvent& event)
{
	if (!running_.load()) {
		return Status(Status::NOT_INITIALIZED, "gateway host server is not running");
	}
	if (event.event_id.empty()) {
		return Status(Status::INVALID_ARGUMENT, "log event_id is empty");
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	if (!consumeLogRateTokenLocked(event.session_id, event.reported_at_ms)) {
		++rate_limited_log_events_;
		return Status::Ok();
	}
	if (log_events_.size() >= flow_control_policy_.max_pending_log_events) {
		log_events_.pop_front();
		++dropped_log_events_;
	}
	log_events_.push_back(event);
	if (audit_writer_) {
		auto audit_st = audit_writer_->write(buildAuditRecord(
			"log_event",
			event.event_id,
			event.session_id,
			event.trace_id,
			event.task_id,
			event.reported_at_ms,
			event.fields_json));
		if (!audit_st.ok()) {
			LOG_WARN("gateway(host): emit log_event audit failed, code={}",
			         static_cast<int>(audit_st.code));
		}
	}
	LOG_DEBUG("gateway(host): log event submitted, id={}, level={}, {}",
	          event.event_id,
	          event.level,
	          buildContextTag(event.session_id, event.trace_id, event.task_id));
	return Status::Ok();
}

Result<GatewayLogEvent> GatewayHostServer::dequeueLogEvent()
{
	if (!running_.load()) {
		return Result<GatewayLogEvent>::Error(Status::NOT_INITIALIZED,
		                                      "gateway host server is not running");
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	if (log_events_.empty()) {
		return Result<GatewayLogEvent>::Error(Status::NOT_FOUND, "no pending log event");
	}

	GatewayLogEvent event = std::move(log_events_.front());
	log_events_.pop_front();
	LOG_DEBUG("gateway(host): log event dequeued, id={}, {}",
	          event.event_id,
	          buildContextTag(event.session_id, event.trace_id, event.task_id));
	return Result<GatewayLogEvent>::Ok(std::move(event));
}

size_t GatewayHostServer::droppedLogEventCount() const
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return dropped_log_events_;
}

size_t GatewayHostServer::rateLimitedLogEventCount() const
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return rate_limited_log_events_;
}

size_t GatewayHostServer::droppedRiskEventCount() const
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return dropped_risk_events_;
}

Status GatewayHostServer::setFlowControlPolicy(const FlowControlPolicy& policy)
{
	if (policy.max_pending_log_events == 0 ||
	    policy.max_pending_risk_events == 0 ||
	    policy.per_session_log_rate_per_sec == 0 ||
	    policy.per_session_log_burst == 0) {
		return Status(Status::INVALID_ARGUMENT, "invalid flow control policy");
	}
	std::lock_guard<std::mutex> lk(state_mutex_);
	flow_control_policy_ = policy;
	while (log_events_.size() > flow_control_policy_.max_pending_log_events) {
		log_events_.pop_front();
		++dropped_log_events_;
	}
	while (risk_events_.size() > flow_control_policy_.max_pending_risk_events) {
		risk_events_.pop_front();
		++dropped_risk_events_;
	}
	return Status::Ok();
}

FlowControlPolicy GatewayHostServer::flowControlPolicy() const
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return flow_control_policy_;
}

void GatewayHostServer::setAuditWriter(std::shared_ptr<audit::AuditWriterInterface> writer)
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	audit_writer_ = std::move(writer);
}

Status GatewayHostServer::submitRiskEvent(const GatewayRiskEvent& event)
{
	if (!running_.load()) {
		return Status(Status::NOT_INITIALIZED, "gateway host server is not running");
	}
	if (event.event_id.empty()) {
		return Status(Status::INVALID_ARGUMENT, "risk event_id is empty");
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	if (risk_events_.size() >= flow_control_policy_.max_pending_risk_events) {
		risk_events_.pop_front();
		++dropped_risk_events_;
	}
	risk_events_.push_back(event);
	if (audit_writer_) {
		auto audit_st = audit_writer_->write(buildAuditRecord(
			"risk_event",
			event.event_id,
			event.session_id,
			event.trace_id,
			event.task_id,
			event.reported_at_ms,
			event.extra_json));
		if (!audit_st.ok()) {
			LOG_WARN("gateway(host): emit risk_event audit failed, code={}",
			         static_cast<int>(audit_st.code));
		}
	}
	LOG_WARN("gateway(host): risk event submitted, id={}, severity={}, {}",
	         event.event_id,
	         event.severity,
	         buildContextTag(event.session_id, event.trace_id, event.task_id));
	return Status::Ok();
}

Result<GatewayRiskEvent> GatewayHostServer::dequeueRiskEvent()
{
	if (!running_.load()) {
		return Result<GatewayRiskEvent>::Error(Status::NOT_INITIALIZED,
		                                       "gateway host server is not running");
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	if (risk_events_.empty()) {
		return Result<GatewayRiskEvent>::Error(Status::NOT_FOUND, "no pending risk event");
	}

	GatewayRiskEvent event = std::move(risk_events_.front());
	risk_events_.pop_front();
	LOG_DEBUG("gateway(host): risk event dequeued, id={}, {}",
	          event.event_id,
	          buildContextTag(event.session_id, event.trace_id, event.task_id));
	return Result<GatewayRiskEvent>::Ok(std::move(event));
}

std::string GatewayHostServer::generateSessionId()
{
	const uint64_t seq = g_session_seq.fetch_add(1);
	return "gw-host-" + std::to_string(seq);
}

void GatewayHostServer::pruneResultCacheToCapacityLocked()
{
	while (command_results_.size() > idempotency_policy_.max_result_entries) {
		auto oldest_it = command_results_.begin();
		for (auto it = command_results_.begin(); it != command_results_.end(); ++it) {
			if (it->second.completed_at_ms < oldest_it->second.completed_at_ms) {
				oldest_it = it;
			}
		}
		command_results_.erase(oldest_it);
	}
}

bool GatewayHostServer::consumeLogRateTokenLocked(std::string_view session_id, int64_t now_ms)
{
	if (session_id.empty()) {
		return true;
	}

	auto& bucket = log_rate_buckets_[std::string(session_id)];
	if (bucket.last_refill_ms == 0) {
		bucket.last_refill_ms = now_ms > 0 ? now_ms : 1;
		bucket.tokens = static_cast<double>(flow_control_policy_.per_session_log_burst);
	}

	if (now_ms > bucket.last_refill_ms) {
		const double elapsed_sec =
			static_cast<double>(now_ms - bucket.last_refill_ms) / 1000.0;
		const double refill =
			elapsed_sec * static_cast<double>(flow_control_policy_.per_session_log_rate_per_sec);
		const double burst = static_cast<double>(flow_control_policy_.per_session_log_burst);
		bucket.tokens = std::min(burst, bucket.tokens + refill);
		bucket.last_refill_ms = now_ms;
	}

	if (bucket.tokens >= 1.0) {
		bucket.tokens -= 1.0;
		return true;
	}
	return false;
}

} // namespace gateway
} // namespace vm_channel
} // namespace clawspan

