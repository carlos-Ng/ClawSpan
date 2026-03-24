// safe_handle_test.cc — SafeHandle RAII 单元测试

#include "common/safe_handle.h"

#include <gtest/gtest.h>

#include <utility>

namespace clawspan {
namespace {

// ── 基本构造 / 析构 ─────────────────────────────────────────────────────────

TEST(SafeHandleBasic, DefaultIsInvalid)
{
	SafeHandle h;
	EXPECT_FALSE(h.isValid());
	EXPECT_FALSE(static_cast<bool>(h));
	EXPECT_EQ(h.get(), INVALID_HANDLE_VALUE);
}

TEST(SafeHandleBasic, InvalidHandleValueIsInvalid)
{
	SafeHandle h(INVALID_HANDLE_VALUE);
	EXPECT_FALSE(h.isValid());
}

TEST(SafeHandleBasic, NullptrIsInvalid)
{
	SafeHandle h(nullptr);
	EXPECT_FALSE(h.isValid());
}

// 使用 CreateEvent 创建真实 HANDLE 测试有效性和自动关闭
TEST(SafeHandleBasic, ValidHandleIsValid)
{
	HANDLE raw = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	ASSERT_NE(raw, nullptr);

	SafeHandle h(raw);
	EXPECT_TRUE(h.isValid());
	EXPECT_TRUE(static_cast<bool>(h));
	EXPECT_EQ(h.get(), raw);
	// h 析构时自动 CloseHandle
}

// ── 移动语义 ────────────────────────────────────────────────────────────────

TEST(SafeHandleMove, MoveConstructor)
{
	HANDLE raw = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	ASSERT_NE(raw, nullptr);

	SafeHandle a(raw);
	SafeHandle b(std::move(a));

	EXPECT_TRUE(b.isValid());
	EXPECT_EQ(b.get(), raw);
	EXPECT_FALSE(a.isValid()); // a 已被移动
}

TEST(SafeHandleMove, MoveAssignment)
{
	HANDLE raw1 = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE raw2 = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	ASSERT_NE(raw1, nullptr);
	ASSERT_NE(raw2, nullptr);

	SafeHandle a(raw1);
	SafeHandle b(raw2);
	b = std::move(a); // b 的 raw2 应被关闭

	EXPECT_TRUE(b.isValid());
	EXPECT_EQ(b.get(), raw1);
	EXPECT_FALSE(a.isValid());
}

TEST(SafeHandleMove, SelfMoveAssignmentSafe)
{
	HANDLE raw = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	ASSERT_NE(raw, nullptr);

	SafeHandle h(raw);
	h = std::move(h); // 自移动不应崩溃
	EXPECT_TRUE(h.isValid());
	EXPECT_EQ(h.get(), raw);
}

// ── release ─────────────────────────────────────────────────────────────────

TEST(SafeHandleRelease, ReleaseTransfersOwnership)
{
	HANDLE raw = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	ASSERT_NE(raw, nullptr);

	SafeHandle h(raw);
	HANDLE released = h.release();

	EXPECT_EQ(released, raw);
	EXPECT_FALSE(h.isValid()); // h 不再拥有句柄

	// 手动关闭
	::CloseHandle(released);
}

TEST(SafeHandleRelease, ReleaseFromInvalid)
{
	SafeHandle h;
	HANDLE released = h.release();
	EXPECT_EQ(released, INVALID_HANDLE_VALUE);
}

} // namespace
} // namespace clawspan
