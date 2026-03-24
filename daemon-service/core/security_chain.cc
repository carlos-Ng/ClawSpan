#include "core/base/security.h"

#include <algorithm>
#include <cassert>

namespace clawspan {
namespace core {

// registerModule 向安全调用链注册一个模块，按 priority 升序插入。
// 相同优先级时，lower_bound 将新模块插在所有等值元素之前（LIFO 语义）。
void SecurityChain::registerModule(SecurityModuleInterface* module, int priority)
{
	if (module == nullptr) {
		return;
	}
#ifndef NDEBUG
	for (const auto& entry : modules_) {
		assert(entry.second != module &&
		       "SecurityChain: duplicate module registration detected");
	}
#endif
	auto it = std::lower_bound(
		modules_.begin(), modules_.end(), priority,
		[](const std::pair<int, SecurityModuleInterface*>& entry, int prio) {
			return entry.first < prio;
		});
	modules_.insert(it, {priority, module});
}

// runPreHook 按 priority 顺序驱动所有模块执行 preHook，汇总裁决结果。
// reason 保留第一个 NeedConfirm 模块填写的原因（优先级最高的模块语义更强）。
SecurityAction SecurityChain::runPreHook(const SecurityContext& ctx, std::string& reason)
{
	SecurityAction final_action = SecurityAction::Pass;
	for (const auto& entry : modules_) {
		std::string mod_reason;
		SecurityAction action = entry.second->preHook(ctx, mod_reason);
		if (action == SecurityAction::Deny) {
			reason = std::move(mod_reason);
			return SecurityAction::Deny;
		}
		if (action == SecurityAction::NeedConfirm) {
			if (final_action != SecurityAction::NeedConfirm) {
				reason = std::move(mod_reason); // 仅记录第一个 NeedConfirm 的 reason
			}
			final_action = SecurityAction::NeedConfirm;
		}
	}
	return final_action;
}

// runPostHook 按 priority 顺序驱动所有模块执行 postHook，汇总裁决结果。
// reason 保留第一个 NeedConfirm 模块填写的原因（同 runPreHook 语义一致）。
SecurityAction SecurityChain::runPostHook(const SecurityContext& ctx,
                                          nlohmann::json& response,
                                          std::string& reason)
{
	SecurityAction final_action = SecurityAction::Pass;
	for (const auto& entry : modules_) {
		std::string mod_reason;
		SecurityAction action = entry.second->postHook(ctx, response, mod_reason);
		if (action == SecurityAction::Deny) {
			reason = std::move(mod_reason);
			return SecurityAction::Deny;
		}
		if (action == SecurityAction::NeedConfirm) {
			if (final_action != SecurityAction::NeedConfirm) {
				reason = std::move(mod_reason); // 仅记录第一个 NeedConfirm 的 reason
			}
			final_action = SecurityAction::NeedConfirm;
		}
	}
	return final_action;
}

// runTaskBeginHook 在任务开始时按 priority 顺序通知所有模块。
// 任何模块返回 Deny 立即短路；NeedConfirm 降级为 Pass（任务级不弹窗）。
SecurityAction SecurityChain::runTaskBeginHook(const TaskContext& task, std::string& reason)
{
	for (const auto& entry : modules_) {
		std::string mod_reason;
		SecurityAction action = entry.second->onTaskBegin(task, mod_reason);
		if (action == SecurityAction::Deny) {
			reason = std::move(mod_reason);
			return SecurityAction::Deny;
		}
	}
	return SecurityAction::Pass;
}

// runTaskEndHook 在任务结束时按 priority 顺序通知所有模块，纯通知语义。
void SecurityChain::runTaskEndHook(const std::string& task_id, bool success)
{
	for (const auto& entry : modules_) {
		entry.second->onTaskEnd(task_id, success);
	}
}

// notifyCapabilityRegistered 在能力插件注册时广播安全声明给所有安全模块。
void SecurityChain::notifyCapabilityRegistered(const std::string& capability_name,
                                               const PluginSecurityDecl& decl)
{
	for (const auto& entry : modules_) {
		entry.second->onCapabilityRegistered(capability_name, decl);
	}
}

} // namespace core
} // namespace clawspan
