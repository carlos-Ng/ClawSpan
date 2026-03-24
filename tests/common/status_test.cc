// status_test.cc — Status / statusMessage / Result<T> / Result<void> 单元测试

#include "common/error.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <utility>

namespace clawspan {
namespace {

// ── Status 基本操作 ────────────────────────────────────────────────────────

TEST(StatusBasic, DefaultIsOk)
{
	Status s;
	EXPECT_TRUE(s.ok());
	EXPECT_EQ(s.code, Status::OK);
}

TEST(StatusBasic, StaticOkFactory)
{
	auto s = Status::Ok();
	EXPECT_TRUE(s.ok());
	EXPECT_EQ(s.code, Status::OK);
}

TEST(StatusBasic, StaticMessageConstructor)
{
	Status s(Status::IO_ERROR, "disk is full");
	EXPECT_FALSE(s.ok());
	EXPECT_EQ(s.code, Status::IO_ERROR);
	EXPECT_STREQ(s.message, "disk is full");
}

TEST(StatusBasic, DynamicMessageConstructor)
{
	std::string reason = "rule denied: rm -rf";
	Status s(Status::OPERATION_DENIED, reason);
	EXPECT_FALSE(s.ok());
	EXPECT_EQ(s.code, Status::OPERATION_DENIED);
	EXPECT_STREQ(s.message, "rule denied: rm -rf");
}

TEST(StatusBasic, DynamicMessageSurvivesMove)
{
	Status s(Status::CONFIRM_DENIED, std::string("user said no"));
	Status moved = std::move(s);
	EXPECT_EQ(moved.code, Status::CONFIRM_DENIED);
	EXPECT_STREQ(moved.message, "user said no");
}

TEST(StatusBasic, CopyPreservesMessage)
{
	Status original(Status::INTERNAL_ERROR, std::string("something broke"));
	Status copy = original;
	EXPECT_EQ(copy.code, Status::INTERNAL_ERROR);
	EXPECT_STREQ(copy.message, "something broke");
	// 确保 copy 的 message 指向自己的存储，不是 original 的
	EXPECT_NE(copy.message, original.message);
}

TEST(StatusBasic, CopyAssignment)
{
	Status a(Status::IO_ERROR, std::string("error a"));
	Status b = Status::Ok();
	b = a;
	EXPECT_EQ(b.code, Status::IO_ERROR);
	EXPECT_STREQ(b.message, "error a");
}

TEST(StatusBasic, MoveAssignment)
{
	Status a(Status::NOT_FOUND, std::string("missing"));
	Status b = Status::Ok();
	b = std::move(a);
	EXPECT_EQ(b.code, Status::NOT_FOUND);
	EXPECT_STREQ(b.message, "missing");
}

TEST(StatusBasic, StaticMessageCopySafe)
{
	Status original(Status::IO_ERROR, "static msg");
	Status copy = original;
	EXPECT_EQ(copy.code, Status::IO_ERROR);
	EXPECT_STREQ(copy.message, "static msg");
}

// ── statusMessage ──────────────────────────────────────────────────────────

TEST(StatusMessage, OkCode)
{
	EXPECT_STREQ(statusMessage(Status::OK), "ok");
}

TEST(StatusMessage, AllCodesHaveMessages)
{
	// 确保每个枚举值都有对应字符串描述，不返回 nullptr
	const Status::Code codes[] = {
		Status::OK, Status::UNKNOWN, Status::INVALID_ARGUMENT,
		Status::NOT_SUPPORTED, Status::OUT_OF_RANGE, Status::IO_ERROR,
		Status::INTERNAL_ERROR, Status::ADDRESS_CONFLICT, Status::INVALID_ADDRESS,
		Status::DEVICE_NOT_FOUND, Status::DEVICE_NOT_READY, Status::HYPERVISOR_ERROR,
		Status::VCPU_SHUTDOWN, Status::VCPU_INTERRUPTED,
		Status::ACCESSIBILITY_DENIED, Status::ELEMENT_NOT_FOUND, Status::WINDOW_NOT_FOUND,
		Status::OPERATION_DENIED, Status::CONFIRM_TIMEOUT, Status::CONFIRM_DENIED,
		Status::CONFIRM_REQUIRED, Status::CONFIG_PARSE_ERROR, Status::NOT_INITIALIZED,
		Status::CAPABILITY_NOT_FOUND, Status::MODULE_LOAD_ERROR,
		Status::ALREADY_EXISTS, Status::NOT_FOUND,
	};

	for (auto code : codes) {
		const char* msg = statusMessage(code);
		EXPECT_NE(msg, nullptr) << "statusMessage returned nullptr for code " << static_cast<int>(code);
		EXPECT_GT(std::strlen(msg), 0u) << "statusMessage returned empty for code " << static_cast<int>(code);
	}
}

TEST(StatusMessage, SpecificMessages)
{
	EXPECT_STREQ(statusMessage(Status::ALREADY_EXISTS), "already exists");
	EXPECT_STREQ(statusMessage(Status::NOT_FOUND), "not found");
	EXPECT_STREQ(statusMessage(Status::OPERATION_DENIED), "operation denied by security policy");
}

// ── Result<T> ──────────────────────────────────────────────────────────────

TEST(ResultT, OkHoldsValue)
{
	auto r = Result<int>::Ok(42);
	EXPECT_TRUE(r.success());
	EXPECT_FALSE(r.failure());
	EXPECT_EQ(r.value(), 42);
}

TEST(ResultT, OkString)
{
	auto r = Result<std::string>::Ok(std::string("hello"));
	EXPECT_TRUE(r.success());
	EXPECT_EQ(r.value(), "hello");
}

TEST(ResultT, ErrorWithCode)
{
	auto r = Result<int>::Error(Status::IO_ERROR);
	EXPECT_TRUE(r.failure());
	EXPECT_EQ(r.error().code, Status::IO_ERROR);
}

TEST(ResultT, ErrorWithStaticMessage)
{
	auto r = Result<int>::Error(Status::IO_ERROR, "read failed");
	EXPECT_TRUE(r.failure());
	EXPECT_EQ(r.error().code, Status::IO_ERROR);
	EXPECT_STREQ(r.error().message, "read failed");
}

TEST(ResultT, ErrorWithDynamicMessage)
{
	auto r = Result<int>::Error(Status::INTERNAL_ERROR, std::string("dynamic error"));
	EXPECT_TRUE(r.failure());
	EXPECT_EQ(r.error().code, Status::INTERNAL_ERROR);
	EXPECT_STREQ(r.error().message, "dynamic error");
}

TEST(ResultT, ErrorAutoFillsMessage)
{
	auto r = Result<int>::Error(Status::NOT_FOUND);
	EXPECT_TRUE(r.failure());
	EXPECT_STREQ(r.error().message, "not found");
}

TEST(ResultT, ValueOrOnSuccess)
{
	auto r = Result<int>::Ok(42);
	EXPECT_EQ(r.value_or(99), 42);
}

TEST(ResultT, ValueOrOnFailure)
{
	auto r = Result<int>::Error(Status::IO_ERROR);
	EXPECT_EQ(r.value_or(99), 99);
}

TEST(ResultT, ValueOrElseOnSuccess)
{
	auto r = Result<int>::Ok(42);
	EXPECT_EQ(r.value_or_else([]{ return 99; }), 42);
}

TEST(ResultT, ValueOrElseOnFailure)
{
	auto r = Result<int>::Error(Status::IO_ERROR);
	EXPECT_EQ(r.value_or_else([]{ return 99; }), 99);
}

TEST(ResultT, MoveValueOut)
{
	auto r = Result<std::string>::Ok(std::string("move me"));
	std::string val = std::move(r).value();
	EXPECT_EQ(val, "move me");
}

TEST(ResultT, PropagateError)
{
	auto r1 = Result<int>::Error(Status::IO_ERROR, "original error");
	auto r2 = Result<std::string>::Error(r1);
	EXPECT_TRUE(r2.failure());
	EXPECT_EQ(r2.error().code, Status::IO_ERROR);
}

// ── Result<void> ───────────────────────────────────────────────────────────

TEST(ResultVoid, OkIsSuccess)
{
	auto r = Result<void>::Ok();
	EXPECT_TRUE(r.success());
	EXPECT_FALSE(r.failure());
}

TEST(ResultVoid, ErrorWithCode)
{
	auto r = Result<void>::Error(Status::INTERNAL_ERROR);
	EXPECT_TRUE(r.failure());
	EXPECT_EQ(r.error().code, Status::INTERNAL_ERROR);
}

TEST(ResultVoid, ErrorWithStaticMessage)
{
	auto r = Result<void>::Error(Status::CONFIG_PARSE_ERROR, "bad toml");
	EXPECT_TRUE(r.failure());
	EXPECT_EQ(r.error().code, Status::CONFIG_PARSE_ERROR);
	EXPECT_STREQ(r.error().message, "bad toml");
}

TEST(ResultVoid, ErrorWithDynamicMessage)
{
	auto r = Result<void>::Error(Status::MODULE_LOAD_ERROR, std::string("dll not found"));
	EXPECT_TRUE(r.failure());
	EXPECT_STREQ(r.error().message, "dll not found");
}

TEST(ResultVoid, PropagateError)
{
	auto r1 = Result<int>::Error(Status::NOT_FOUND, "missing");
	auto r2 = Result<void>::Error(r1);
	EXPECT_TRUE(r2.failure());
	EXPECT_EQ(r2.error().code, Status::NOT_FOUND);
}

} // namespace
} // namespace clawspan
