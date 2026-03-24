// security_filter_test.cc — SecurityFilter 单元测试
//
// SecurityFilter 的 preHook/matchRule 是纯逻辑（给定规则 + SecurityContext → 裁决）。
// loadRules 需要临时 TOML 文件，使用 std::ofstream 创建。

#include "security_filter.h"
#include "core/base/module_config.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace clawspan {
namespace security {
namespace {

using core::SecurityAction;
using core::SecurityContext;

// Helper: 创建 SecurityContext
SecurityContext makeCtx(std::string_view cap, std::string_view op,
                         const nlohmann::json& params = nlohmann::json::object())
{
	return SecurityContext{
		"op-test",   // operation_id
		cap,         // capability_name
		op,          // operation
		params,      // params
		false,       // is_readonly
		nullptr      // task
	};
}

// Helper: 将 TOML 内容写入临时文件，返回路径
std::string writeTempToml(const std::string& content)
{
	// 使用固定名称在 temp 目录（测试结束后清理）
	std::string path = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".")
	                 + "\\clawspan_security_filter_test.toml";
	std::ofstream f(path, std::ios::trunc);
	f << content;
	f.close();
	return path;
}

// ── 基本 preHook / postHook ─────────────────────────────────────────────────

TEST(SecurityFilterBasic, PostHookAlwaysSkip)
{
	SecurityFilter filter;
	auto ctx = makeCtx("cap", "op");
	nlohmann::json response = {{"result", "ok"}};
	std::string reason;
	auto action = filter.postHook(ctx, response, reason);
	EXPECT_EQ(action, SecurityAction::Skip);
}

TEST(SecurityFilterBasic, NoRulesAllPass)
{
	SecurityFilter filter;
	auto ctx = makeCtx("any_cap", "any_op");
	std::string reason;
	auto action = filter.preHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::Pass);
}

// ── loadRules + preHook ─────────────────────────────────────────────────────

class SecurityFilterRulesTest : public ::testing::Test
{
protected:
	SecurityFilter filter;
	std::string temp_path;

	void TearDown() override
	{
		if (!temp_path.empty()) {
			std::remove(temp_path.c_str());
		}
	}

	void loadFromToml(const std::string& toml_content)
	{
		temp_path = writeTempToml(toml_content);
		core::ModuleConfig cfg;
		cfg.set("rules_file", temp_path);
		auto result = filter.init(cfg);
		ASSERT_TRUE(result.success()) << "loadRules failed";
	}
};

TEST_F(SecurityFilterRulesTest, DenyByCapabilityAndOperation)
{
	loadFromToml(R"(
[[deny]]
capability = "cmd_exec"
operations = ["exec"]
reason = "command execution blocked"
)");

	std::string reason;
	auto ctx = makeCtx("cmd_exec", "exec");
	auto action = filter.preHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::Deny);
	EXPECT_EQ(reason, "command execution blocked");
}

TEST_F(SecurityFilterRulesTest, DenyDoesNotMatchDifferentCapability)
{
	loadFromToml(R"(
[[deny]]
capability = "cmd_exec"
operations = ["exec"]
reason = "blocked"
)");

	std::string reason;
	auto ctx = makeCtx("file_access", "exec");
	auto action = filter.preHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::Pass);
}

TEST_F(SecurityFilterRulesTest, DenyDoesNotMatchDifferentOperation)
{
	loadFromToml(R"(
[[deny]]
capability = "cmd_exec"
operations = ["exec"]
reason = "blocked"
)");

	std::string reason;
	auto ctx = makeCtx("cmd_exec", "list");
	auto action = filter.preHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::Pass);
}

TEST_F(SecurityFilterRulesTest, WildcardCapabilityMatchesAll)
{
	loadFromToml(R"(
[[deny]]
capability = "*"
operations = ["delete"]
reason = "delete is forbidden"
)");

	std::string reason;
	auto ctx = makeCtx("any_capability", "delete");
	auto action = filter.preHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::Deny);
}

TEST_F(SecurityFilterRulesTest, EmptyCapabilityMatchesAll)
{
	loadFromToml(R"(
[[deny]]
capability = ""
operations = ["nuke"]
reason = "nuke is forbidden"
)");

	std::string reason;
	auto ctx = makeCtx("whatever", "nuke");
	auto action = filter.preHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::Deny);
}

TEST_F(SecurityFilterRulesTest, EmptyOperationsMatchesAllOps)
{
	loadFromToml(R"(
[[deny]]
capability = "dangerous_cap"
reason = "entire capability blocked"
)");

	std::string reason;
	auto action1 = filter.preHook(makeCtx("dangerous_cap", "op1"), reason);
	EXPECT_EQ(action1, SecurityAction::Deny);
	auto action2 = filter.preHook(makeCtx("dangerous_cap", "op2"), reason);
	EXPECT_EQ(action2, SecurityAction::Deny);
}

TEST_F(SecurityFilterRulesTest, ConfirmRule)
{
	loadFromToml(R"(
[[confirm]]
capability = "file_access"
operations = ["write"]
reason = "file write needs confirmation"
)");

	std::string reason;
	auto ctx = makeCtx("file_access", "write");
	auto action = filter.preHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::NeedConfirm);
	EXPECT_EQ(reason, "file write needs confirmation");
}

TEST_F(SecurityFilterRulesTest, DenyTakesPriorityOverConfirm)
{
	loadFromToml(R"(
[[deny]]
capability = "cap"
operations = ["op"]
reason = "denied"

[[confirm]]
capability = "cap"
operations = ["op"]
reason = "needs confirm"
)");

	std::string reason;
	auto ctx = makeCtx("cap", "op");
	auto action = filter.preHook(ctx, reason);
	EXPECT_EQ(action, SecurityAction::Deny);
	EXPECT_EQ(reason, "denied");
}

TEST_F(SecurityFilterRulesTest, ParamsFieldPresenceCheck)
{
	loadFromToml(R"(
[[deny]]
capability = "cmd_exec"
operations = ["exec"]
params_field = "cmd"
reason = "must check cmd field"
)");

	// Has the field → match
	{
		std::string reason;
		nlohmann::json params = {{"cmd", "rm -rf /"}};
		auto ctx = makeCtx("cmd_exec", "exec", params);
		auto action = filter.preHook(ctx, reason);
		EXPECT_EQ(action, SecurityAction::Deny);
	}
	// Missing the field → no match
	{
		std::string reason;
		nlohmann::json params = {{"other", "value"}};
		auto ctx = makeCtx("cmd_exec", "exec", params);
		auto action = filter.preHook(ctx, reason);
		EXPECT_EQ(action, SecurityAction::Pass);
	}
}

TEST_F(SecurityFilterRulesTest, ParamsPatternRegexMatch)
{
	loadFromToml(R"(
[[deny]]
capability = "cmd_exec"
operations = ["exec"]
params_field = "cmd"
params_patterns = ["rm\\s+-rf", "sudo"]
reason = "dangerous command"
)");

	// Matches "rm -rf"
	{
		std::string reason;
		nlohmann::json params = {{"cmd", "rm -rf /home"}};
		auto ctx = makeCtx("cmd_exec", "exec", params);
		EXPECT_EQ(filter.preHook(ctx, reason), SecurityAction::Deny);
	}
	// Matches "sudo"
	{
		std::string reason;
		nlohmann::json params = {{"cmd", "sudo apt install"}};
		auto ctx = makeCtx("cmd_exec", "exec", params);
		EXPECT_EQ(filter.preHook(ctx, reason), SecurityAction::Deny);
	}
	// No match
	{
		std::string reason;
		nlohmann::json params = {{"cmd", "ls -la"}};
		auto ctx = makeCtx("cmd_exec", "exec", params);
		EXPECT_EQ(filter.preHook(ctx, reason), SecurityAction::Pass);
	}
}

TEST_F(SecurityFilterRulesTest, ParamsPatternCaseInsensitive)
{
	loadFromToml(R"(
[[deny]]
capability = "cap"
operations = ["op"]
params_field = "text"
params_patterns = ["hello"]
reason = "matched"
)");

	std::string reason;
	nlohmann::json params = {{"text", "HELLO WORLD"}};
	auto ctx = makeCtx("cap", "op", params);
	EXPECT_EQ(filter.preHook(ctx, reason), SecurityAction::Deny);
}

// ── loadRules edge cases ────────────────────────────────────────────────────

TEST(SecurityFilterLoad, MissingRulesFileReturnsError)
{
	SecurityFilter filter;
	core::ModuleConfig cfg;
	cfg.set("rules_file", std::string("C:\\nonexistent\\path\\rules.toml"));
	auto result = filter.init(cfg);
	// toml++ treats file-not-found as parse_error, so init returns failure
	EXPECT_TRUE(result.failure());
}

TEST(SecurityFilterLoad, NoRulesFileConfigured)
{
	SecurityFilter filter;
	core::ModuleConfig cfg;
	auto result = filter.init(cfg);
	EXPECT_TRUE(result.success()); // no config → ok
}

TEST(SecurityFilterLoad, MalformedTomlReturnsError)
{
	SecurityFilter filter;
	std::string path = writeTempToml("this is not valid { toml [[[");
	core::ModuleConfig cfg;
	cfg.set("rules_file", path);
	auto result = filter.init(cfg);
	EXPECT_TRUE(result.failure());
	std::remove(path.c_str());
}

// ── release ─────────────────────────────────────────────────────────────────

TEST(SecurityFilterRelease, ReleasesClearsRules)
{
	SecurityFilter filter;
	std::string path = writeTempToml(R"(
[[deny]]
capability = "cap"
reason = "blocked"
)");
	core::ModuleConfig cfg;
	cfg.set("rules_file", path);
	(void)filter.init(cfg);

	// Before release: should deny
	{
		std::string reason;
		auto ctx = makeCtx("cap", "op");
		EXPECT_EQ(filter.preHook(ctx, reason), SecurityAction::Deny);
	}

	filter.release();

	// After release: all pass
	{
		std::string reason;
		auto ctx = makeCtx("cap", "op");
		EXPECT_EQ(filter.preHook(ctx, reason), SecurityAction::Pass);
	}

	std::remove(path.c_str());
}

} // namespace
} // namespace security
} // namespace clawspan
