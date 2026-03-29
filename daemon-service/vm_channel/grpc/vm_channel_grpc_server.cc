#include "vm_channel/grpc/vm_channel_grpc_server.h"

#include "common/log.h"
#include "common/status.h"

namespace clawspan {
namespace vm_channel {
namespace grpc {

namespace {

using BeginTaskRequest = clawspan::rpc::vm_channel::v1::BeginTaskRequest;
using BeginTaskResponse = clawspan::rpc::vm_channel::v1::BeginTaskResponse;
using EndTaskRequest = clawspan::rpc::vm_channel::v1::EndTaskRequest;
using EndTaskResponse = clawspan::rpc::vm_channel::v1::EndTaskResponse;
using CallCapabilityRequest = clawspan::rpc::vm_channel::v1::CallCapabilityRequest;
using CallCapabilityResponse = clawspan::rpc::vm_channel::v1::CallCapabilityResponse;

} // namespace

VmChannelRpcService::VmChannelRpcService(BeginTaskHandler  begin_handler,
                                         EndTaskHandler    end_handler,
                                         CapabilityHandler capability_handler)
	: begin_handler_(std::move(begin_handler))
	, end_handler_(std::move(end_handler))
	, capability_handler_(std::move(capability_handler))
{}

::grpc::Status VmChannelRpcService::BeginTask(::grpc::ServerContext* /*context*/,
                                              const BeginTaskRequest* request,
                                              BeginTaskResponse*      response)
{
	try {
		const std::string task_id = begin_handler_(
			request->description(),
			request->root_description(),
			request->parent_task_id(),
			request->session_id());
		if (task_id.empty()) {
			return ::grpc::Status(::grpc::StatusCode::INTERNAL,
			                      "beginTask handler returned empty task_id");
		}
		response->set_task_id(task_id);
		return ::grpc::Status::OK;
	} catch (const std::exception& e) {
		return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
	}
}

::grpc::Status VmChannelRpcService::EndTask(::grpc::ServerContext* /*context*/,
                                            const EndTaskRequest* request,
                                            EndTaskResponse*      response)
{
	try {
		end_handler_(request->task_id(), request->success());
		response->set_success(true);
		return ::grpc::Status::OK;
	} catch (const std::exception& e) {
		return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
	}
}

::grpc::Status VmChannelRpcService::CallCapability(
	::grpc::ServerContext* /*context*/,
	const CallCapabilityRequest* request,
	CallCapabilityResponse*      response)
{
	try {
		nlohmann::json params = nlohmann::json::object();
		if (!request->params_json().empty()) {
			try {
				params = nlohmann::json::parse(request->params_json());
			} catch (const std::exception& e) {
				response->set_success(false);
				response->set_error_code(static_cast<int>(Status::INVALID_ARGUMENT));
				response->set_error_message(std::string("invalid params_json: ") + e.what());
				return ::grpc::Status::OK;
			}
		}

		auto result = capability_handler_(
			request->task_id(),
			request->capability(),
			request->operation(),
			params);

		if (result.success()) {
			response->set_success(true);
			response->set_result_json(result.value().dump());
			response->set_error_code(0);
			response->clear_error_message();
		} else {
			response->set_success(false);
			response->set_error_code(static_cast<int>(result.error().code));
			response->set_error_message(result.error().message != nullptr
			                                ? result.error().message
			                                : statusMessage(result.error().code));
		}

		return ::grpc::Status::OK;
	} catch (const std::exception& e) {
		return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
	}
}

VmChannelGrpcServer::VmChannelGrpcServer(BeginTaskHandler  begin_handler,
                                         EndTaskHandler    end_handler,
                                         CapabilityHandler capability_handler)
	: service_(std::move(begin_handler), std::move(end_handler), std::move(capability_handler))
{}

VmChannelGrpcServer::~VmChannelGrpcServer()
{
	stop();
}

Status VmChannelGrpcServer::start(std::string_view listen_address)
{
	if (server_ != nullptr) {
		return Status(Status::INTERNAL_ERROR, "vm_channel grpc server is already running");
	}

	::grpc::ServerBuilder builder;
	builder.AddListeningPort(std::string(listen_address), ::grpc::InsecureServerCredentials());
	builder.RegisterService(&service_);

	server_ = builder.BuildAndStart();
	if (server_ == nullptr) {
		return Status(Status::IO_ERROR, "failed to start vm_channel grpc server");
	}

	server_thread_ = std::thread([this]() {
		server_->Wait();
	});

	LOG_INFO("vm_channel grpc: listening on {}", listen_address);
	return Status::Ok();
}

void VmChannelGrpcServer::stop()
{
	if (server_ == nullptr) {
		return;
	}

	server_->Shutdown();
	if (server_thread_.joinable()) {
		server_thread_.join();
	}
	server_.reset();
	LOG_INFO("vm_channel grpc: stopped");
}

bool VmChannelGrpcServer::isRunning() const
{
	return server_ != nullptr;
}

} // namespace grpc
} // namespace vm_channel
} // namespace clawspan
