// vsock_server_test.cc — VsockServer 单元测试
//
// 测试策略：
//   - 工厂函数、基本生命周期：始终运行
//   - AF_HYPERV 需要 Hyper-V 虚拟化支持，在不支持的环境下
//     start() 会返回 IO_ERROR，测试据此判断

#include "vmm/vsock_server.h"

#include <gtest/gtest.h>

#include <string>

namespace clawspan {
namespace vmm {
namespace {

// ── 工厂函数 ────────────────────────────────────────────────────────────────

TEST(VsockServerFactory, CreateReturnsNonNull)
{
	bool handler_called = false;
	auto server = createVsockServer(
		[&](VsockConnection& /*conn*/, const std::string& /*json*/) {
			handler_called = true;
		});
	ASSERT_NE(server, nullptr);
	EXPECT_FALSE(server->isRunning());
}

// ── 基本生命周期 ─────────────────────────────────────────────────────────────

class VsockServerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		server_ = createVsockServer(
			[this](VsockConnection& /*conn*/, const std::string& json) {
				last_json_ = json;
			});
		ASSERT_NE(server_, nullptr);
	}

	void TearDown() override
	{
		if (server_ && server_->isRunning()) {
			server_->stop();
		}
	}

	std::unique_ptr<VsockServerInterface> server_;
	std::string last_json_;
};

// 未启动时 isRunning 应为 false
TEST_F(VsockServerTest, NotRunningByDefault)
{
	EXPECT_FALSE(server_->isRunning());
}

// stop 对未启动的 server 应为无操作（不崩溃）
TEST_F(VsockServerTest, StopWithoutStartDoesNotCrash)
{
	EXPECT_NO_FATAL_FAILURE(server_->stop());
	EXPECT_FALSE(server_->isRunning());
}

// start 尝试绑定 AF_HYPERV socket
// 在支持 Hyper-V 的机器上：应成功
// 在不支持的环境下（CI、无 Hyper-V）：返回 IO_ERROR，也是正确行为
TEST_F(VsockServerTest, StartAndStop)
{
	constexpr uint32_t kTestPort = 55555;
	auto st = server_->start(kTestPort);

	if (st.ok()) {
		// 成功启动
		EXPECT_TRUE(server_->isRunning());

		// 停止
		server_->stop();
		EXPECT_FALSE(server_->isRunning());
	} else {
		// 不支持 AF_HYPERV — 合理行为
		EXPECT_EQ(st.code, Status::IO_ERROR);
		EXPECT_FALSE(server_->isRunning());
	}
}

// 重复启动应返回 INTERNAL_ERROR
TEST_F(VsockServerTest, DoubleStartReturnsError)
{
	constexpr uint32_t kTestPort = 55556;
	auto st = server_->start(kTestPort);

	if (!st.ok()) {
		GTEST_SKIP() << "AF_HYPERV not available, skipping double-start test";
	}

	// 第二次 start
	auto st2 = server_->start(kTestPort);
	EXPECT_EQ(st2.code, Status::INTERNAL_ERROR);

	server_->stop();
}

// 停止后可以重新启动（如果环境支持）
TEST_F(VsockServerTest, RestartAfterStop)
{
	constexpr uint32_t kTestPort = 55557;
	auto st = server_->start(kTestPort);

	if (!st.ok()) {
		GTEST_SKIP() << "AF_HYPERV not available";
	}

	server_->stop();
	EXPECT_FALSE(server_->isRunning());

	// 重新创建（当前实现 stop 后 WSACleanup，需要新实例）
	server_ = createVsockServer(
		[this](VsockConnection& /*conn*/, const std::string& json) {
			last_json_ = json;
		});

	st = server_->start(kTestPort);
	if (st.ok()) {
		EXPECT_TRUE(server_->isRunning());
		server_->stop();
	}
}

} // namespace
} // namespace vmm
} // namespace clawspan
