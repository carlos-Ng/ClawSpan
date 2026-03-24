#include "core/task_registry.h"

#include "common/log.h"

#include <atomic>
#include <chrono>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace clawspan {
namespace core {

// ─── TaskEntry ──────────────────────────────────────────────────────────────
//
// TaskEntry 是 TaskRegistry 内部持有的任务记录。
// 将 TaskContext 与授权缓存共置于同一对象中，减少 map 二次查找。
//
// unique_ptr<TaskEntry> 存于 unordered_map：
//   C++ 标准保证 unordered_map 的 erase 不会使其他元素的指针/引用失效，
//   insert 在不触发 rehash 时同样不失效；rehash（容量超阈值）可能失效，
//   由 Implement 的 reserve 策略在高负载场景规避。
//   总体语义：调用方持有 TaskContext* 期间，对应任务不应被 endTask 移除，
//   该约束由业务流程（任务存活期间 capability 才在执行）天然保证。
struct TaskEntry
{
	TaskContext                  ctx;
	std::vector<IntentFingerprint> authorizations; // 本任务内已授权的意图指纹列表
};

// ─── Implement ──────────────────────────────────────────────────────────────

struct TaskRegistry::Implement
{
	// tasks_：task_id → TaskEntry，通过 unique_ptr 保证 TaskContext* 稳定性
	std::unordered_map<std::string, std::unique_ptr<TaskEntry>> tasks_;

	// rw_mutex_：读写分离锁
	//   shared_lock  → findTask、checkAuthorization、activeTasks（只读）
	//   unique_lock  → beginTask、endTask、cacheAuthorization、incrementOpCount（写）
	mutable std::shared_mutex rw_mutex_;

	// next_id_：任务 ID 序列号，自 1 开始单调递增
	std::atomic<uint64_t> next_id_{1};

	// generateTaskId 生成格式为 "task-N" 的唯一 task_id。
	std::string generateTaskId()
	{
		return "task-" + std::to_string(next_id_.fetch_add(1, std::memory_order_relaxed));
	}

	// nowMs 返回当前 UTC 毫秒时间戳。
	static int64_t nowMs()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(
		    system_clock::now().time_since_epoch()).count();
	}
};

// ─── TaskRegistry 方法实现 ───────────────────────────────────────────────────

TaskRegistry::TaskRegistry()
	: impl_(std::make_unique<Implement>())
{}

TaskRegistry::~TaskRegistry() = default;

// beginTask 注册新任务，分配 task_id，处理父子任务继承关系。
std::string TaskRegistry::beginTask(const std::string& description,
                                    const std::string& root_description,
                                    const std::string& parent_task_id,
                                    const std::string& session_id)
{
	std::unique_lock<std::shared_mutex> lock(impl_->rw_mutex_);

	std::string task_id = impl_->generateTaskId();

	TaskContext ctx;
	ctx.task_id       = task_id;
	ctx.description   = description;
	ctx.session_id    = session_id;
	ctx.started_at_ms = Implement::nowMs();
	ctx.op_count      = 0;

	if (parent_task_id.empty()) {
		// 顶层任务：root = self，描述即意图
		ctx.parent_task_id   = "";
		ctx.root_task_id     = task_id;
		ctx.root_description = root_description.empty() ? description : root_description;

	} else {
		ctx.parent_task_id = parent_task_id;

		auto it = impl_->tasks_.find(parent_task_id);
		if (it != impl_->tasks_.end()) {
			// 继承父任务的信任锚点（root_task_id 与 root_description 不可由子任务覆盖）
			ctx.root_task_id     = it->second->ctx.root_task_id;
			ctx.root_description = it->second->ctx.root_description;
		} else {
			// 父任务不存在（可能已提前结束），降级为独立顶层任务并告警
			LOG_WARN("task registry: parent task '{}' not found, "
			         "treating task '{}' as root task",
			         parent_task_id, task_id);
			ctx.root_task_id     = task_id;
			ctx.root_description = root_description.empty() ? description : root_description;
		}
	}

	auto entry = std::make_unique<TaskEntry>();
	entry->ctx = std::move(ctx);
	impl_->tasks_.emplace(task_id, std::move(entry));

	LOG_INFO("task registry: begin task_id='{}' parent='{}' description='{}'",
	         task_id,
	         parent_task_id.empty() ? "(root)" : parent_task_id,
	         description);

	return task_id;
}

// endTask 结束并移除任务，清除全部关联授权缓存。
void TaskRegistry::endTask(const std::string& task_id)
{
	std::unique_lock<std::shared_mutex> lock(impl_->rw_mutex_);

	auto it = impl_->tasks_.find(task_id);
	if (it == impl_->tasks_.end()) {
		LOG_WARN("task registry: endTask called for unknown task_id='{}'", task_id);
		return;
	}

	const size_t auth_count  = it->second->authorizations.size();
	const uint32_t op_count  = it->second->ctx.op_count;
	impl_->tasks_.erase(it);

	LOG_INFO("task registry: end task_id='{}' ops={} cleared_auths={}",
	         task_id, op_count, auth_count);
}

// findTask 查找活跃任务，返回其 TaskContext 的非拥有 const 指针。
const TaskContext* TaskRegistry::findTask(const std::string& task_id) const
{
	std::shared_lock<std::shared_mutex> lock(impl_->rw_mutex_);

	auto it = impl_->tasks_.find(task_id);
	if (it == impl_->tasks_.end()) {
		return nullptr;
	}
	return &it->second->ctx;
}

// cacheAuthorization 为任务写入意图指纹授权记录（幂等：重复添加无效）。
void TaskRegistry::cacheAuthorization(const std::string&       task_id,
                                      const IntentFingerprint& fingerprint)
{
	std::unique_lock<std::shared_mutex> lock(impl_->rw_mutex_);

	auto it = impl_->tasks_.find(task_id);
	if (it == impl_->tasks_.end()) {
		LOG_WARN("task registry: cacheAuthorization for unknown task_id='{}'", task_id);
		return;
	}

	auto& auths = it->second->authorizations;
	for (const auto& existing : auths) {
		if (existing == fingerprint) {
			return; // 已存在，幂等
		}
	}

	auths.push_back(fingerprint);

	LOG_DEBUG("task registry: cached auth task_id='{}' cap='{}' op='{}' scope='{}'",
	          task_id,
	          fingerprint.capability,
	          fingerprint.operation,
	          fingerprint.scope_value);
}

// checkAuthorization 检查意图指纹是否已在本任务内获得用户授权。
bool TaskRegistry::checkAuthorization(const std::string&       task_id,
                                      const IntentFingerprint& fingerprint) const
{
	std::shared_lock<std::shared_mutex> lock(impl_->rw_mutex_);

	auto it = impl_->tasks_.find(task_id);
	if (it == impl_->tasks_.end()) {
		return false;
	}
	for (const auto& auth : it->second->authorizations) {
		if (auth == fingerprint) {
			return true;
		}
	}
	return false;
}

// incrementOpCount 递增任务的操作计数器（任务不存在时静默返回）。
void TaskRegistry::incrementOpCount(const std::string& task_id)
{
	std::unique_lock<std::shared_mutex> lock(impl_->rw_mutex_);

	auto it = impl_->tasks_.find(task_id);
	if (it == impl_->tasks_.end()) {
		return;
	}
	++it->second->ctx.op_count;
}

// activeTasks 返回所有活跃任务的 task_id 列表（顺序不保证）。
std::vector<std::string> TaskRegistry::activeTasks() const
{
	std::shared_lock<std::shared_mutex> lock(impl_->rw_mutex_);

	std::vector<std::string> result;
	result.reserve(impl_->tasks_.size());
	for (const auto& [id, _] : impl_->tasks_) {
		result.push_back(id);
	}
	return result;
}

} // namespace core
} // namespace clawspan
