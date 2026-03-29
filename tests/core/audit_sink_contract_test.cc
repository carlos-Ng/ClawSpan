#include "audit/audit_service.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace clawspan {
namespace audit {
namespace {

std::unique_ptr<AuditSinkInterface> createSinkUnderTest()
{
	return std::make_unique<InMemoryAuditSink>();
}

TEST(AuditSinkContractTest, PolicyValidationAndReadback)
{
	auto sink = createSinkUnderTest();
	ASSERT_NE(sink, nullptr);

	EXPECT_EQ(sink->setPolicy(AuditStoragePolicy{
		.max_records = 0,
	}).code, Status::INVALID_ARGUMENT);

	ASSERT_TRUE(sink->setPolicy(AuditStoragePolicy{
		.max_records = 3,
	}).ok());
	EXPECT_EQ(sink->policy().max_records, 3u);
}

TEST(AuditSinkContractTest, AppendValidationAndAutoRecordId)
{
	auto sink = createSinkUnderTest();
	ASSERT_NE(sink, nullptr);

	EXPECT_EQ(sink->append(AuditRecord{
		.event_type = "",
		.event_id = "evt-1",
	}).code, Status::INVALID_ARGUMENT);

	EXPECT_EQ(sink->append(AuditRecord{
		.event_type = "custom",
		.event_id = "",
	}).code, Status::INVALID_ARGUMENT);

	AuditRecord record{
		.session_id = "s-1",
		.trace_id = "t-1",
		.task_id = "task-1",
		.event_type = "custom",
		.event_id = "evt-2",
		.timestamp_ms = 1000,
		.payload_json = "{}",
	};
	ASSERT_TRUE(sink->append(record).ok());
	auto by_session = sink->queryBySession("s-1", 10);
	ASSERT_EQ(by_session.size(), 1u);
	EXPECT_FALSE(by_session[0].record_id.empty());
}

TEST(AuditSinkContractTest, CapacityEvictsOldest)
{
	auto sink = createSinkUnderTest();
	ASSERT_NE(sink, nullptr);
	ASSERT_TRUE(sink->setPolicy(AuditStoragePolicy{
		.max_records = 2,
	}).ok());

	for (int i = 0; i < 3; ++i) {
		AuditRecord record{
			.session_id = "s-cap",
			.trace_id = "t-cap",
			.task_id = "task-cap",
			.event_type = "custom",
			.event_id = "evt-" + std::to_string(i),
			.timestamp_ms = 1000 + i,
			.payload_json = "{}",
		};
		ASSERT_TRUE(sink->append(record).ok());
	}

	EXPECT_EQ(sink->recordCount(), 2u);
	auto by_session = sink->queryBySession("s-cap", 10);
	ASSERT_EQ(by_session.size(), 2u);
	EXPECT_EQ(by_session[0].event_id, "evt-2");
	EXPECT_EQ(by_session[1].event_id, "evt-1");
}

TEST(AuditSinkContractTest, QueryByTaskLatestFirstAndLimit)
{
	auto sink = createSinkUnderTest();
	ASSERT_NE(sink, nullptr);

	for (int i = 0; i < 4; ++i) {
		AuditRecord record{
			.session_id = "s-query",
			.trace_id = "t-query",
			.task_id = (i == 2) ? "task-other" : "task-main",
			.event_type = "custom",
			.event_id = "evt-q-" + std::to_string(i),
			.timestamp_ms = 2000 + i,
			.payload_json = "{}",
		};
		ASSERT_TRUE(sink->append(record).ok());
	}

	auto by_task = sink->queryByTask("task-main", 2);
	ASSERT_EQ(by_task.size(), 2u);
	EXPECT_EQ(by_task[0].event_id, "evt-q-3");
	EXPECT_EQ(by_task[1].event_id, "evt-q-1");
}

TEST(AuditSinkContractTest, ClearRemovesAllRecords)
{
	auto sink = createSinkUnderTest();
	ASSERT_NE(sink, nullptr);

	ASSERT_TRUE(sink->append(AuditRecord{
		.session_id = "s-clear",
		.trace_id = "t-clear",
		.task_id = "task-clear",
		.event_type = "custom",
		.event_id = "evt-clear",
		.timestamp_ms = 1,
		.payload_json = "{}",
	}).ok());
	ASSERT_EQ(sink->recordCount(), 1u);

	sink->clear();
	EXPECT_EQ(sink->recordCount(), 0u);
	EXPECT_EQ(sink->queryBySession("s-clear", 10).size(), 0u);
	EXPECT_EQ(sink->policy().max_records, 10000u);
}

} // namespace
} // namespace audit
} // namespace clawspan
