#pragma once

#include "core/base/core_config.h"
#include "common/error.h"

#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace clawspan {
namespace ipc {
// 前向声明：UIService 定义在 ipc/ui_service.h，此处仅需指针参数，避免 core→ipc 头文件依赖。
class UIService;
} // namespace ipc
} // namespace clawspan

namespace clawspan {
namespace core {

// 前向声明：避免 service.h 直接引入 task_registry.h 形成循环依赖风险，
// 同时允许 setTaskRegistry 接受指针参数。
class TaskRegistry;

// CapabilityService 是 core 对外暴露的唯一入口，daemon 通过它访问所有能力。
//
// 职责：
// - 生命周期管理：init 时内部启动 ModuleManager，动态加载所有能力插件与安全模块；
//                 release 时逆序 Shutdown 所有模块。
// - 统一调用入口：callCapability 将请求透明地经过 SecurityChain 审查后，
//                 路由到对应的 CapabilityInterface 执行。
// - 任务上下文装配：通过注入的 TaskRegistry 根据 task_id 查找 TaskContext，
//                   填入 SecurityContext.task，使安全模块可感知任务级状态。
// - 确认通道管理：SecurityAction::NeedConfirm 时的用户确认流程由内部处理，
//                 daemon 不感知此细节。
class CapabilityService
{
public:
	CapabilityService();
	~CapabilityService();

	CapabilityService(const CapabilityService&)            = delete;
	CapabilityService& operator=(const CapabilityService&) = delete;

	// init 初始化 core 运行时。
	//
	// 内部流程：
	//   1. 遍历 config.modules，通过 ModuleManager 动态加载各模块。
	//   2. 调用各模块的 init(spec.params) 完成初始化。
	//   3. 按 spec.priority 将安全模块注册进 SecurityChain。
	//
	// 入参:
	// - config: CoreConfig，由 daemon 解析 TOML 后构造。
	//
	// 出参/返回:
	// - Result::Ok()：初始化成功。
	// - Result::Error(status)：初始化失败。
	Result<void> init(const CoreConfig& config);

	// release 关闭 core 运行时，逆序 Shutdown 所有模块。
	void release();

	// setTaskRegistry 注入 TaskRegistry，供 callCapability 装配 SecurityContext.task。
	//
	// 未注入时 SecurityContext.task = nullptr，安全模块自动降级为无任务上下文模式。
	// registry 的生命周期须长于 CapabilityService 实例。
	//
	// 入参:
	// - registry: TaskRegistry 指针，可为 nullptr（相当于卸载）。
	void setTaskRegistry(TaskRegistry* registry);

	// setUIService 注入 UIService，供 callCapability 推送 op_log 与处理 NeedConfirm 确认。
	//
	// 未注入时 NeedConfirm 降级为旧版 ConfirmHandlerInterface；两者均未注入则返回
	// CONFIRM_REQUIRED 错误。ui_service 的生命周期须长于 CapabilityService 实例。
	//
	// 入参:
	// - ui_service: UIService 指针，可为 nullptr（相当于卸载）。
	void setUIService(ipc::UIService* ui_service);

	// capabilityNames 返回已加载的所有能力模块名称列表（须在 init 成功后调用）。
	std::vector<std::string> capabilityNames() const;

	// callCapability 是 daemon 调用能力的统一入口。
	//
	// 内部流程：
	//   1. 若注入了 TaskRegistry，通过 task_id 查找 TaskContext 并装配进 SecurityContext。
	//   2. SecurityChain::runPreHook —— 入站审查（Pass / NeedConfirm / Deny）。
	//   3. NeedConfirm → 通过 ConfirmHandler 阻塞等待用户确认。
	//   4. 路由到对应 CapabilityInterface::execute(operation, params)。
	//   5. SecurityChain::runPostHook —— 出站过滤。
	//
	// 入参:
	// - capability_name: 目标能力标识，例如 "capability_ax"。
	// - operation:       操作名称，例如 "list_windows"。
	// - params:          操作参数 JSON 对象，无参数时传 {} 空对象。
	// - task_id:         当前操作所属任务 ID，空字符串表示无关联任务。
	//
	// 出参/返回:
	// - Result::Ok(json)：操作成功，json 为返回数据。
	// - Result::Error(status)：操作失败或被安全策略拒绝。
	Result<nlohmann::json> callCapability(std::string_view      capability_name,
	                                      std::string_view      operation,
	                                      const nlohmann::json& params,
	                                      const std::string&    task_id = "");

private:
	struct Implement;
	std::unique_ptr<Implement> implement_;
};

} // namespace core
} // namespace clawspan
