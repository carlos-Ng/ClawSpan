// security_chain_test.cc — SecurityChain 单元测试
//
// 使用 mock SecurityModuleInterface 验证：
//   - priority 排序
//   - 裁决合并逻辑（Deny > NeedConfirm > Pass > Skip）
//   - Deny 短路
//   - reason 传播
//   - 任务生命周期 hooks

#include "core/base/security.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace clawspan {
namespace core {
namespace {

// ── Mock SecurityModule ─────────────────────────────────────────────────────

class MockSecurityModule : public SecurityModuleInterface
{
public:
	MockSecurityModule(const char* mod_name,
	                   SecurityAction pre_action,
	                   SecurityAction post_action = SecurityAction::Skip,
	                   SecurityAction task_begin_action = SecurityAction::Skip)
		: name_(mod_name)
		, pre_action_(pre_action)
		, post_action_(post_action)
		, task_begin_action_(task_begin_action)
	{}

	const char* name()    const override { return name_; }
	const char* version() const override { return "1.0.0"; }
	Result<void> init(const ModuleConfig&) override { return Result<void>::Ok(); }
	void release() override {}

	SecurityAction preHook(const SecurityContext& /*ctx*/, std::string& reason) override
	{
		++pre_call_count_;
		reason = std::string(name_) + "-pre-reason";
		return pre_action_;
	}

	SecurityAction postHook(const SecurityContext& /*ctx*/,
	                         nlohmann::json& /*response*/,
	                         std::string& reason) override
	{
		++post_call_count_;
		reason = std::string(name_) + "-post-reason";
		return post_action_;
	}

	SecurityAction onTaskBegin(const TaskContext& /*task*/, std::string& reason) override
	{
		++task_begin_call_count_;
		reason = std::string(name_) + "-task-begin-reason";
		return task_begin_action_;
	}

	void onTaskEnd(const std::string& /*task_id*/, bool /*success*/) override
	{
		++task_end_call_count_;
	}

	void onCapabilityRegistered(const std::string& /*cap_name*/,
	                             const PluginSecurityDecl& /*decl*/) override
	{
		++cap_registered_call_count_;
	}

	int pre_call_count_  = 0;
	int post_call_count_ = 0;
	int task_begin_call_count_ = 0;
	int task_end_call_count_ = 0;
	int cap_registered_call_count_ = 0;

private:
	const char* name_;
	SecurityAction pre_action_;
	SecurityAction post_action_;
	SecurityAction task_begin_action_;
};

// Helper to create a SecurityContext with minimal fields
SecurityContext makeCtx()
{
	static nlohmann::json empty_params = nlohmann::json::object();
	return SecurityContext{
		"op-1",          // operation_id
		"test_cap",      // capability_name
		"test_op",       // operation
		empty_params,    // params
		false,           // is_readonly
		nullptr          // task
	};
}

// ── registerModule ──────────────────────────────────────────────────────────

TEST(SecurityChainRegister, NullModuleIgnored)
{
	SecurityChain chain;
	EXPECT_NO_FATAL_FAILURE(chain.registerModule(nullptr));
}

// ── runPreHook ──────────────────────────────────────────────────────────────

class SecurityChainPreHookTest : public ::testing::Test
{
protected:
	SecurityChain chain;
};

TEST_F(SecurityChainPreHookTest, NoModulesReturnsPass)
{
	auto ctx = makeCtx();
	std::string reason;
	auto action = chain.runPreHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::Pass);
}

TEST_F(SecurityChainPreHookTest, SinglePassModule)
{
	MockSecurityModule mod("pass_mod", SecurityAction::Pass);
	chain.registerModule(&mod, 100);

	auto ctx = makeCtx();
	std::string reason;
	auto action = chain.runPreHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::Pass);
	EXPECT_EQ(mod.pre_call_count_, 1);
}

TEST_F(SecurityChainPreHookTest, SingleDenyModule)
{
	MockSecurityModule mod("deny_mod", SecurityAction::Deny);
	chain.registerModule(&mod, 100);

	auto ctx = makeCtx();
	std::string reason;
	auto action = chain.runPreHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::Deny);
	EXPECT_EQ(reason, "deny_mod-pre-reason");
}

TEST_F(SecurityChainPreHookTest, DenyShortCircuits)
{
	MockSecurityModule deny_mod("deny", SecurityAction::Deny);
	MockSecurityModule pass_mod("pass", SecurityAction::Pass);
	chain.registerModule(&deny_mod, 10);  // runs first
	chain.registerModule(&pass_mod, 20);  // should NOT run

	auto ctx = makeCtx();
	std::string reason;
	auto action = chain.runPreHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::Deny);
	EXPECT_EQ(deny_mod.pre_call_count_, 1);
	EXPECT_EQ(pass_mod.pre_call_count_, 0); // short-circuited
}

TEST_F(SecurityChainPreHookTest, NeedConfirmWinsOverPass)
{
	MockSecurityModule pass_mod("pass", SecurityAction::Pass);
	MockSecurityModule confirm_mod("confirm", SecurityAction::NeedConfirm);
	chain.registerModule(&pass_mod, 10);
	chain.registerModule(&confirm_mod, 20);

	auto ctx = makeCtx();
	std::string reason;
	auto action = chain.runPreHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::NeedConfirm);
	EXPECT_EQ(reason, "confirm-pre-reason");
}

TEST_F(SecurityChainPreHookTest, DenyWinsOverNeedConfirm)
{
	MockSecurityModule confirm_mod("confirm", SecurityAction::NeedConfirm);
	MockSecurityModule deny_mod("deny", SecurityAction::Deny);
	chain.registerModule(&confirm_mod, 10);
	chain.registerModule(&deny_mod, 20);

	auto ctx = makeCtx();
	std::string reason;
	auto action = chain.runPreHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::Deny);
	EXPECT_EQ(reason, "deny-pre-reason");
	// confirm_mod ran but deny_mod overrides
	EXPECT_EQ(confirm_mod.pre_call_count_, 1);
}

TEST_F(SecurityChainPreHookTest, SkipDoesNotAffectResult)
{
	MockSecurityModule skip_mod("skip", SecurityAction::Skip);
	MockSecurityModule pass_mod("pass", SecurityAction::Pass);
	chain.registerModule(&skip_mod, 10);
	chain.registerModule(&pass_mod, 20);

	auto ctx = makeCtx();
	std::string reason;
	auto action = chain.runPreHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::Pass);
}

TEST_F(SecurityChainPreHookTest, PriorityOrderRespected)
{
	// Modules registered out of order, should execute in priority order
	MockSecurityModule mod_high("high", SecurityAction::NeedConfirm);
	MockSecurityModule mod_low("low", SecurityAction::Deny);
	chain.registerModule(&mod_low, 200);
	chain.registerModule(&mod_high, 50);

	auto ctx = makeCtx();
	std::string reason;
	auto action = chain.runPreHook(ctx, reason);
	// high (50) runs first → NeedConfirm
	// low (200) runs second → Deny → short-circuit
	EXPECT_EQ(action, SecurityAction::Deny);
	EXPECT_EQ(mod_high.pre_call_count_, 1);
	EXPECT_EQ(mod_low.pre_call_count_, 1);
}

TEST_F(SecurityChainPreHookTest, FirstNeedConfirmReasonPreserved)
{
	MockSecurityModule c1("first-confirm", SecurityAction::NeedConfirm);
	MockSecurityModule c2("second-confirm", SecurityAction::NeedConfirm);
	chain.registerModule(&c1, 10);
	chain.registerModule(&c2, 20);

	auto ctx = makeCtx();
	std::string reason;
	chain.runPreHook(ctx, reason);
	EXPECT_EQ(reason, "first-confirm-pre-reason"); // first NeedConfirm's reason kept
}

// ── runPostHook ─────────────────────────────────────────────────────────────

TEST(SecurityChainPostHook, DenyInPostHook)
{
	SecurityChain chain;
	MockSecurityModule mod("post_deny", SecurityAction::Pass, SecurityAction::Deny);
	chain.registerModule(&mod, 100);

	auto ctx = makeCtx();
	nlohmann::json response = {{"result", "ok"}};
	std::string reason;
	auto action = chain.runPostHook(ctx, response, reason);
	EXPECT_EQ(action, SecurityAction::Deny);
	EXPECT_EQ(reason, "post_deny-post-reason");
}

// ── runTaskBeginHook ────────────────────────────────────────────────────────

TEST(SecurityChainTaskBegin, AllSkipReturnsPass)
{
	SecurityChain chain;
	MockSecurityModule mod("skip_mod", SecurityAction::Pass, SecurityAction::Skip, SecurityAction::Skip);
	chain.registerModule(&mod, 100);

	TaskContext task;
	task.task_id = "t-1";
	std::string reason;
	auto action = chain.runTaskBeginHook(task, reason);
	EXPECT_EQ(action, SecurityAction::Pass);
}

TEST(SecurityChainTaskBegin, DenyShortCircuits)
{
	SecurityChain chain;
	MockSecurityModule deny_mod("deny", SecurityAction::Pass, SecurityAction::Skip, SecurityAction::Deny);
	MockSecurityModule skip_mod("skip", SecurityAction::Pass, SecurityAction::Skip, SecurityAction::Skip);
	chain.registerModule(&deny_mod, 10);
	chain.registerModule(&skip_mod, 20);

	TaskContext task;
	task.task_id = "t-1";
	std::string reason;
	auto action = chain.runTaskBeginHook(task, reason);
	EXPECT_EQ(action, SecurityAction::Deny);
	EXPECT_EQ(deny_mod.task_begin_call_count_, 1);
	EXPECT_EQ(skip_mod.task_begin_call_count_, 0); // short-circuited
}

// ── runTaskEndHook ──────────────────────────────────────────────────────────

TEST(SecurityChainTaskEnd, AllModulesNotified)
{
	SecurityChain chain;
	MockSecurityModule m1("m1", SecurityAction::Pass);
	MockSecurityModule m2("m2", SecurityAction::Pass);
	chain.registerModule(&m1, 10);
	chain.registerModule(&m2, 20);

	chain.runTaskEndHook("t-1", true);
	EXPECT_EQ(m1.task_end_call_count_, 1);
	EXPECT_EQ(m2.task_end_call_count_, 1);
}

// ── notifyCapabilityRegistered ──────────────────────────────────────────────

TEST(SecurityChainCapRegistered, AllModulesNotified)
{
	SecurityChain chain;
	MockSecurityModule m1("m1", SecurityAction::Pass);
	MockSecurityModule m2("m2", SecurityAction::Pass);
	chain.registerModule(&m1, 10);
	chain.registerModule(&m2, 20);

	PluginSecurityDecl decl;
	chain.notifyCapabilityRegistered("test_cap", decl);
	EXPECT_EQ(m1.cap_registered_call_count_, 1);
	EXPECT_EQ(m2.cap_registered_call_count_, 1);
}

} // namespace
} // namespace core
} // namespace clawspan
