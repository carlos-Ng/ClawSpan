#pragma once

#include "core/base/module.h"

#include <nlohmann/json.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace clawspan {
namespace core {

// ─────────────────────────────────────────────────────────────────────────────
// 任务上下文（Task-Scoped Security Context）
// Phase 3 引入，Phase 1/2 中 SecurityContext::task 为 nullopt，安全模块自动降级。
// ─────────────────────────────────────────────────────────────────────────────

// TaskContext 描述一次 Agent 任务的完整上下文。
//
// root_description 在顶层任务创建时由 vgui before_prompt_build hook 写入，
// 所有子任务继承且不允许修改——它是整个安全体系的唯一信任锚点。
// TaskRegistry 在实现层强制保证此不变式。
struct TaskContext
{
	std::string task_id;          // 唯一标识（daemon TaskRegistry 分配）
	std::string parent_task_id;   // 父任务 ID，空表示顶层任务
	std::string root_task_id;     // 始终指向最顶层任务（便于跨子任务聚合审查）
	std::string description;      // 当前任务描述（来自 before_prompt_build）
	std::string root_description; // 用户原始意图（从顶层任务继承，不可篡改）
	std::string session_id;       // 用户会话标识（来自 OpenClaw sessionKey）
	int64_t     started_at_ms;    // 任务开始时间戳（毫秒）
	uint32_t    op_count;         // 该任务已执行操作次数（预算控制）
};

// ─────────────────────────────────────────────────────────────────────────────
// 意图指纹与授权缓存
// ─────────────────────────────────────────────────────────────────────────────

// OperationRiskLevel 描述一次操作的风险等级，供语义一致性检测和风险分级确认使用。
//
// 等级从低到高表示操作影响范围逐步扩大：
//   ReadOnly    — 纯读取，无副作用（查看/浏览/搜索）
//   LocalWrite  — 本地写入（创建/修改/删除本地文件）
//   NetworkSend — 对外发送（发邮件/发消息/提交表单/API 调用）
//   Financial   — 金融交易（支付/购买/转账）
enum class OperationRiskLevel
{
	ReadOnly    = 1,
	LocalWrite  = 2,
	NetworkSend = 3,
	Financial   = 4,
};

// IntentFingerprint 是授权缓存的键，描述"在哪个范围内用哪个能力做什么操作"。
//
// 设计原则：不校验操作参数的具体值，只校验参数的类别或范围，从而让一次授权
// 覆盖同类操作（例如"在 Chrome 中点击任意 AXButton"），避免每次操作都弹窗。
//
// scope_value 和 target_category 的语义由各能力插件通过 PluginSecurityDecl 声明：
//   ax-eng:      scope_value = app_bundle_id，target_category = target_ax_role
//   file-access: scope_value = path 前缀，    target_category = 文件类型
//   cmd-exec:    scope_value = 命令名，        target_category = ""（可空）
//   web-access:  scope_value = 域名，          target_category = 资源类型
struct IntentFingerprint
{
	std::string capability;       // 能力模块名（如 "capability_ax"、"cmd-exec"）
	std::string operation;        // 操作名（如 "click"、"type_text"、"exec"）
	std::string scope_value;      // 操作范围（由 PluginSecurityDecl.scope_param 字段提取）
	std::string target_category;  // 目标类别，可为空（由 PluginSecurityDecl.target_param 字段提取）

	bool operator==(const IntentFingerprint& other) const
	{
		return capability     == other.capability
		    && operation      == other.operation
		    && scope_value    == other.scope_value
		    && target_category == other.target_category;
	}
};

// AuthorizationRecord 是一条已缓存的用户授权记录。
//
// 作用域为单个任务（task_id），任务结束时整批失效，防止权限跨任务扩散。
// always_confirm 用于高危操作（如金融交易），即使缓存命中仍强制弹窗确认。
struct AuthorizationRecord
{
	IntentFingerprint fingerprint;
	std::string       task_id;        // 授权所属任务（作用域）
	int64_t           granted_at_ms;  // 授权时间戳（毫秒）
	int64_t           expires_at_ms;  // 过期时间戳，-1 表示任务结束即失效
	bool              always_confirm; // true = 即使缓存命中仍需每次确认
};

// ─────────────────────────────────────────────────────────────────────────────
// 插件安全声明
// ─────────────────────────────────────────────────────────────────────────────

// PluginSecurityDecl 是能力插件向安全框架提交的安全声明。
//
// 安全框架据此自动配置通用安全保护，无需为每种工具修改安全插件代码：
//   - scope_param / target_param 告知框架如何提取 IntentFingerprint 的字段
//   - text_params_to_scan 指定哪些 string 字段需要做注入内容扫描
//   - operation_risk_levels 定义各操作的默认风险等级
//   - always_confirm_operations 声明哪些操作无论缓存是否命中都需要用户确认
//
// 注册新能力插件时只需提交一份 PluginSecurityDecl，安全框架自动生效。
struct PluginSecurityDecl
{
	// 提取 IntentFingerprint.scope_value 的 params 字段名。
	// 例如: ax-eng="app_bundle_id", file-access="path", cmd-exec="cmd", web-access="domain"
	std::string scope_param;

	// 提取 IntentFingerprint.target_category 的 params 字段名，可为空。
	// 例如: ax-eng="target_ax_role", file-access="file_extension"
	std::string target_param;

	// 需要做注入内容扫描的 string 类型 params 字段列表。
	// 例如: ax-eng=["text","value"], cmd-exec=["cmd","args"], email=["subject","body"]
	std::vector<std::string> text_params_to_scan;

	// 各操作类型的默认风险等级，未声明的操作默认为 LocalWrite。
	std::map<std::string, OperationRiskLevel> operation_risk_levels;

	// 无论意图指纹缓存是否命中，始终强制用户确认的操作列表。
	// 例如: ["pay", "delete", "send_email"]
	std::vector<std::string> always_confirm_operations;
};

// ─────────────────────────────────────────────────────────────────────────────
// 核心安全接口
// ─────────────────────────────────────────────────────────────────────────────

// SecurityAction 描述安全模块对一次调用的裁决结果。
//
// 多个安全模块的裁决由 SecurityChain 按"最严格者胜"规则合并：
//   Deny > NeedConfirm > Pass > Skip
enum class SecurityAction
{
	Pass,        // 允许通过
	Deny,        // 拒绝，操作不执行
	NeedConfirm, // 需用户在弹窗中确认后才能继续
	Skip,        // 本模块不关心此次调用，不参与裁决
};

// SecurityContext 描述一次 callCapability 调用的完整上下文，
// 作为安全模块做裁决的依据。
//
// SecurityContext 是栈上的短生命周期对象，仅在单次 callCapability 调用期间有效。
// params 持有调用方参数的常量引用，调用方须保证其在整个调用期间有效。
// task 字段为 optional，Phase 1/2 阶段为 nullopt，安全模块自动降级为操作级审查。
struct SecurityContext
{
	// 本次调用的全局唯一 ID，用于跨模块 trace 与审计日志关联。
	std::string operation_id;

	// 调用的目标能力名称，例如 "capability_ax"、"cmd-exec"。
	std::string_view capability_name;

	// 调用的操作名称，例如 "list_windows"、"perform_action"。
	std::string_view operation;

	// 调用的完整参数，安全模块可检查参数内容（如路径、文本等）。
	const nlohmann::json& params;

	// 标记本次调用是否为只读操作（list/get 类），供安全模块快速过滤。
	bool is_readonly;

	// 任务级上下文（Phase 3 新增）。
	// nullptr 表示无任务上下文（Phase 1/2 兼容模式），安全模块应优雅降级。
	// SecurityContext 是栈上短生命周期对象，TaskContext 生命周期由上层 TaskRegistry 保证，
	// 无需深拷贝，使用非拥有指针以避免每次调用的全字段 string 深拷贝开销。
	const TaskContext* task = nullptr;
};

// SecurityModuleInterface 是所有安全模块的基类，继承 ModuleInterface 获得统一的
// 生命周期管理，并新增安全检查所需的双向审查接口和任务生命周期接口。
//
// 操作级接口（每次 tool call）：
//   preHook  — 在能力执行前调用，决定是否放行（入站审查）
//   postHook — 在能力执行后调用，可修改响应内容做脱敏（出站过滤）
//
// 任务生命周期接口（Phase 3，每个任务一次）：
//   onTaskBegin — 任务开始时调用，可做任务级预授权或整体拒绝
//   onTaskEnd   — 任务结束时调用，用于清理授权缓存、写入任务级审计日志
//
// 能力注册接口（启动时，每个能力一次）：
//   onCapabilityRegistered — 能力插件注册时调用，接收 PluginSecurityDecl 配置自身行为
//
// 所有新增接口均提供默认无操作实现，现有安全模块无需修改即可继续工作。
class SecurityModuleInterface : public ModuleInterface
{
public:
	// moduleType 返回 ModuleType::Security，标识本接口为安全模块类别。
	ModuleType moduleType() const override
	{
		return ModuleType::Security;
	}

	// priority 返回模块在调用链中的执行优先级，数值越小越先执行，默认为 100。
	virtual int priority() const
	{
		return 100;
	}

	// preHook 在能力执行前被调用，对入站请求进行审查。
	//
	// 入参:
	// - ctx:    本次调用的上下文，包含 capability 名、操作名、参数、任务上下文等。
	// - reason: 输出参数，裁决为非 Pass 时填写供用户/日志查阅的原因。
	//
	// 出参/返回:
	// - SecurityAction 裁决结果。
	virtual SecurityAction preHook(const SecurityContext& ctx,
	                               std::string& reason) = 0;

	// postHook 在能力执行后被调用，对出站响应进行过滤或脱敏。
	//
	// 入参:
	// - ctx:      本次调用的上下文。
	// - response: 能力执行返回的 JSON 响应，模块可就地修改（如脱敏字段）。
	// - reason:   输出参数，裁决为非 Pass 时填写原因。
	//
	// 出参/返回:
	// - SecurityAction 裁决结果。
	virtual SecurityAction postHook(const SecurityContext& ctx,
	                                nlohmann::json& response,
	                                std::string& reason) = 0;

	// onTaskBegin 在任务开始时被调用（来自 vgui begin_task 消息，Phase 3）。
	//
	// 安全模块可在此建立任务级授权缓存、预检查任务描述、或整体拒绝可疑任务。
	// 返回 Deny 将阻止整个任务启动；返回 Skip 表示本模块不参与任务级裁决。
	//
	// 默认实现返回 Skip，现有模块无需修改。
	//
	// 入参:
	// - task:   新建任务的完整上下文（task_id、root_description、session_id 等）。
	// - reason: 输出参数，裁决为非 Pass 时填写原因。
	//
	// 出参/返回:
	// - SecurityAction 裁决结果（仅 Pass / Deny / Skip 有意义，NeedConfirm 视为 Pass）。
	virtual SecurityAction onTaskBegin(const TaskContext& task,
	                                   std::string& reason)
	{
		(void)task;
		(void)reason;
		return SecurityAction::Skip;
	}

	// onTaskEnd 在任务结束时被调用（来自 vgui end_task 消息，Phase 3）。
	//
	// 安全模块应在此清理与该任务关联的授权缓存，并写入任务级审计日志。
	// 无返回值，任务结束不可阻止。
	//
	// 默认实现为空操作，现有模块无需修改。
	//
	// 入参:
	// - task_id: 结束的任务 ID。
	// - success: true 表示任务正常完成，false 表示失败或被用户中止。
	virtual void onTaskEnd(const std::string& task_id, bool success)
	{
		(void)task_id;
		(void)success;
	}

	// onCapabilityRegistered 在能力插件注册时被调用（daemon 启动阶段，Phase 3）。
	//
	// 安全模块可据此配置针对特定能力的参数提取规则（意图指纹字段、注入扫描字段等）。
	// 典型用法：intent-fingerprint-cache 模块从 decl 中读取 scope_param / target_param，
	// 用于后续 preHook 中提取 IntentFingerprint；injection-scanner 从 text_params_to_scan
	// 中获取需要扫描的字段列表。
	//
	// 默认实现为空操作，不关心能力声明的模块无需修改。
	//
	// 入参:
	// - capability_name: 注册的能力名称（与 SecurityContext::capability_name 一致）。
	// - decl:            该能力的安全声明。
	virtual void onCapabilityRegistered(const std::string& capability_name,
	                                    const PluginSecurityDecl& decl)
	{
		(void)capability_name;
		(void)decl;
	}
};

// SecurityChain 管理一组 SecurityModuleInterface，驱动安全检查调用链。
//
// 操作级调用链（每次 tool call）：
//   runPreHook / runPostHook — 按 priority 升序执行，"最严格者胜"合并裁决
//
// 任务生命周期调用链（每个任务一次，Phase 3）：
//   runTaskBeginHook — 按 priority 升序执行，任何模块返回 Deny 即终止任务
//   runTaskEndHook   — 按 priority 升序通知所有模块，无裁决合并
//
// 能力注册通知（daemon 启动阶段，Phase 3）：
//   notifyCapabilityRegistered — 广播给所有已注册的安全模块
//
// 模块实例的生命周期由 ModuleManager 管理，SecurityChain 持有非拥有指针。
class SecurityChain
{
public:
	// registerModule 向调用链中注册一个安全模块，按 priority 升序插入。
	//
	// priority 由调用方（ModuleManager）从外部配置（TOML）传入，
	// 而非依赖模块自身声明的默认值，以支持运行时动态调整执行顺序。
	//
	// 入参:
	// - module:   安全模块指针，生命周期须长于 SecurityChain 实例。
	// - priority: 执行优先级，数值越小越先执行，默认 100。
	//
	// 注：重复注册同名模块的行为未定义，调用方须保证唯一性。
	void registerModule(SecurityModuleInterface* module, int priority = 100);

	// runPreHook 按 priority 顺序驱动所有模块执行 preHook，汇总裁决结果。
	//
	// 入参:
	// - ctx:    本次调用的上下文。
	// - reason: 输出参数，最终裁决为非 Pass 时由决定性模块填写原因。
	//
	// 出参/返回:
	// - 汇总后的 SecurityAction。
	SecurityAction runPreHook(const SecurityContext& ctx, std::string& reason);

	// runPostHook 按 priority 顺序驱动所有模块执行 postHook，汇总裁决结果。
	//
	// 入参:
	// - ctx:      本次调用的上下文。
	// - response: 能力执行结果，各模块可就地修改（如脱敏）。
	// - reason:   输出参数，最终裁决为非 Pass 时由决定性模块填写原因。
	//
	// 出参/返回:
	// - 汇总后的 SecurityAction。
	SecurityAction runPostHook(const SecurityContext& ctx,
	                           nlohmann::json& response,
	                           std::string& reason);

	// runTaskBeginHook 在任务开始时按 priority 顺序通知所有模块（Phase 3）。
	//
	// 合并策略：Deny > Pass > Skip，任何模块返回 Deny 立即短路并终止任务。
	// NeedConfirm 降级为 Pass（任务级不支持弹窗，弹窗在 preHook 中处理）。
	//
	// 入参:
	// - task:   新建任务的完整上下文。
	// - reason: 输出参数，Deny 时由决定性模块填写原因。
	//
	// 出参/返回:
	// - SecurityAction（仅 Pass / Deny 有意义）。
	SecurityAction runTaskBeginHook(const TaskContext& task, std::string& reason);

	// runTaskEndHook 在任务结束时按 priority 顺序通知所有模块（Phase 3）。
	//
	// 无裁决合并，纯通知语义，不可阻止任务结束。
	//
	// 入参:
	// - task_id: 结束的任务 ID。
	// - success: 任务是否正常完成。
	void runTaskEndHook(const std::string& task_id, bool success);

	// notifyCapabilityRegistered 在能力插件注册时广播给所有安全模块（Phase 3）。
	//
	// 入参:
	// - capability_name: 注册的能力名称。
	// - decl:            该能力的安全声明。
	void notifyCapabilityRegistered(const std::string& capability_name,
	                                const PluginSecurityDecl& decl);

private:
	// modules_ 按 priority 升序排列，每个元素为 {priority, module} pair。
	std::vector<std::pair<int, SecurityModuleInterface*>> modules_;
};

} // namespace core
} // namespace clawspan
