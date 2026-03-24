#include "core/base/module_config.h"

namespace clawspan {
namespace core {

std::optional<std::string> ModuleConfig::getString(std::string_view key) const
{
	auto it = entries_.find(std::string(key));
	if (it == entries_.end()) return std::nullopt;
	const auto* v = std::get_if<std::string>(&it->second);
	return v ? std::optional<std::string>(*v) : std::nullopt;
}

std::optional<int64_t> ModuleConfig::getInt(std::string_view key) const
{
	auto it = entries_.find(std::string(key));
	if (it == entries_.end()) return std::nullopt;
	const auto* v = std::get_if<int64_t>(&it->second);
	return v ? std::optional<int64_t>(*v) : std::nullopt;
}

std::optional<double> ModuleConfig::getDouble(std::string_view key) const
{
	auto it = entries_.find(std::string(key));
	if (it == entries_.end()) return std::nullopt;
	const auto* v = std::get_if<double>(&it->second);
	return v ? std::optional<double>(*v) : std::nullopt;
}

std::optional<bool> ModuleConfig::getBool(std::string_view key) const
{
	auto it = entries_.find(std::string(key));
	if (it == entries_.end()) return std::nullopt;
	const auto* v = std::get_if<bool>(&it->second);
	return v ? std::optional<bool>(*v) : std::nullopt;
}

// set 写入一个配置项。
void ModuleConfig::set(std::string key, Value value)
{
	entries_[std::move(key)] = std::move(value);
}

} // namespace core
} // namespace clawspan
