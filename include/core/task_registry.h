#pragma once

#include "core/base/security.h"
#include "common/error.h"

#include <memory>
#include <string>
#include <vector>

namespace clawspan {
namespace core {

// ─────────────────────────────────────────────────────────────────────────────
// TaskRegistry：Agent 任务生命周期与意图指纹授权缓存的统一管理器
//
// 职责：
//   1. 注册与注销活跃任务（beginTask / endTask），分配唯一 task_id
//   2. 维护每个任务的上下文（TaskContext），供 CapabilityService 填充
//      SecurityContext.task，使安全模块可感知任务级别的状态
//   3. 管理每个任务的意图指纹授权缓存（cacheAuthorization / checkAuthorization）：
//      用户在确认弹窗中勾选「本任务内相同操作不再询问」时，框架调用 cacheAuthorization
//      写入缓存；再次执行相同类型操作时，checkAuthorization 命中缓存，跳过弹窗
//   4. 任务结束时（endTask）自动清除全部关联授权，阻止权限跨任务扩散
//
// 线程安全：
//   所有公开方法均可从多线程并发调用。
//   findTask 返回的 const TaskContext* 在对应 endTask 调用前始终有效；
//   调用方须保证在持有指针期间，对应任务不会被 endTask 移除。
// ─────────────────────────────────────────────────────────────────────────────
class TaskRegistry
{
public:
	TaskRegistry();
	~TaskRegistry();

	TaskRegistry(const TaskRegistry&)            = delete;
	TaskRegistry& operator=(const TaskRegistry&) = delete;

	// beginTask 注册一个新任务，返回 daemon 分配的唯一 task_id。
	//
	// 父子任务关系：
	//   parent_task_id 为空时，本任务为顶层任务：
	//     root_task_id     = 分配的 task_id
	//     root_description = root_description 参数（不可为空时使用 description 兜底）
	//   parent_task_id 非空时，本任务继承父任务的 root_task_id 与 root_description
	//   （信任锚点，不可被子任务覆盖）。若父任务不存在，降级为独立顶层任务并打 WARN 日志。
	//
	// 入参:
	// - description:      当前任务描述（来自 before_prompt_build hook）。
	// - root_description: 用户原始意图；顶层任务时传入，子任务传空字符串（自动继承）。
	// - parent_task_id:   父任务 ID，空字符串表示顶层任务。
	// - session_id:       用户会话标识（来自 OpenClaw sessionKey）。
	//
	// 出参/返回:
	// - 分配的 task_id 字符串（格式 "task-N"，N 为单调递增整数）。
	std::string beginTask(const std::string& description,
	                      const std::string& root_description,
	                      const std::string& parent_task_id,
	                      const std::string& session_id);

	// endTask 结束并移除活跃任务，同时清除其全部关联的意图指纹授权缓存。
	//
	// 任务不存在时打 WARN 日志，不抛异常（幂等）。
	//
	// 入参:
	// - task_id: 要结束的任务 ID。
	void endTask(const std::string& task_id);

	// findTask 查找活跃任务，返回其上下文的非拥有指针。
	//
	// 返回的指针在对应 endTask 调用前始终有效（由 TaskRegistry 生命周期保证）。
	// 任务不存在时返回 nullptr；调用方可据此将 SecurityContext.task 设为 nullptr，
	// 安全模块自动降级为无任务上下文模式。
	//
	// 入参:
	// - task_id: 要查找的任务 ID。
	//
	// 出参/返回:
	// - 任务上下文指针；任务不存在时返回 nullptr。
	const TaskContext* findTask(const std::string& task_id) const;

	// cacheAuthorization 为指定任务缓存一条意图指纹授权记录（任务级作用域）。
	//
	// 当 UIService::askConfirm 返回 trust_fingerprint=true 时由 CapabilityService 调用。
	// 重复缓存相同指纹为幂等操作（不产生重复条目）。
	// 任务不存在时打 WARN 日志并静默返回。
	//
	// 入参:
	// - task_id:     授权所属的任务 ID。
	// - fingerprint: 待缓存的意图指纹。
	void cacheAuthorization(const std::string&       task_id,
	                        const IntentFingerprint& fingerprint);

	// checkAuthorization 检查意图指纹是否已在指定任务内获得用户授权。
	//
	// 在 SecurityChain::runPreHook 返回 NeedConfirm 之前，由 CapabilityService
	// 调用此方法。若命中缓存，直接放行，不再弹出确认弹窗。
	//
	// 入参:
	// - task_id:     目标任务 ID。
	// - fingerprint: 待检查的意图指纹。
	//
	// 出参/返回:
	// - true：本任务内已授权该指纹。
	// - false：未授权，或任务不存在。
	bool checkAuthorization(const std::string&       task_id,
	                        const IntentFingerprint& fingerprint) const;

	// incrementOpCount 递增指定任务的操作计数器。
	//
	// 由 CapabilityService 在每次操作成功执行后调用，用于预算控制与审计。
	// 任务不存在时静默返回。
	//
	// 入参:
	// - task_id: 目标任务 ID。
	void incrementOpCount(const std::string& task_id);

	// activeTasks 返回当前所有活跃任务的 task_id 列表（顺序不保证）。
	//
	// 供状态查询（UI 推送、审计日志、测试）使用。
	//
	// 出参/返回:
	// - 活跃 task_id 列表；无活跃任务时返回空列表。
	std::vector<std::string> activeTasks() const;

private:
	struct Implement;
	std::unique_ptr<Implement> impl_;
};

} // namespace core
} // namespace clawspan
