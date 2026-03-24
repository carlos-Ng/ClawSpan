// task_registry_test.cc — TaskRegistry 单元测试

#include "core/task_registry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

namespace clawspan {
namespace core {
namespace {

// ── 工厂 / 基本构造 ─────────────────────────────────────────────────────────

TEST(TaskRegistryBasic, DefaultConstructible)
{
	TaskRegistry reg;
	EXPECT_TRUE(reg.activeTasks().empty());
}

// ── beginTask / endTask ────────────────────────────────────────────────────

class TaskRegistryTest : public ::testing::Test
{
protected:
	TaskRegistry reg;
};

TEST_F(TaskRegistryTest, BeginTaskReturnsUniqueIds)
{
	auto id1 = reg.beginTask("desc1", "root1", "", "session1");
	auto id2 = reg.beginTask("desc2", "root2", "", "session1");
	EXPECT_NE(id1, id2);
	EXPECT_EQ(id1.substr(0, 5), "task-");
	EXPECT_EQ(id2.substr(0, 5), "task-");
}

TEST_F(TaskRegistryTest, RootTaskSelfReference)
{
	auto id = reg.beginTask("desc", "root intent", "", "s1");
	const auto* ctx = reg.findTask(id);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->task_id, id);
	EXPECT_EQ(ctx->root_task_id, id);   // root = self
	EXPECT_EQ(ctx->parent_task_id, ""); // no parent
	EXPECT_EQ(ctx->root_description, "root intent");
	EXPECT_EQ(ctx->description, "desc");
	EXPECT_EQ(ctx->session_id, "s1");
	EXPECT_EQ(ctx->op_count, 0u);
}

TEST_F(TaskRegistryTest, RootDescriptionFallsBackToDescription)
{
	auto id = reg.beginTask("my desc", "", "", "s1");
	const auto* ctx = reg.findTask(id);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->root_description, "my desc");
}

TEST_F(TaskRegistryTest, SubTaskInheritsRootFields)
{
	auto parent_id = reg.beginTask("parent desc", "user intent", "", "s1");
	auto child_id = reg.beginTask("child desc", "ignored", parent_id, "s1");

	const auto* child = reg.findTask(child_id);
	ASSERT_NE(child, nullptr);
	EXPECT_EQ(child->parent_task_id, parent_id);
	EXPECT_EQ(child->root_task_id, parent_id);           // inherited
	EXPECT_EQ(child->root_description, "user intent");   // inherited, not overridden
	EXPECT_EQ(child->description, "child desc");          // own description
}

TEST_F(TaskRegistryTest, SubTaskOfSubTaskInheritsGrandparentRoot)
{
	auto root_id = reg.beginTask("root", "intent", "", "s1");
	auto mid_id  = reg.beginTask("mid", "", root_id, "s1");
	auto leaf_id = reg.beginTask("leaf", "", mid_id, "s1");

	const auto* leaf = reg.findTask(leaf_id);
	ASSERT_NE(leaf, nullptr);
	EXPECT_EQ(leaf->root_task_id, root_id);
	EXPECT_EQ(leaf->root_description, "intent");
}

TEST_F(TaskRegistryTest, OrphanSubTaskDegradesToRoot)
{
	// Parent doesn't exist → child becomes root
	auto child_id = reg.beginTask("orphan", "backup intent", "nonexistent-parent", "s1");
	const auto* child = reg.findTask(child_id);
	ASSERT_NE(child, nullptr);
	EXPECT_EQ(child->root_task_id, child_id); // degrades to self
	EXPECT_EQ(child->root_description, "backup intent");
}

TEST_F(TaskRegistryTest, EndTaskRemovesFromActive)
{
	auto id = reg.beginTask("desc", "root", "", "s1");
	EXPECT_EQ(reg.activeTasks().size(), 1u);

	reg.endTask(id);
	EXPECT_TRUE(reg.activeTasks().empty());
	EXPECT_EQ(reg.findTask(id), nullptr);
}

TEST_F(TaskRegistryTest, EndTaskIdempotent)
{
	auto id = reg.beginTask("desc", "root", "", "s1");
	reg.endTask(id);
	// Second endTask should not crash (just warn)
	EXPECT_NO_FATAL_FAILURE(reg.endTask(id));
}

TEST_F(TaskRegistryTest, EndUnknownTaskIdempotent)
{
	EXPECT_NO_FATAL_FAILURE(reg.endTask("unknown-task-id"));
}

TEST_F(TaskRegistryTest, FindTaskReturnsNullForUnknown)
{
	EXPECT_EQ(reg.findTask("nonexistent"), nullptr);
}

// ── Authorization cache ────────────────────────────────────────────────────

TEST_F(TaskRegistryTest, CacheAndCheckAuthorization)
{
	auto id = reg.beginTask("desc", "root", "", "s1");
	IntentFingerprint fp{"cap_ax", "click", "com.example", "AXButton"};

	EXPECT_FALSE(reg.checkAuthorization(id, fp));

	reg.cacheAuthorization(id, fp);
	EXPECT_TRUE(reg.checkAuthorization(id, fp));
}

TEST_F(TaskRegistryTest, AuthorizationCacheIsIdempotent)
{
	auto id = reg.beginTask("desc", "root", "", "s1");
	IntentFingerprint fp{"cap", "op", "scope", "target"};

	reg.cacheAuthorization(id, fp);
	reg.cacheAuthorization(id, fp); // duplicate — should not error
	EXPECT_TRUE(reg.checkAuthorization(id, fp));
}

TEST_F(TaskRegistryTest, AuthorizationNotCrossTask)
{
	auto id1 = reg.beginTask("t1", "root", "", "s1");
	auto id2 = reg.beginTask("t2", "root", "", "s1");
	IntentFingerprint fp{"cap", "op", "scope", "target"};

	reg.cacheAuthorization(id1, fp);
	EXPECT_TRUE(reg.checkAuthorization(id1, fp));
	EXPECT_FALSE(reg.checkAuthorization(id2, fp)); // not cross-task
}

TEST_F(TaskRegistryTest, EndTaskClearsAuthorizations)
{
	auto id = reg.beginTask("desc", "root", "", "s1");
	IntentFingerprint fp{"cap", "op", "scope", "target"};
	reg.cacheAuthorization(id, fp);
	EXPECT_TRUE(reg.checkAuthorization(id, fp));

	reg.endTask(id);
	EXPECT_FALSE(reg.checkAuthorization(id, fp)); // task gone, auth gone
}

TEST_F(TaskRegistryTest, CacheAuthForUnknownTaskSilent)
{
	IntentFingerprint fp{"cap", "op", "scope", "target"};
	EXPECT_NO_FATAL_FAILURE(reg.cacheAuthorization("unknown", fp));
}

TEST_F(TaskRegistryTest, DifferentFingerprintsAreSeparate)
{
	auto id = reg.beginTask("desc", "root", "", "s1");
	IntentFingerprint fp1{"cap", "click", "scope", "btn"};
	IntentFingerprint fp2{"cap", "type",  "scope", "field"};

	reg.cacheAuthorization(id, fp1);
	EXPECT_TRUE(reg.checkAuthorization(id, fp1));
	EXPECT_FALSE(reg.checkAuthorization(id, fp2));
}

// ── incrementOpCount ────────────────────────────────────────────────────────

TEST_F(TaskRegistryTest, IncrementOpCount)
{
	auto id = reg.beginTask("desc", "root", "", "s1");
	const auto* ctx = reg.findTask(id);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->op_count, 0u);

	reg.incrementOpCount(id);
	reg.incrementOpCount(id);
	reg.incrementOpCount(id);
	EXPECT_EQ(ctx->op_count, 3u);
}

TEST_F(TaskRegistryTest, IncrementOpCountUnknownTaskSilent)
{
	EXPECT_NO_FATAL_FAILURE(reg.incrementOpCount("unknown"));
}

// ── activeTasks ─────────────────────────────────────────────────────────────

TEST_F(TaskRegistryTest, ActiveTasksReturnsAll)
{
	auto id1 = reg.beginTask("t1", "", "", "s1");
	auto id2 = reg.beginTask("t2", "", "", "s1");
	auto id3 = reg.beginTask("t3", "", "", "s1");

	auto active = reg.activeTasks();
	EXPECT_EQ(active.size(), 3u);

	std::sort(active.begin(), active.end());
	std::vector<std::string> expected{id1, id2, id3};
	std::sort(expected.begin(), expected.end());
	EXPECT_EQ(active, expected);
}

// ── Concurrency (stress test) ───────────────────────────────────────────────

TEST_F(TaskRegistryTest, ConcurrentBeginEndDoesNotCrash)
{
	constexpr int kThreads = 8;
	constexpr int kOpsPerThread = 100;

	auto worker = [this](int thread_id) {
		for (int i = 0; i < kOpsPerThread; ++i) {
			auto id = reg.beginTask(
				"thread-" + std::to_string(thread_id) + "-op-" + std::to_string(i),
				"root", "", "session");
			reg.incrementOpCount(id);
			reg.endTask(id);
		}
	};

	std::vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int t = 0; t < kThreads; ++t) {
		threads.emplace_back(worker, t);
	}
	for (auto& t : threads) {
		t.join();
	}

	EXPECT_TRUE(reg.activeTasks().empty());
}

} // namespace
} // namespace core
} // namespace clawspan
