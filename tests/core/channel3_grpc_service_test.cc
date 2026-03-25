#include "channel3/channel3_grpc_server.h"

#include <gtest/gtest.h>
#include <grpcpp/server_context.h>
#include <nlohmann/json.hpp>

namespace clawspan {
namespace channel3 {
namespace {

using BeginTaskRequest = clawspan::rpc::channel3::v1::BeginTaskRequest;
using BeginTaskResponse = clawspan::rpc::channel3::v1::BeginTaskResponse;
using EndTaskRequest = clawspan::rpc::channel3::v1::EndTaskRequest;
using EndTaskResponse = clawspan::rpc::channel3::v1::EndTaskResponse;
using CallCapabilityRequest = clawspan::rpc::channel3::v1::CallCapabilityRequest;
using CallCapabilityResponse = clawspan::rpc::channel3::v1::CallCapabilityResponse;

TEST(Channel3RpcServiceTest, BeginTaskForwardsRequest)
{
	Channel3RpcService service(
		[](const std::string& description,
		   const std::string& root_description,
		   const std::string& parent_task_id,
		   const std::string& session_id) {
			EXPECT_EQ(description, "child task");
			EXPECT_EQ(root_description, "root task");
			EXPECT_EQ(parent_task_id, "task-parent");
			EXPECT_EQ(session_id, "session-1");
			return std::string("task-123");
		},
		[](const std::string&, bool) {},
		[](const std::string&, const std::string&, const std::string&, const nlohmann::json&) {
			return Result<nlohmann::json>::Error(Status::NOT_SUPPORTED);
		});

	grpc::ServerContext context;
	BeginTaskRequest request;
	request.set_description("child task");
	request.set_root_description("root task");
	request.set_parent_task_id("task-parent");
	request.set_session_id("session-1");

	BeginTaskResponse response;
	const grpc::Status status = service.BeginTask(&context, &request, &response);

	EXPECT_TRUE(status.ok());
	EXPECT_EQ(response.task_id(), "task-123");
}

TEST(Channel3RpcServiceTest, EndTaskForwardsSuccessFlag)
{
	bool called = false;

	Channel3RpcService service(
		[](const std::string&, const std::string&, const std::string&, const std::string&) {
			return std::string("task-123");
		},
		[&called](const std::string& task_id, bool success) {
			called = true;
			EXPECT_EQ(task_id, "task-123");
			EXPECT_FALSE(success);
		},
		[](const std::string&, const std::string&, const std::string&, const nlohmann::json&) {
			return Result<nlohmann::json>::Error(Status::NOT_SUPPORTED);
		});

	grpc::ServerContext context;
	EndTaskRequest request;
	request.set_task_id("task-123");
	request.set_success(false);

	EndTaskResponse response;
	const grpc::Status status = service.EndTask(&context, &request, &response);

	EXPECT_TRUE(status.ok());
	EXPECT_TRUE(response.success());
	EXPECT_TRUE(called);
}

TEST(Channel3RpcServiceTest, CallCapabilityReturnsResultJson)
{
	Channel3RpcService service(
		[](const std::string&, const std::string&, const std::string&, const std::string&) {
			return std::string("task-123");
		},
		[](const std::string&, bool) {},
		[](const std::string&    task_id,
		   const std::string&    capability,
		   const std::string&    operation,
		   const nlohmann::json& params) {
			EXPECT_EQ(task_id, "task-123");
			EXPECT_EQ(capability, "capability_ax");
			EXPECT_EQ(operation, "click");
			EXPECT_EQ(params.at("element_path"), "root/button");
			return Result<nlohmann::json>::Ok(nlohmann::json{
				{"clicked", true},
			});
		});

	grpc::ServerContext context;
	CallCapabilityRequest request;
	request.set_task_id("task-123");
	request.set_capability("capability_ax");
	request.set_operation("click");
	request.set_params_json(R"({"element_path":"root/button"})");

	CallCapabilityResponse response;
	const grpc::Status status = service.CallCapability(&context, &request, &response);

	ASSERT_TRUE(status.ok());
	EXPECT_TRUE(response.success());
	EXPECT_EQ(nlohmann::json::parse(response.result_json())["clicked"], true);
	EXPECT_EQ(response.error_code(), 0);
	EXPECT_TRUE(response.error_message().empty());
}

TEST(Channel3RpcServiceTest, CallCapabilityReturnsErrorPayloadOnFailure)
{
	Channel3RpcService service(
		[](const std::string&, const std::string&, const std::string&, const std::string&) {
			return std::string("task-123");
		},
		[](const std::string&, bool) {},
		[](const std::string&, const std::string&, const std::string&, const nlohmann::json&) {
			return Result<nlohmann::json>::Error(Status::NOT_FOUND, "missing capability");
		});

	grpc::ServerContext context;
	CallCapabilityRequest request;
	request.set_task_id("task-123");
	request.set_capability("capability_ax");
	request.set_operation("click");
	request.set_params_json("{}");

	CallCapabilityResponse response;
	const grpc::Status status = service.CallCapability(&context, &request, &response);

	ASSERT_TRUE(status.ok());
	EXPECT_FALSE(response.success());
	EXPECT_EQ(response.error_code(), Status::NOT_FOUND);
	EXPECT_EQ(response.error_message(), "missing capability");
}

TEST(Channel3RpcServiceTest, CallCapabilityRejectsInvalidParamsJson)
{
	Channel3RpcService service(
		[](const std::string&, const std::string&, const std::string&, const std::string&) {
			return std::string("task-123");
		},
		[](const std::string&, bool) {},
		[](const std::string&, const std::string&, const std::string&, const nlohmann::json&) {
			return Result<nlohmann::json>::Ok(nlohmann::json::object());
		});

	grpc::ServerContext context;
	CallCapabilityRequest request;
	request.set_task_id("task-123");
	request.set_capability("capability_ax");
	request.set_operation("click");
	request.set_params_json("{not-json");

	CallCapabilityResponse response;
	const grpc::Status status = service.CallCapability(&context, &request, &response);

	ASSERT_TRUE(status.ok());
	EXPECT_FALSE(response.success());
	EXPECT_EQ(response.error_code(), Status::INVALID_ARGUMENT);
	EXPECT_FALSE(response.error_message().empty());
}

} // namespace
} // namespace channel3
} // namespace clawspan
