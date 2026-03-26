#include "vmm/vsock_bridge_server.h"

#include <gtest/gtest.h>

namespace clawspan {
namespace vmm {
namespace {

TEST(VsockBridgeServerFactory, CreateReturnsNonNull)
{
	auto server = createVsockBridgeServer();
	ASSERT_NE(server, nullptr);
	EXPECT_FALSE(server->isRunning());
}

class VsockBridgeServerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		server_ = createVsockBridgeServer();
		ASSERT_NE(server_, nullptr);
	}

	void TearDown() override
	{
		if (server_ && server_->isRunning()) {
			server_->stop();
		}
	}

	std::unique_ptr<VsockBridgeServerInterface> server_;
};

TEST_F(VsockBridgeServerTest, StopWithoutStartDoesNotCrash)
{
	EXPECT_NO_FATAL_FAILURE(server_->stop());
	EXPECT_FALSE(server_->isRunning());
}

TEST_F(VsockBridgeServerTest, StartAndStop)
{
	constexpr uint32_t kTestPort = 55561;
	auto st = server_->start(kTestPort, "127.0.0.1", 50051);

	if (st.ok()) {
		EXPECT_TRUE(server_->isRunning());
		server_->stop();
		EXPECT_FALSE(server_->isRunning());
	} else {
		EXPECT_EQ(st.code, Status::IO_ERROR);
		EXPECT_FALSE(server_->isRunning());
	}
}

TEST_F(VsockBridgeServerTest, DoubleStartReturnsError)
{
	constexpr uint32_t kTestPort = 55562;
	auto st = server_->start(kTestPort, "127.0.0.1", 50051);

	if (!st.ok()) {
		GTEST_SKIP() << "AF_HYPERV not available, skipping double-start test";
	}

	auto st2 = server_->start(kTestPort, "127.0.0.1", 50051);
	EXPECT_EQ(st2.code, Status::INTERNAL_ERROR);
	server_->stop();
}

} // namespace
} // namespace vmm
} // namespace clawspan
