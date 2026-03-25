#pragma once

#include "common/error.h"

#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "channel3.grpc.pb.h"

namespace clawspan {
namespace channel3 {

using BeginTaskHandler = std::function<std::string(
	const std::string& description,
	const std::string& root_description,
	const std::string& parent_task_id,
	const std::string& session_id)>;

using EndTaskHandler = std::function<void(
	const std::string& task_id,
	bool               success)>;

using CapabilityHandler = std::function<Result<nlohmann::json>(
	const std::string&    task_id,
	const std::string&    capability,
	const std::string&    operation,
	const nlohmann::json& params)>;

class Channel3RpcService final
	: public clawspan::rpc::channel3::v1::Channel3Service::Service
{
public:
	Channel3RpcService(BeginTaskHandler  begin_handler,
	                   EndTaskHandler    end_handler,
	                   CapabilityHandler capability_handler);

	grpc::Status BeginTask(grpc::ServerContext*                                   context,
	                       const clawspan::rpc::channel3::v1::BeginTaskRequest*  request,
	                       clawspan::rpc::channel3::v1::BeginTaskResponse*       response) override;

	grpc::Status EndTask(grpc::ServerContext*                                 context,
	                     const clawspan::rpc::channel3::v1::EndTaskRequest*  request,
	                     clawspan::rpc::channel3::v1::EndTaskResponse*       response) override;

	grpc::Status CallCapability(
		grpc::ServerContext*                                          context,
		const clawspan::rpc::channel3::v1::CallCapabilityRequest*     request,
		clawspan::rpc::channel3::v1::CallCapabilityResponse*          response) override;

private:
	BeginTaskHandler  begin_handler_;
	EndTaskHandler    end_handler_;
	CapabilityHandler capability_handler_;
};

class Channel3GrpcServer
{
public:
	Channel3GrpcServer(BeginTaskHandler  begin_handler,
	                   EndTaskHandler    end_handler,
	                   CapabilityHandler capability_handler);
	~Channel3GrpcServer();

	Channel3GrpcServer(const Channel3GrpcServer&)            = delete;
	Channel3GrpcServer& operator=(const Channel3GrpcServer&) = delete;

	Status start(std::string_view listen_address);
	void   stop();
	bool   isRunning() const;

private:
	Channel3RpcService           service_;
	std::unique_ptr<grpc::Server> server_;
	std::thread                  server_thread_;
};

} // namespace channel3
} // namespace clawspan
