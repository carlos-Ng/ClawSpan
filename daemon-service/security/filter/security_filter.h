#pragma once

#include "core/base/security.h"

#include <regex>
#include <string>
#include <vector>

namespace clawspan {
namespace security {

// FilterRule 描述一条过滤规则。
//
// 匹配逻辑（全部条件须同时满足）：
//   1. capability 非空且非 "*" 时，须与 ctx.capability_name 相等。
//   2. operations 非空时，ctx.operation 须在列表中。
//   3. params_field 非空时，ctx.params 须含该字段；
//      patterns 非空时，字段值须匹配任意一个正则表达式。
struct FilterRule
{
	// 目标能力名称，"*" 或空 = 匹配所有能力。
	std::string capability;

	// 目标操作列表，空 = 匹配该能力的所有操作。
	std::vector<std::string> operations;

	// 触发时向用户或日志展示的原因描述。
	std::string reason;

	// 可选：要检查的 params 字段名称，空 = 不检查 params。
	std::string params_field;

	// 可选：预编译的 ECMAScript 正则模式，空 = 无条件触发（不做 params 匹配）。
	std::vector<std::regex> patterns;
};

// SecurityFilter 是基于规则的内容过滤安全模块。
//
// 在 preHook 中按 deny → confirm 顺序匹配规则；postHook 始终 Pass。
// 规则从外部 TOML 文件加载（路径由 ModuleConfig "rules_file" 参数指定）。
class SecurityFilter : public core::SecurityModuleInterface
{
public:
	// name 返回模块唯一标识 "security_filter"。
	const char* name()    const override;

	// version 返回模块版本。
	const char* version() const override;

	// init 从 ModuleConfig 读取 "rules_file" 参数并加载规则。
	Result<void> init(const core::ModuleConfig& config) override;

	// release 释放模块资源（清空规则列表）。
	void release() override;

	// preHook 按 deny → confirm → pass 顺序匹配规则。
	core::SecurityAction preHook(const core::SecurityContext& ctx,
	                             std::string& reason) override;

	// postHook 始终返回 Pass（当前版本不做出站过滤）。
	core::SecurityAction postHook(const core::SecurityContext& ctx,
	                               nlohmann::json& response,
	                               std::string& reason) override;

private:
	std::vector<FilterRule> deny_rules_;
	std::vector<FilterRule> confirm_rules_;

	// matchRule 检查 ctx 是否命中给定规则，命中时返回 true。
	bool matchRule(const FilterRule& rule, const core::SecurityContext& ctx) const;

	// loadRules 从 TOML 文件加载 deny / confirm 规则列表。
	//
	// 入参:
	// - rules_file: 规则 TOML 文件路径。
	//
	// 出参/返回:
	// - Result::Ok()：加载成功，或文件不存在（允许无规则启动）。
	// - Result::Error(CONFIG_PARSE_ERROR)：文件存在但格式错误，视为致命错误。
	Result<void> loadRules(const std::string& rules_file);
};

} // namespace security
} // namespace clawspan
