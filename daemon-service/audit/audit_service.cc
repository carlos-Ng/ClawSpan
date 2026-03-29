#include "audit/audit_service.h"

namespace clawspan {
namespace audit {

Status InMemoryAuditSink::setPolicy(const AuditStoragePolicy& policy)
{
	if (policy.max_records == 0) {
		return Status(Status::INVALID_ARGUMENT, "invalid audit storage policy");
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	policy_ = policy;
	pruneToCapacityLocked();
	return Status::Ok();
}

AuditStoragePolicy InMemoryAuditSink::policy() const
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return policy_;
}

Status InMemoryAuditSink::append(const AuditRecord& record)
{
	if (record.event_type.empty() || record.event_id.empty()) {
		return Status(Status::INVALID_ARGUMENT, "audit record fields are invalid");
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	AuditRecord stored = record;
	if (stored.record_id.empty()) {
		stored.record_id = "audit-" + std::to_string(sequence_.fetch_add(1));
	}
	records_.push_back(std::move(stored));
	pruneToCapacityLocked();
	return Status::Ok();
}

std::vector<AuditRecord> InMemoryAuditSink::queryBySession(std::string_view session_id, size_t limit) const
{
	std::vector<AuditRecord> out;
	if (session_id.empty() || limit == 0) {
		return out;
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	out.reserve(limit);
	for (auto it = records_.rbegin(); it != records_.rend() && out.size() < limit; ++it) {
		if (it->session_id == session_id) {
			out.push_back(*it);
		}
	}
	return out;
}

std::vector<AuditRecord> InMemoryAuditSink::queryByTask(std::string_view task_id, size_t limit) const
{
	std::vector<AuditRecord> out;
	if (task_id.empty() || limit == 0) {
		return out;
	}

	std::lock_guard<std::mutex> lk(state_mutex_);
	out.reserve(limit);
	for (auto it = records_.rbegin(); it != records_.rend() && out.size() < limit; ++it) {
		if (it->task_id == task_id) {
			out.push_back(*it);
		}
	}
	return out;
}

size_t InMemoryAuditSink::recordCount() const
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return records_.size();
}

void InMemoryAuditSink::clear()
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	records_.clear();
	policy_ = AuditStoragePolicy{};
}

void InMemoryAuditSink::pruneToCapacityLocked()
{
	while (records_.size() > policy_.max_records) {
		records_.pop_front();
	}
}

AuditService::AuditService() : sink_(createDefaultAuditSink())
{
}

Status AuditService::setSink(std::unique_ptr<AuditSinkInterface> sink)
{
	if (!sink) {
		return Status(Status::INVALID_ARGUMENT, "audit sink is null");
	}
	std::lock_guard<std::mutex> lk(state_mutex_);
	sink_ = std::move(sink);
	return Status::Ok();
}

Status AuditService::setPolicy(const AuditStoragePolicy& policy)
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return sink_->setPolicy(policy);
}

AuditStoragePolicy AuditService::policy() const
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return sink_->policy();
}

std::vector<AuditRecord> AuditService::queryBySession(std::string_view session_id, size_t limit) const
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return sink_->queryBySession(session_id, limit);
}

std::vector<AuditRecord> AuditService::queryByTask(std::string_view task_id, size_t limit) const
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return sink_->queryByTask(task_id, limit);
}

size_t AuditService::recordCount() const
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return sink_->recordCount();
}

void AuditService::clear()
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	sink_->clear();
}

Status AuditService::write(const AuditRecord& record)
{
	std::lock_guard<std::mutex> lk(state_mutex_);
	return sink_->append(record);
}

std::unique_ptr<AuditSinkInterface> createDefaultAuditSink()
{
	return std::make_unique<InMemoryAuditSink>();
}

} // namespace audit
} // namespace clawspan
