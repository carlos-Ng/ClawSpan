// frame_codec_test.cc — FrameCodec 集成测试
//
// 使用真实 Named Pipe 进行 writeFrame → readFrame 往返测试。
// 大帧测试使用后台线程写、主线程读，避免 pipe 缓冲区满导致死锁。

#include "frame.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>

namespace clawspan {
namespace ipc {
namespace {

// 每个测试使用独立管道名避免并发冲突
static std::atomic<int> g_pipe_counter{0};

std::string uniquePipeName()
{
	int id = g_pipe_counter.fetch_add(1);
	return std::string("\\\\.\\pipe\\clawspan-frame-test-") + std::to_string(id);
}

// Helper: 创建一对连接好的 Named Pipe 句柄用于测试
// 返回 {server_handle, client_handle}
std::pair<HANDLE, HANDLE> createPipePair(const std::string& pipe_name)
{
	HANDLE server = ::CreateNamedPipeA(
		pipe_name.c_str(),
		PIPE_ACCESS_DUPLEX,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		1,
		65536, 65536,
		0, nullptr
	);
	if (server == INVALID_HANDLE_VALUE) {
		return {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
	}

	// 在后台线程中连接服务端（ConnectNamedPipe 会阻塞直到客户端连接）
	std::thread connector([server]() {
		::ConnectNamedPipe(server, nullptr);
	});

	HANDLE client = ::CreateFileA(
		pipe_name.c_str(),
		GENERIC_READ | GENERIC_WRITE,
		0, nullptr,
		OPEN_EXISTING, 0, nullptr
	);

	connector.join();

	if (client == INVALID_HANDLE_VALUE) {
		::CloseHandle(server);
		return {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
	}

	return {server, client};
}

class FrameCodecTest : public ::testing::Test
{
protected:
	HANDLE server_ = INVALID_HANDLE_VALUE;
	HANDLE client_ = INVALID_HANDLE_VALUE;

	void SetUp() override
	{
		auto [s, c] = createPipePair(uniquePipeName());
		server_ = s;
		client_ = c;
		ASSERT_NE(server_, INVALID_HANDLE_VALUE) << "failed to create test pipe pair";
		ASSERT_NE(client_, INVALID_HANDLE_VALUE) << "failed to connect test pipe pair";
	}

	void TearDown() override
	{
		if (server_ != INVALID_HANDLE_VALUE) ::CloseHandle(server_);
		if (client_ != INVALID_HANDLE_VALUE) ::CloseHandle(client_);
	}
};

TEST_F(FrameCodecTest, WriteAndReadSimpleString)
{
	std::string payload = R"({"type":"hello","data":42})";

	auto st = FrameCodec::writeFrame(server_, payload);
	ASSERT_TRUE(st.ok()) << st.message;

	auto result = FrameCodec::readFrame(client_);
	ASSERT_TRUE(result.success()) << result.error().message;
	EXPECT_EQ(result.value(), payload);
}

TEST_F(FrameCodecTest, WriteAndReadEmptyString)
{
	auto st = FrameCodec::writeFrame(server_, "");
	ASSERT_TRUE(st.ok());

	auto result = FrameCodec::readFrame(client_);
	ASSERT_TRUE(result.success());
	EXPECT_EQ(result.value(), "");
}

TEST_F(FrameCodecTest, WriteAndReadLargePayload)
{
	// 1 MiB payload — 超过 pipe 缓冲区（64KB），需要多线程
	std::string payload(1024 * 1024, 'X');
	payload[0] = '{';
	payload[payload.size() - 1] = '}';

	// 后台线程写，主线程读
	Status write_status = Status::Ok();
	std::thread writer([&]() {
		write_status = FrameCodec::writeFrame(server_, payload);
	});

	auto result = FrameCodec::readFrame(client_);
	writer.join();

	ASSERT_TRUE(write_status.ok()) << write_status.message;
	ASSERT_TRUE(result.success()) << result.error().message;
	EXPECT_EQ(result.value(), payload);
}

TEST_F(FrameCodecTest, MultipleFramesInSequence)
{
	const std::string msgs[] = {"frame-1", "frame-2", "frame-3"};

	for (const auto& m : msgs) {
		auto st = FrameCodec::writeFrame(server_, m);
		ASSERT_TRUE(st.ok());
	}

	for (const auto& m : msgs) {
		auto result = FrameCodec::readFrame(client_);
		ASSERT_TRUE(result.success());
		EXPECT_EQ(result.value(), m);
	}
}

TEST_F(FrameCodecTest, BidirectionalCommunication)
{
	// server → client
	{
		auto st = FrameCodec::writeFrame(server_, "request");
		ASSERT_TRUE(st.ok());
		auto r = FrameCodec::readFrame(client_);
		ASSERT_TRUE(r.success());
		EXPECT_EQ(r.value(), "request");
	}
	// client → server
	{
		auto st = FrameCodec::writeFrame(client_, "response");
		ASSERT_TRUE(st.ok());
		auto r = FrameCodec::readFrame(server_);
		ASSERT_TRUE(r.success());
		EXPECT_EQ(r.value(), "response");
	}
}

TEST_F(FrameCodecTest, OversizedFrameRejected)
{
	std::string huge(MAX_FRAME_BODY_SIZE + 1, 'A');
	auto st = FrameCodec::writeFrame(server_, huge);
	EXPECT_FALSE(st.ok());
	EXPECT_EQ(st.code, Status::INVALID_ARGUMENT);
}

TEST_F(FrameCodecTest, ExactMaxSizeFrame)
{
	// 4 MiB — 远超 pipe 缓冲区，必须多线程
	std::string exact(MAX_FRAME_BODY_SIZE, 'B');

	Status write_status = Status::Ok();
	std::thread writer([&]() {
		write_status = FrameCodec::writeFrame(server_, exact);
	});

	auto result = FrameCodec::readFrame(client_);
	writer.join();

	ASSERT_TRUE(write_status.ok()) << write_status.message;
	ASSERT_TRUE(result.success()) << result.error().message;
	EXPECT_EQ(result.value().size(), MAX_FRAME_BODY_SIZE);
}

TEST_F(FrameCodecTest, ReadFromClosedPipeReturnsError)
{
	::CloseHandle(server_);
	server_ = INVALID_HANDLE_VALUE;

	auto result = FrameCodec::readFrame(client_);
	EXPECT_TRUE(result.failure());
	EXPECT_EQ(result.error().code, Status::IO_ERROR);
}

TEST_F(FrameCodecTest, UnicodePayload)
{
	// MSVC C++20: u8"" produces char8_t[], use reinterpret_cast
	std::string payload = reinterpret_cast<const char*>(u8R"({"msg":"你好世界","emoji":"🚀"})");

	auto st = FrameCodec::writeFrame(server_, payload);
	ASSERT_TRUE(st.ok());

	auto result = FrameCodec::readFrame(client_);
	ASSERT_TRUE(result.success());
	EXPECT_EQ(result.value(), payload);
}

} // namespace
} // namespace ipc
} // namespace clawspan
