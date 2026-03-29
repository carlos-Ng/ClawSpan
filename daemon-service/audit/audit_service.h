#pragma once

#include "common/error.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace clawspan {
namespace audit {

struct AuditStoragePolicy
{
	size_t max_records = 10000;
};

struct AuditRecord
{
	std::string record_id;
	std::string session_id;
	std::string trace_id;
	std::string task_id;
	std::string event_type; // command_result / log_event / risk_event / custom
	std::string event_id;   // command_id or event_id
	int64_t     timestamp_ms = 0;
	std::string payload_json;
};

class AuditWriterInterface
{
public:
	virtual ~AuditWriterInterface() = default;
	virtual Status write(const AuditRecord& record) = 0;
};

class AuditSinkInterface
{
public:
	virtual ~AuditSinkInterface() = default;

	virtual Status setPolicy(const AuditStoragePolicy& policy) = 0;
	virtual AuditStoragePolicy policy() const = 0;
	virtual Status append(const AuditRecord& record) = 0;
	virtual std::vector<AuditRecord> queryBySession(std::string_view session_id, size_t limit) const = 0;
	virtual std::vector<AuditRecord> queryByTask(std::string_view task_id, size_t limit) const = 0;
	virtual size_t recordCount() const = 0;
	virtual void clear() = 0;
};

class InMemoryAuditSink final : public AuditSinkInterface
{
public:
	InMemoryAuditSink() = default;

	Status setPolicy(const AuditStoragePolicy& policy) override;
	AuditStoragePolicy policy() const override;
	Status append(const AuditRecord& record) override;
	std::vector<AuditRecord> queryBySession(std::string_view session_id, size_t limit) const override;
	std::vector<AuditRecord> queryByTask(std::string_view task_id, size_t limit) const override;
	size_t recordCount() const override;
	void clear() override;

private:
	void pruneToCapacityLocked();

	mutable std::mutex state_mutex_;
	AuditStoragePolicy policy_{};
	std::deque<AuditRecord> records_;
	std::atomic<uint64_t> sequence_{1};
};

class AuditService final : public AuditWriterInterface
{
public:
	AuditService();

	Status setSink(std::unique_ptr<AuditSinkInterface> sink);
	Status setPolicy(const AuditStoragePolicy& policy);
	AuditStoragePolicy policy() const;
	std::vector<AuditRecord> queryBySession(std::string_view session_id, size_t limit) const;
	std::vector<AuditRecord> queryByTask(std::string_view task_id, size_t limit) const;
	size_t recordCount() const;
	void clear();

	Status write(const AuditRecord& record) override;

private:
	mutable std::mutex state_mutex_;
	std::unique_ptr<AuditSinkInterface> sink_;
};

std::unique_ptr<AuditSinkInterface> createDefaultAuditSink();

} // namespace audit
} // namespace clawspan
