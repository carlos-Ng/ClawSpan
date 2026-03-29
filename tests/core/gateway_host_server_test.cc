#include "vm_channel/gateway/gateway_host_server.h"
#include "audit/audit_service.h"

#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace clawspan {
namespace vm_channel {
namespace gateway {
namespace {

class CollectingAuditWriter final : public audit::AuditWriterInterface
{
public:
	Status write(const audit::AuditRecord& record) override
	{
		records.push_back(record);
		return Status::Ok();
	}

	std::vector<audit::AuditRecord> records;
};

TEST(GatewayHostServerTest, StartStopLifecycle)
{
	GatewayHostServer server;
	EXPECT_FALSE(server.isRunning());

	auto st = server.start("vsock://vm-channel-gateway");
	EXPECT_TRUE(st.ok());
	EXPECT_TRUE(server.isRunning());
	EXPECT_EQ(server.listenTarget(), "vsock://vm-channel-gateway");

	server.stop();
	EXPECT_FALSE(server.isRunning());
}

TEST(GatewayHostServerTest, StartRejectsEmptyTarget)
{
	GatewayHostServer server;
	auto st = server.start("");
	EXPECT_EQ(st.code, Status::INVALID_ARGUMENT);
	EXPECT_FALSE(server.isRunning());
}

TEST(GatewayHostServerTest, ValidateEnvelopeVersion)
{
	GatewayHostServer server;

	EXPECT_TRUE(server.validateEnvelopeVersion("v1").ok());
	EXPECT_EQ(server.validateEnvelopeVersion("").code, Status::INVALID_ARGUMENT);
	EXPECT_EQ(server.validateEnvelopeVersion("v2").code, Status::NOT_SUPPORTED);
}

TEST(GatewayHostServerTest, DoubleStartReturnsError)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	auto st = server.start("vsock://gateway-2");
	EXPECT_EQ(st.code, Status::INTERNAL_ERROR);
	EXPECT_TRUE(server.isRunning());
	EXPECT_EQ(server.listenTarget(), "vsock://gateway");
}

TEST(GatewayHostServerTest, RegisterAndRefreshSessionHeartbeat)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	auto st = server.registerSession("session-1", 1000);
	EXPECT_TRUE(st.ok());
	EXPECT_EQ(server.sessionCount(), 1u);

	st = server.refreshSessionHeartbeat("session-1", 1500);
	EXPECT_TRUE(st.ok());
	EXPECT_EQ(server.sessionCount(), 1u);
}

TEST(GatewayHostServerTest, RefreshHeartbeatReturnsNotFoundForUnknownSession)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	auto st = server.refreshSessionHeartbeat("missing-session", 1500);
	EXPECT_EQ(st.code, Status::NOT_FOUND);
}

TEST(GatewayHostServerTest, SweepExpiredSessionsRemovesTimeoutOnes)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	ASSERT_TRUE(server.registerSession("s-old", 1000).ok());
	ASSERT_TRUE(server.registerSession("s-live", 3000).ok());

	const size_t removed = server.sweepExpiredSessions(5000, 2500);
	EXPECT_EQ(removed, 1u);
	EXPECT_EQ(server.sessionCount(), 1u);
}

TEST(GatewayHostServerTest, RegisterRequiresRunningServer)
{
	GatewayHostServer server;
	auto st = server.registerSession("session-1", 1000);
	EXPECT_EQ(st.code, Status::NOT_INITIALIZED);
}

TEST(GatewayHostServerTest, HandleHandshakeRegistersAndReturnsAck)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	auto ack = server.handleHandshake("session-handshake", 2000);
	ASSERT_TRUE(ack.success());
	EXPECT_TRUE(ack.value().accepted);
	EXPECT_EQ(ack.value().session_id, "session-handshake");
	EXPECT_EQ(ack.value().message, "handshake accepted");
	EXPECT_EQ(server.sessionCount(), 1u);
}

TEST(GatewayHostServerTest, HandleHandshakeAllocatesSessionWhenMissing)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	auto ack = server.handleHandshake("", 2200);
	ASSERT_TRUE(ack.success());
	EXPECT_TRUE(ack.value().accepted);
	EXPECT_FALSE(ack.value().session_id.empty());
	EXPECT_EQ(server.sessionCount(), 1u);
}

TEST(GatewayHostServerTest, HandleHandshakeFailsWhenStopped)
{
	GatewayHostServer server;
	auto ack = server.handleHandshake("session-1", 1000);
	ASSERT_TRUE(ack.failure());
	EXPECT_EQ(ack.error().code, Status::NOT_INITIALIZED);
}

TEST(GatewayHostServerTest, CommandQueueRoundTrip)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	GatewayCommand command{
		.session_id = "session-1",
		.trace_id = "trace-1",
		.task_id = "task-1",
		.command_id = "cmd-1",
		.capability = "capability_ax",
		.operation = "click",
		.params_json = R"({"element_path":"root/button"})",
		.enqueued_at_ms = 1000,
	};

	ASSERT_TRUE(server.enqueueCommand(command).ok());

	auto dequeued = server.dequeueCommand();
	ASSERT_TRUE(dequeued.success());
	EXPECT_EQ(dequeued.value().command_id, "cmd-1");
	EXPECT_EQ(dequeued.value().operation, "click");
	EXPECT_EQ(dequeued.value().trace_id, "trace-1");
	EXPECT_EQ(dequeued.value().task_id, "task-1");

	auto none = server.dequeueCommand();
	ASSERT_TRUE(none.failure());
	EXPECT_EQ(none.error().code, Status::NOT_FOUND);
}

TEST(GatewayHostServerTest, DuplicateCommandIdIsIdempotentInQueueAndInflight)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	GatewayCommand command{
		.session_id = "session-1",
		.trace_id = "trace-dup-1",
		.task_id = "task-dup-1",
		.command_id = "cmd-dup-1",
		.capability = "capability_ax",
		.operation = "click",
		.params_json = R"({"element_path":"root/button"})",
		.enqueued_at_ms = 1000,
	};

	ASSERT_TRUE(server.enqueueCommand(command).ok());
	ASSERT_TRUE(server.enqueueCommand(command).ok()); // duplicate in queue

	auto first = server.dequeueCommand();
	ASSERT_TRUE(first.success());
	EXPECT_EQ(first.value().command_id, "cmd-dup-1");

	ASSERT_TRUE(server.enqueueCommand(command).ok()); // duplicate while inflight

	auto none = server.dequeueCommand();
	ASSERT_TRUE(none.failure());
	EXPECT_EQ(none.error().code, Status::NOT_FOUND);
}

TEST(GatewayHostServerTest, CommandResultStoreAndTake)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	GatewayCommandResult result{
		.session_id = "session-1",
		.trace_id = "trace-1",
		.task_id = "task-1",
		.command_id = "cmd-1",
		.success = true,
		.error_code = 0,
		.error_message = "",
		.result_json = R"({"clicked":true})",
		.completed_at_ms = 1200,
	};

	ASSERT_TRUE(server.submitCommandResult(result).ok());

	auto taken = server.takeCommandResult("cmd-1");
	ASSERT_TRUE(taken.success());
	EXPECT_TRUE(taken.value().success);
	EXPECT_EQ(taken.value().result_json, R"({"clicked":true})");
	EXPECT_EQ(taken.value().trace_id, "trace-1");
	EXPECT_EQ(taken.value().task_id, "task-1");

	auto missing = server.takeCommandResult("cmd-1");
	ASSERT_TRUE(missing.failure());
	EXPECT_EQ(missing.error().code, Status::NOT_FOUND);
}

TEST(GatewayHostServerTest, DuplicateCommandIdAfterResultDoesNotReenqueue)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	GatewayCommand command{
		.session_id = "session-1",
		.trace_id = "trace-dup-2",
		.task_id = "task-dup-2",
		.command_id = "cmd-dup-2",
		.capability = "capability_ax",
		.operation = "click",
		.params_json = R"({"element_path":"root/button"})",
		.enqueued_at_ms = 1000,
	};
	ASSERT_TRUE(server.enqueueCommand(command).ok());
	ASSERT_TRUE(server.dequeueCommand().success());

	GatewayCommandResult result{
		.session_id = "session-1",
		.trace_id = "trace-dup-2",
		.task_id = "task-dup-2",
		.command_id = "cmd-dup-2",
		.success = true,
		.result_json = R"({"clicked":true})",
		.completed_at_ms = 1100,
	};
	ASSERT_TRUE(server.submitCommandResult(result).ok());

	ASSERT_TRUE(server.enqueueCommand(command).ok()); // duplicate after result cached
	auto none = server.dequeueCommand();
	ASSERT_TRUE(none.failure());
	EXPECT_EQ(none.error().code, Status::NOT_FOUND);
}

TEST(GatewayHostServerTest, IdempotencyCachePolicyValidation)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	EXPECT_EQ(
		server.setIdempotencyCachePolicy(IdempotencyCachePolicy{
			.max_result_entries = 0,
			.result_ttl_ms = 1000,
		}).code,
		Status::INVALID_ARGUMENT);
	EXPECT_EQ(
		server.setIdempotencyCachePolicy(IdempotencyCachePolicy{
			.max_result_entries = 4,
			.result_ttl_ms = 0,
		}).code,
		Status::INVALID_ARGUMENT);

	ASSERT_TRUE(server.setIdempotencyCachePolicy(IdempotencyCachePolicy{
		.max_result_entries = 4,
		.result_ttl_ms = 2000,
	}).ok());
	auto policy = server.idempotencyCachePolicy();
	EXPECT_EQ(policy.max_result_entries, 4u);
	EXPECT_EQ(policy.result_ttl_ms, 2000);
}

TEST(GatewayHostServerTest, IdempotencyCacheEvictsOldestByCapacity)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());
	ASSERT_TRUE(server.setIdempotencyCachePolicy(IdempotencyCachePolicy{
		.max_result_entries = 2,
		.result_ttl_ms = 10000,
	}).ok());

	for (int i = 1; i <= 3; ++i) {
		GatewayCommandResult result{
			.session_id = "session-1",
			.trace_id = "trace-capacity",
			.task_id = "task-capacity",
			.command_id = "cmd-cap-" + std::to_string(i),
			.success = true,
			.result_json = "{}",
			.completed_at_ms = 1000 + i,
		};
		ASSERT_TRUE(server.submitCommandResult(result).ok());
	}

	EXPECT_EQ(server.idempotencyResultCount(), 2u);
	EXPECT_TRUE(server.takeCommandResult("cmd-cap-1").failure());
	EXPECT_TRUE(server.takeCommandResult("cmd-cap-2").success());
	EXPECT_TRUE(server.takeCommandResult("cmd-cap-3").success());
}

TEST(GatewayHostServerTest, IdempotencyCacheSweepsExpiredResultsByTtl)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());
	ASSERT_TRUE(server.setIdempotencyCachePolicy(IdempotencyCachePolicy{
		.max_result_entries = 8,
		.result_ttl_ms = 1000,
	}).ok());

	GatewayCommandResult old_result{
		.session_id = "session-1",
		.trace_id = "trace-ttl",
		.task_id = "task-ttl",
		.command_id = "cmd-old",
		.success = true,
		.result_json = "{}",
		.completed_at_ms = 1000,
	};
	GatewayCommandResult new_result{
		.session_id = "session-1",
		.trace_id = "trace-ttl",
		.task_id = "task-ttl",
		.command_id = "cmd-new",
		.success = true,
		.result_json = "{}",
		.completed_at_ms = 2500,
	};
	ASSERT_TRUE(server.submitCommandResult(old_result).ok());
	ASSERT_TRUE(server.submitCommandResult(new_result).ok());

	const size_t removed = server.sweepExpiredCommandResults(3001);
	EXPECT_EQ(removed, 1u);
	EXPECT_EQ(server.idempotencyResultCount(), 1u);
	EXPECT_TRUE(server.takeCommandResult("cmd-old").failure());
	EXPECT_TRUE(server.takeCommandResult("cmd-new").success());
}

TEST(GatewayHostServerTest, FlowControlPolicyValidationAndReadback)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	EXPECT_EQ(
		server.setFlowControlPolicy(FlowControlPolicy{
			.max_pending_log_events = 0,
			.max_pending_risk_events = 8,
			.per_session_log_rate_per_sec = 10,
			.per_session_log_burst = 10,
		}).code,
		Status::INVALID_ARGUMENT);

	ASSERT_TRUE(server.setFlowControlPolicy(FlowControlPolicy{
		.max_pending_log_events = 64,
		.max_pending_risk_events = 32,
		.per_session_log_rate_per_sec = 20,
		.per_session_log_burst = 5,
	}).ok());

	auto policy = server.flowControlPolicy();
	EXPECT_EQ(policy.max_pending_log_events, 64u);
	EXPECT_EQ(policy.max_pending_risk_events, 32u);
	EXPECT_EQ(policy.per_session_log_rate_per_sec, 20u);
	EXPECT_EQ(policy.per_session_log_burst, 5u);
}

TEST(GatewayHostServerTest, PerSessionLogRateLimitDropsExcessBursts)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());
	ASSERT_TRUE(server.setFlowControlPolicy(FlowControlPolicy{
		.max_pending_log_events = 64,
		.max_pending_risk_events = 32,
		.per_session_log_rate_per_sec = 2,
		.per_session_log_burst = 2,
	}).ok());

	for (int i = 0; i < 3; ++i) {
		GatewayLogEvent event{
			.session_id = "session-rate-1",
			.trace_id = "trace-rate",
			.task_id = "task-rate",
			.event_id = "rate-" + std::to_string(i),
			.level = "info",
			.source = "vm-agent",
			.message = "rate-limit test",
			.fields_json = "{}",
			.reported_at_ms = 1000,
		};
		ASSERT_TRUE(server.submitLogEvent(event).ok());
	}

	EXPECT_EQ(server.rateLimitedLogEventCount(), 1u);
}

TEST(GatewayHostServerTest, PerSessionLogRateLimitRefillsByElapsedTime)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());
	ASSERT_TRUE(server.setFlowControlPolicy(FlowControlPolicy{
		.max_pending_log_events = 64,
		.max_pending_risk_events = 32,
		.per_session_log_rate_per_sec = 2,
		.per_session_log_burst = 2,
	}).ok());

	GatewayLogEvent e1{
		.session_id = "session-rate-2",
		.trace_id = "trace-rate",
		.task_id = "task-rate",
		.event_id = "r1",
		.level = "info",
		.source = "vm-agent",
		.message = "m1",
		.fields_json = "{}",
		.reported_at_ms = 1000,
	};
	GatewayLogEvent e2 = e1;
	e2.event_id = "r2";
	GatewayLogEvent e3 = e1;
	e3.event_id = "r3";
	GatewayLogEvent e4 = e1;
	e4.event_id = "r4";
	e4.reported_at_ms = 2000; // +1s => refill 2 tokens

	ASSERT_TRUE(server.submitLogEvent(e1).ok());
	ASSERT_TRUE(server.submitLogEvent(e2).ok());
	ASSERT_TRUE(server.submitLogEvent(e3).ok()); // rate-limited
	ASSERT_TRUE(server.submitLogEvent(e4).ok()); // accepted after refill

	EXPECT_EQ(server.rateLimitedLogEventCount(), 1u);
}

TEST(GatewayHostServerTest, LogEventRoundTrip)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	GatewayLogEvent event{
		.session_id = "session-1",
		.trace_id = "trace-log-1",
		.task_id = "task-log-1",
		.event_id = "log-1",
		.level = "info",
		.source = "vm-agent",
		.message = "hello log",
		.fields_json = R"({"k":"v"})",
		.reported_at_ms = 3000,
	};
	ASSERT_TRUE(server.submitLogEvent(event).ok());

	auto dequeued = server.dequeueLogEvent();
	ASSERT_TRUE(dequeued.success());
	EXPECT_EQ(dequeued.value().event_id, "log-1");
	EXPECT_EQ(dequeued.value().message, "hello log");
	EXPECT_EQ(dequeued.value().trace_id, "trace-log-1");
	EXPECT_EQ(dequeued.value().task_id, "task-log-1");

	auto none = server.dequeueLogEvent();
	ASSERT_TRUE(none.failure());
	EXPECT_EQ(none.error().code, Status::NOT_FOUND);
}

TEST(GatewayHostServerTest, LogEventBackpressureDropsOldestWhenQueueFull)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());
	ASSERT_TRUE(server.setFlowControlPolicy(FlowControlPolicy{
		.max_pending_log_events = GatewayHostServer::MAX_PENDING_LOG_EVENTS,
		.max_pending_risk_events = GatewayHostServer::MAX_PENDING_RISK_EVENTS,
		.per_session_log_rate_per_sec = 5000,
		.per_session_log_burst = 5000,
	}).ok());

	for (size_t i = 0; i < GatewayHostServer::MAX_PENDING_LOG_EVENTS + 3; ++i) {
		GatewayLogEvent event{
			.session_id = "session-1",
			.trace_id = "trace-log-pressure",
			.task_id = "task-log-pressure",
			.event_id = "log-" + std::to_string(i),
			.level = "info",
			.source = "vm-agent",
			.message = "log message",
			.fields_json = "{}",
			.reported_at_ms = static_cast<int64_t>(1000 + i),
		};
		ASSERT_TRUE(server.submitLogEvent(event).ok());
	}

	EXPECT_EQ(server.droppedLogEventCount(), 3u);

	auto first = server.dequeueLogEvent();
	ASSERT_TRUE(first.success());
	EXPECT_EQ(first.value().event_id, "log-3");
}

TEST(GatewayHostServerTest, RiskEventNotBlockedUnderLogPressure)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());
	ASSERT_TRUE(server.setFlowControlPolicy(FlowControlPolicy{
		.max_pending_log_events = GatewayHostServer::MAX_PENDING_LOG_EVENTS,
		.max_pending_risk_events = GatewayHostServer::MAX_PENDING_RISK_EVENTS,
		.per_session_log_rate_per_sec = 5000,
		.per_session_log_burst = 5000,
	}).ok());

	for (size_t i = 0; i < GatewayHostServer::MAX_PENDING_LOG_EVENTS + 10; ++i) {
		GatewayLogEvent event{
			.session_id = "session-1",
			.trace_id = "trace-log-pressure",
			.task_id = "task-log-pressure",
			.event_id = "log-" + std::to_string(i),
			.level = "info",
			.source = "vm-agent",
			.message = "log message",
			.fields_json = "{}",
			.reported_at_ms = static_cast<int64_t>(1000 + i),
		};
		ASSERT_TRUE(server.submitLogEvent(event).ok());
	}

	GatewayRiskEvent risk{
		.session_id = "session-1",
		.trace_id = "trace-risk-priority",
		.task_id = "task-risk-priority",
		.event_id = "risk-priority-1",
		.severity = "high",
		.behavior_type = "file_delete",
		.detail = "priority risk event",
		.extra_json = "{}",
		.reported_at_ms = 5000,
	};
	ASSERT_TRUE(server.submitRiskEvent(risk).ok());

	auto risk_event = server.dequeueRiskEvent();
	ASSERT_TRUE(risk_event.success());
	EXPECT_EQ(risk_event.value().event_id, "risk-priority-1");
	EXPECT_EQ(risk_event.value().severity, "high");
	EXPECT_GE(server.droppedLogEventCount(), 1u);
}

TEST(GatewayHostServerTest, RiskEventBackpressureDropsOldestWhenQueueFull)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());
	ASSERT_TRUE(server.setFlowControlPolicy(FlowControlPolicy{
		.max_pending_log_events = 64,
		.max_pending_risk_events = 2,
		.per_session_log_rate_per_sec = 100,
		.per_session_log_burst = 100,
	}).ok());

	for (int i = 0; i < 3; ++i) {
		GatewayRiskEvent event{
			.session_id = "session-1",
			.trace_id = "trace-risk-pressure",
			.task_id = "task-risk-pressure",
			.event_id = "risk-" + std::to_string(i),
			.severity = "high",
			.behavior_type = "file_delete",
			.detail = "risk message",
			.extra_json = "{}",
			.reported_at_ms = 1000 + i,
		};
		ASSERT_TRUE(server.submitRiskEvent(event).ok());
	}

	EXPECT_EQ(server.droppedRiskEventCount(), 1u);
	auto first = server.dequeueRiskEvent();
	ASSERT_TRUE(first.success());
	EXPECT_EQ(first.value().event_id, "risk-1");
}

TEST(GatewayHostServerTest, RiskEventRoundTrip)
{
	GatewayHostServer server;
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	GatewayRiskEvent event{
		.session_id = "session-1",
		.trace_id = "trace-risk-1",
		.task_id = "task-risk-1",
		.event_id = "risk-1",
		.severity = "high",
		.behavior_type = "file_delete",
		.detail = "attempt delete system file",
		.extra_json = R"({"path":"C:\\Windows\\System32"})",
		.reported_at_ms = 3300,
	};
	ASSERT_TRUE(server.submitRiskEvent(event).ok());

	auto dequeued = server.dequeueRiskEvent();
	ASSERT_TRUE(dequeued.success());
	EXPECT_EQ(dequeued.value().event_id, "risk-1");
	EXPECT_EQ(dequeued.value().severity, "high");
	EXPECT_EQ(dequeued.value().trace_id, "trace-risk-1");
	EXPECT_EQ(dequeued.value().task_id, "task-risk-1");

	auto none = server.dequeueRiskEvent();
	ASSERT_TRUE(none.failure());
	EXPECT_EQ(none.error().code, Status::NOT_FOUND);
}

TEST(GatewayHostServerTest, BuiltinPathsAppendAuditRecords)
{
	GatewayHostServer server;
	auto writer = std::make_shared<CollectingAuditWriter>();
	server.setAuditWriter(writer);
	ASSERT_TRUE(server.start("vsock://gateway").ok());

	GatewayCommandResult result{
		.session_id = "session-audit-3",
		.trace_id = "trace-audit-3",
		.task_id = "task-audit-3",
		.command_id = "cmd-audit-3",
		.success = true,
		.result_json = R"({"ok":true})",
		.completed_at_ms = 1000,
	};
	ASSERT_TRUE(server.submitCommandResult(result).ok());

	GatewayLogEvent log{
		.session_id = "session-audit-3",
		.trace_id = "trace-audit-3",
		.task_id = "task-audit-3",
		.event_id = "log-audit-3",
		.level = "info",
		.source = "vm",
		.message = "hello",
		.fields_json = R"({"k":"v"})",
		.reported_at_ms = 1100,
	};
	ASSERT_TRUE(server.submitLogEvent(log).ok());

	GatewayRiskEvent risk{
		.session_id = "session-audit-3",
		.trace_id = "trace-audit-3",
		.task_id = "task-audit-3",
		.event_id = "risk-audit-3",
		.severity = "high",
		.behavior_type = "x",
		.detail = "y",
		.extra_json = "{}",
		.reported_at_ms = 1200,
	};
	ASSERT_TRUE(server.submitRiskEvent(risk).ok());

	ASSERT_EQ(writer->records.size(), 3u);
	EXPECT_EQ(writer->records[0].event_type, "command_result");
	EXPECT_EQ(writer->records[1].event_type, "log_event");
	EXPECT_EQ(writer->records[2].event_type, "risk_event");
}

} // namespace
} // namespace gateway
} // namespace vm_channel
} // namespace clawspan

