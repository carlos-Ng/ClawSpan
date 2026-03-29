#pragma once

#include "common/error.h"

#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "vm_channel.grpc.pb.h"

namespace clawspan {
namespace vm_channel {
namespace grpc {

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

class VmChannelRpcService final
	: public clawspan::rpc::vm_channel::v1::VmChannelService::Service
{
public:
	VmChannelRpcService(BeginTaskHandler  begin_handler,
	                    EndTaskHandler    end_handler,
	                    CapabilityHandler capability_handler);

	::grpc::Status BeginTask(
		::grpc::ServerContext*                                     context,
		const clawspan::rpc::vm_channel::v1::BeginTaskRequest*     request,
		clawspan::rpc::vm_channel::v1::BeginTaskResponse*          response) override;

	::grpc::Status EndTask(
		::grpc::ServerContext*                                   context,
		const clawspan::rpc::vm_channel::v1::EndTaskRequest*     request,
		clawspan::rpc::vm_channel::v1::EndTaskResponse*          response) override;

	::grpc::Status CallCapability(
		::grpc::ServerContext*                                           context,
		const clawspan::rpc::vm_channel::v1::CallCapabilityRequest*      request,
		clawspan::rpc::vm_channel::v1::CallCapabilityResponse*           response) override;

private:
	BeginTaskHandler  begin_handler_;
	EndTaskHandler    end_handler_;
	CapabilityHandler capability_handler_;
};

class VmChannelGrpcServer
{
public:
	VmChannelGrpcServer(BeginTaskHandler  begin_handler,
	                    EndTaskHandler    end_handler,
	                    CapabilityHandler capability_handler);
	~VmChannelGrpcServer();

	VmChannelGrpcServer(const VmChannelGrpcServer&) = delete;
	VmChannelGrpcServer& operator=(const VmChannelGrpcServer&) = delete;

	Status start(std::string_view listen_address);
	void   stop();
	bool   isRunning() const;

private:
	VmChannelRpcService          service_;
	std::unique_ptr<::grpc::Server> server_;
	std::thread                  server_thread_;
};

} // namespace grpc
} // namespace vm_channel
} // namespace clawspan
