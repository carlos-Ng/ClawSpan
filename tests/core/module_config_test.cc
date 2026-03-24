// module_config_test.cc — ModuleConfig 单元测试

#include "core/base/module_config.h"

#include <gtest/gtest.h>

#include <string>

namespace clawspan {
namespace core {
namespace {

// ── 基本 get/set ──────────────────────────────────────────────────────────

TEST(ModuleConfigBasic, GetMissingKeyReturnsNullopt)
{
	ModuleConfig cfg;
	EXPECT_FALSE(cfg.getString("nonexistent").has_value());
	EXPECT_FALSE(cfg.getInt("nonexistent").has_value());
	EXPECT_FALSE(cfg.getDouble("nonexistent").has_value());
	EXPECT_FALSE(cfg.getBool("nonexistent").has_value());
}

TEST(ModuleConfigBasic, SetAndGetString)
{
	ModuleConfig cfg;
	cfg.set("path", std::string("/usr/bin"));
	auto val = cfg.getString("path");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(*val, "/usr/bin");
}

TEST(ModuleConfigBasic, SetAndGetInt)
{
	ModuleConfig cfg;
	cfg.set("port", int64_t{8080});
	auto val = cfg.getInt("port");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(*val, 8080);
}

TEST(ModuleConfigBasic, SetAndGetDouble)
{
	ModuleConfig cfg;
	cfg.set("ratio", 3.14);
	auto val = cfg.getDouble("ratio");
	ASSERT_TRUE(val.has_value());
	EXPECT_DOUBLE_EQ(*val, 3.14);
}

TEST(ModuleConfigBasic, SetAndGetBool)
{
	ModuleConfig cfg;
	cfg.set("enabled", true);
	auto val = cfg.getBool("enabled");
	ASSERT_TRUE(val.has_value());
	EXPECT_TRUE(*val);
}

// ── 类型不匹配 ──────────────────────────────────────────────────────────────

TEST(ModuleConfigTypeMismatch, GetStringOnIntReturnsNullopt)
{
	ModuleConfig cfg;
	cfg.set("port", int64_t{8080});
	EXPECT_FALSE(cfg.getString("port").has_value());
}

TEST(ModuleConfigTypeMismatch, GetIntOnStringReturnsNullopt)
{
	ModuleConfig cfg;
	cfg.set("name", std::string("test"));
	EXPECT_FALSE(cfg.getInt("name").has_value());
}

TEST(ModuleConfigTypeMismatch, GetBoolOnDoubleReturnsNullopt)
{
	ModuleConfig cfg;
	cfg.set("ratio", 1.5);
	EXPECT_FALSE(cfg.getBool("ratio").has_value());
}

TEST(ModuleConfigTypeMismatch, GetDoubleOnBoolReturnsNullopt)
{
	ModuleConfig cfg;
	cfg.set("flag", true);
	EXPECT_FALSE(cfg.getDouble("flag").has_value());
}

// ── 覆盖写入 ──────────────────────────────────────────────────────────────

TEST(ModuleConfigOverwrite, OverwriteSameType)
{
	ModuleConfig cfg;
	cfg.set("key", std::string("old"));
	cfg.set("key", std::string("new"));
	auto val = cfg.getString("key");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(*val, "new");
}

TEST(ModuleConfigOverwrite, OverwriteDifferentType)
{
	ModuleConfig cfg;
	cfg.set("key", std::string("text"));
	cfg.set("key", int64_t{123});
	// 新类型覆盖后，getString 应返回 nullopt
	EXPECT_FALSE(cfg.getString("key").has_value());
	auto val = cfg.getInt("key");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(*val, 123);
}

// ── 边界值 ──────────────────────────────────────────────────────────────────

TEST(ModuleConfigEdgeCases, EmptyStringKey)
{
	ModuleConfig cfg;
	cfg.set("", std::string("empty key"));
	auto val = cfg.getString("");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(*val, "empty key");
}

TEST(ModuleConfigEdgeCases, EmptyStringValue)
{
	ModuleConfig cfg;
	cfg.set("empty_val", std::string(""));
	auto val = cfg.getString("empty_val");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(*val, "");
}

TEST(ModuleConfigEdgeCases, NegativeInt)
{
	ModuleConfig cfg;
	cfg.set("neg", int64_t{-42});
	auto val = cfg.getInt("neg");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(*val, -42);
}

TEST(ModuleConfigEdgeCases, ZeroDouble)
{
	ModuleConfig cfg;
	cfg.set("zero", 0.0);
	auto val = cfg.getDouble("zero");
	ASSERT_TRUE(val.has_value());
	EXPECT_DOUBLE_EQ(*val, 0.0);
}

TEST(ModuleConfigEdgeCases, FalseBoolean)
{
	ModuleConfig cfg;
	cfg.set("off", false);
	auto val = cfg.getBool("off");
	ASSERT_TRUE(val.has_value());
	EXPECT_FALSE(*val);
}

// ── 多键共存 ──────────────────────────────────────────────────────────────

TEST(ModuleConfigMultiKey, MultipleKeysCoexist)
{
	ModuleConfig cfg;
	cfg.set("name", std::string("test"));
	cfg.set("port", int64_t{9090});
	cfg.set("rate", 0.5);
	cfg.set("debug", true);

	EXPECT_EQ(*cfg.getString("name"), "test");
	EXPECT_EQ(*cfg.getInt("port"), 9090);
	EXPECT_DOUBLE_EQ(*cfg.getDouble("rate"), 0.5);
	EXPECT_TRUE(*cfg.getBool("debug"));
}

} // namespace
} // namespace core
} // namespace clawspan
