#include "core/base/service.h"
#include "core/base/capability.h"
#include "core/base/module_manager.h"
#include "core/base/security.h"
#include "core/task_registry.h"

#include "ipc/ui_service.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace clawspan {
namespace core {

// ─── Implement ──────────────────────────────────────────────────────────────

struct CapabilityService::Implement
{
	ModuleManager   manager_;
	SecurityChain   chain_;
	TaskRegistry*   task_registry_ = nullptr;
	ipc::UIService* ui_service_    = nullptr;
	bool            initialized_   = false;
};

// ─── CapabilityService 公开接口 ──────────────────────────────────────────────

CapabilityService::CapabilityService()
	: implement_(std::make_unique<Implement>())
{}

CapabilityService::~CapabilityService()
{
	release();
}

// init 初始化 core 运行时：通过 ModuleManager 动态加载所有模块。
Result<void> CapabilityService::init(const CoreConfig& config)
{
	if (implement_->initialized_) {
		return Result<void>::Error(Status::INTERNAL_ERROR, "already initialized");
	}
	auto result = implement_->manager_.init(config, implement_->chain_);
	if (result.failure()) {
		return result;
	}
	implement_->initialized_ = true;
	return Result<void>::Ok();
}

// release 关闭 core 运行时，逆序释放所有已加载模块。
void CapabilityService::release()
{
	if (!implement_->initialized_) {
		return;
	}
	implement_->manager_.release();
	implement_->initialized_ = false;
}

// setTaskRegistry 注入 TaskRegistry，供 callCapability 装配 SecurityContext.task。
void CapabilityService::setTaskRegistry(TaskRegistry* registry)
{
	implement_->task_registry_ = registry;
}

// setUIService 注入 UIService，供 callCapability 推送 op_log 与处理 NeedConfirm 确认。
void CapabilityService::setUIService(ipc::UIService* ui_service)
{
	implement_->ui_service_ = ui_service;
}

// capabilityNames 返回已加载的所有能力模块名称列表。
std::vector<std::string> CapabilityService::capabilityNames() const
{
	return implement_->manager_.capabilityNames();
}

// callCapability 统一能力调用入口，经 SecurityChain 审查后路由到对应插件执行。
//
// NeedConfirm 处理优先级：
//   1. TaskRegistry 意图指纹缓存命中 → 直接放行（fingerprint_cache）
//   2. UIService 注入 → 弹出 UI 确认弹窗；trust_fingerprint=true 时写入缓存
//   3. 均未注入 → 返回 CONFIRM_REQUIRED 错误
//
// op_log 在每次操作完成（包括被拒绝）后通过 UIService 推送给 UI。
Result<nlohmann::json> CapabilityService::callCapability(std::string_view      capability_name,
                                                         std::string_view      operation,
                                                         const nlohmann::json& params,
                                                         const std::string&    task_id)
{
	if (!implement_->initialized_) {
		return Result<nlohmann::json>::Error(Status::NOT_INITIALIZED);
	}

	// 为本次调用分配会话内唯一 ID
	static std::atomic<uint64_t> s_next_op_id{1};
	const std::string op_id =
	    std::to_string(s_next_op_id.fetch_add(1, std::memory_order_relaxed));

	// 查找任务上下文（若有）
	const TaskContext* task_ctx = nullptr;
	if (!task_id.empty() && implement_->task_registry_ != nullptr) {
		task_ctx = implement_->task_registry_->findTask(task_id);
	}

	SecurityContext ctx{
		.operation_id    = op_id,
		.capability_name = capability_name,
		.operation       = operation,
		.params          = params,
		.is_readonly     = false, // TODO: Phase 2 — 根据 operation 前缀推导
		.task            = task_ctx,
	};

	// op_log 推送辅助 lambda（仅在 UIService 已注入且 task_id 非空时有效）
	auto pushOpLog =
	    [&](const std::string& result, const std::string& source) {
		    if (implement_->ui_service_ == nullptr || task_id.empty()) {
			    return;
		    }
		    const int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
		                           std::chrono::system_clock::now().time_since_epoch())
		                           .count();
		    implement_->ui_service_->push(
		        ipc::UIMessageFactory::createOpLog(
		            task_id, std::string(operation), result, source,
		            std::string(capability_name), ts));
	    };

	std::string reason;
	SecurityAction pre = implement_->chain_.runPreHook(ctx, reason);

	if (pre == SecurityAction::Deny) {
		pushOpLog("denied", "rule_deny");
		return Result<nlohmann::json>::Error(
		    Status(Status::OPERATION_DENIED, std::move(reason)));
	}

	// op_log 的 result/source，在 NeedConfirm 路径中按实际处置结果填写，
	// Pass/Skip 统一为 auto_allow。
	std::string op_log_result = "allowed";
	std::string op_log_source = "auto_allow";

	if (pre == SecurityAction::NeedConfirm) {
		// ── 1. 意图指纹缓存检查 ──────────────────────────────────────────────
		IntentFingerprint fp{
		    std::string(capability_name),
		    std::string(operation),
		    "", // scope_value：Phase 2 根据 PluginSecurityDecl.scope_param 提取
		    "", // target_category：Phase 2 根据 PluginSecurityDecl.target_param 提取
		};

		const bool cache_hit =
		    !task_id.empty()
		    && implement_->task_registry_ != nullptr
		    && implement_->task_registry_->checkAuthorization(task_id, fp);

		if (cache_hit) {
			op_log_result = "allowed";
			op_log_source = "fingerprint_cache";
		} else if (implement_->ui_service_ != nullptr) {
			// ── 2. UIService 确认弹窗 ───────────────────────────────────────
			ipc::UIConfirmRequest req;
			req.task_id          = task_id;
			req.capability       = std::string(capability_name);
			req.operation        = std::string(operation);
			req.params           = params;
			req.reason           = reason;
			req.fingerprint      = std::string(capability_name) + "|" + std::string(operation);
			if (task_ctx != nullptr) {
				req.root_description = task_ctx->root_description;
			}

			ipc::UIConfirmResult confirm_result =
			    implement_->ui_service_->askConfirm(req);

			if (confirm_result.trust_fingerprint
			    && !task_id.empty()
			    && implement_->task_registry_ != nullptr) {
				implement_->task_registry_->cacheAuthorization(task_id, fp);
			}

			if (!confirm_result.confirmed) {
				pushOpLog("denied", "user_confirm");
				return Result<nlohmann::json>::Error(
				    Status(Status::OPERATION_DENIED, std::move(reason)));
			}

			op_log_result = "confirmed";
			op_log_source = "user_confirm";
		} else {
			return Result<nlohmann::json>::Error(
			    Status(Status::CONFIRM_REQUIRED, std::move(reason)));
		}
	}

	// ── 执行能力插件 ─────────────────────────────────────────────────────────
	auto* cap = implement_->manager_.getCapability(capability_name);
	if (cap == nullptr) {
		return Result<nlohmann::json>::Error(Status::CAPABILITY_NOT_FOUND);
	}

	auto exec_result = cap->execute(operation, params);
	if (exec_result.failure()) {
		return exec_result;
	}

	nlohmann::json response = exec_result.value();
	SecurityAction post = implement_->chain_.runPostHook(ctx, response, reason);
	if (post == SecurityAction::Deny) {
		pushOpLog("denied", "rule_deny");
		return Result<nlohmann::json>::Error(
		    Status(Status::OPERATION_DENIED, std::move(reason)));
	}

	// ── 操作成功：递增任务操作计数 ───────────────────────────────────────────
	if (!task_id.empty() && implement_->task_registry_ != nullptr) {
		implement_->task_registry_->incrementOpCount(task_id);
	}

	// ── 推送操作日志（UIService 已注入时有效）───────────────────────────────
	if (!op_log_result.empty()) {
		pushOpLog(op_log_result, op_log_source);
	}

	return Result<nlohmann::json>::Ok(std::move(response));
}

} // namespace core
} // namespace clawspan
