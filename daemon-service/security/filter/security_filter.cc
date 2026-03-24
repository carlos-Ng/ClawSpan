#include "security_filter.h"

#include "common/log.h"

#include <toml++/toml.hpp>

#include <string_view>

namespace clawspan {
namespace security {

// ─── 模块身份 ────────────────────────────────────────────────────────────────

const char* SecurityFilter::name()    const { return "security_filter"; }
const char* SecurityFilter::version() const { return "1.0.0"; }

// ─── 生命周期 ────────────────────────────────────────────────────────────────

// init 从 ModuleConfig 读取 "rules_file" 参数并加载规则。
//
// 入参:
// - config: 模块配置，须包含键 "rules_file"。
//
// 出参/返回:
// - Result::Ok()：加载成功（或未配置 rules_file，允许无规则运行）。
// - Result::Error(CONFIG_PARSE_ERROR)：规则文件存在但格式错误。
Result<void> SecurityFilter::init(const core::ModuleConfig& config)
{
	auto rules_file_opt = config.getString("rules_file");
	if (!rules_file_opt.has_value() || rules_file_opt->empty()) {
		LOG_WARN("security_filter: 'rules_file' not configured — "
		         "running WITHOUT security rules; all operations will PASS");
		return Result<void>::Ok();
	}

	// 相对路径依赖进程 CWD，若 daemon 以非预期目录启动（如 Windows 服务、
	// 任务计划程序），规则文件将静默缺失，导致安全策略失效。
	// 此处提前记录 WARN，便于运维排查；生产部署建议使用绝对路径。
	const auto& rules_file = *rules_file_opt;
	bool is_relative = true;
#ifdef _WIN32
	// Windows 绝对路径：以驱动器号（如 C:\）或 UNC 路径（\\server）开头
	if (rules_file.size() >= 3 &&
	    ((rules_file[1] == ':' && (rules_file[2] == '\\' || rules_file[2] == '/')) ||
	     (rules_file[0] == '\\' && rules_file[1] == '\\'))) {
		is_relative = false;
	}
#else
	if (!rules_file.empty() && rules_file[0] == '/') {
		is_relative = false;
	}
#endif
	if (is_relative) {
		LOG_WARN("security_filter: 'rules_file' is a relative path ('{}') — "
		         "depends on process CWD; use an absolute path in production to "
		         "prevent rules from silently missing on unexpected startup directory",
		         rules_file);
	}

	auto result = loadRules(rules_file);
	if (result.failure()) {
		return result;
	}

	LOG_INFO("security_filter: loaded {} deny rule(s) and {} confirm rule(s)",
	         deny_rules_.size(), confirm_rules_.size());
	return Result<void>::Ok();
}

// release 释放模块资源（清空规则列表）。
void SecurityFilter::release()
{
	deny_rules_.clear();
	confirm_rules_.clear();
}

// ─── 规则加载 ────────────────────────────────────────────────────────────────

// parseRuleArray 从 TOML array 中解析规则列表，填充到 out。
//
// 入参:
// - arr: TOML 数组节点，可为 nullptr（函数直接返回）。
// - out: 输出规则列表，解析结果追加到此容器。
static void parseRuleArray(const toml::array* arr, std::vector<FilterRule>& out)
{
	if (arr == nullptr) {
		return;
	}

	for (const auto& elem : *arr) {
		const auto* tbl = elem.as_table();
		if (tbl == nullptr) {
			continue;
		}

		FilterRule rule;

		if (auto v = tbl->get_as<std::string>("capability")) {
			rule.capability = **v;
		}
		if (const auto* ops = tbl->get_as<toml::array>("operations")) {
			for (const auto& op : *ops) {
				if (op.is_string()) {
					rule.operations.push_back(**op.as_string());
				}
			}
		}
		if (auto v = tbl->get_as<std::string>("reason")) {
			rule.reason = **v;
		}
		if (auto v = tbl->get_as<std::string>("params_field")) {
			rule.params_field = **v;
		}
		if (const auto* pats = tbl->get_as<toml::array>("params_patterns")) {
			for (const auto& pat : *pats) {
				if (!pat.is_string()) {
					continue;
				}
				try {
					rule.patterns.emplace_back(
					    **pat.as_string(),
					    std::regex::ECMAScript | std::regex::icase);
				} catch (const std::regex_error& e) {
					LOG_WARN("security_filter: invalid regex '{}': {}",
					         **pat.as_string(), e.what());
				}
			}
		}

		out.push_back(std::move(rule));
	}
}

// loadRules 从 TOML 文件加载 deny / confirm 规则列表。
//
// 直接尝试解析文件，不做先探测再打开的 TOCTOU 两阶段操作：
//   - toml::parse_error：文件存在但格式错误 → CONFIG_PARSE_ERROR
//   - std::exception（含文件不存在的 I/O 错误）→ 允许无规则启动，返回 Ok
Result<void> SecurityFilter::loadRules(const std::string& rules_file)
{
	toml::table tbl;
	try {
		tbl = toml::parse_file(rules_file);
	} catch (const toml::parse_error& e) {
		LOG_ERROR("security_filter: failed to parse rules file '{}': {}",
		          rules_file, e.description().data());
		return Result<void>::Error(Status::CONFIG_PARSE_ERROR,
		                           "security rules file parse error");
	} catch (const std::exception& e) {
		LOG_WARN("security_filter: cannot open rules file '{}': {}, no rules loaded",
		         rules_file, e.what());
		return Result<void>::Ok();
	}

	parseRuleArray(tbl.get_as<toml::array>("deny"),    deny_rules_);
	parseRuleArray(tbl.get_as<toml::array>("confirm"), confirm_rules_);
	return Result<void>::Ok();
}

// ─── 规则匹配 ────────────────────────────────────────────────────────────────

// matchRule 检查 ctx 是否命中给定规则，命中时返回 true。
//
// 入参:
// - rule: 待匹配的规则。
// - ctx:  本次调用的安全上下文。
//
// 出参/返回:
// - true：所有条件均满足，规则命中。
// - false：任一条件不满足，规则未命中。
bool SecurityFilter::matchRule(const FilterRule&            rule,
                               const core::SecurityContext& ctx) const
{
	// 1. 能力名匹配
	if (!rule.capability.empty() && rule.capability != "*") {
		if (ctx.capability_name != rule.capability) {
			return false;
		}
	}

	// 2. 操作名匹配
	if (!rule.operations.empty()) {
		bool found = false;
		for (const auto& op : rule.operations) {
			if (ctx.operation == op) {
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
	}

	// 3. params 字段正则匹配（params_field 为空时跳过）
	if (!rule.params_field.empty()) {
		if (!ctx.params.contains(rule.params_field)) {
			return false;
		}

		const auto& field = ctx.params.at(rule.params_field);
		std::string field_str;
		if (field.is_string()) {
			field_str = field.get<std::string>();
		} else {
			field_str = field.dump();
		}

		// patterns 为空 = 字段存在即命中；否则须匹配任意一条 pattern
		if (!rule.patterns.empty()) {
			bool matched = false;
			for (const auto& pattern : rule.patterns) {
				if (std::regex_search(field_str, pattern)) {
					matched = true;
					break;
				}
			}
			if (!matched) {
				return false;
			}
		}
	}

	return true;
}

// ─── Hook 实现 ───────────────────────────────────────────────────────────────

// preHook 按 deny → confirm → pass 顺序匹配规则。
//
// 入参:
// - ctx:    本次调用的安全上下文。
// - reason: 输出参数，规则命中时填写对应规则的 reason 字段。
//
// 出参/返回:
// - SecurityAction::Deny / NeedConfirm / Pass。
core::SecurityAction SecurityFilter::preHook(const core::SecurityContext& ctx,
                                              std::string&                 reason)
{
	// 1. 拒绝规则优先
	for (const auto& rule : deny_rules_) {
		if (matchRule(rule, ctx)) {
			reason = rule.reason;
			LOG_INFO("security_filter: deny  cap={} op={} reason={}",
			         ctx.capability_name, ctx.operation, reason);
			return core::SecurityAction::Deny;
		}
	}

	// 2. 确认规则次之
	for (const auto& rule : confirm_rules_) {
		if (matchRule(rule, ctx)) {
			reason = rule.reason;
			LOG_INFO("security_filter: confirm  cap={} op={} reason={}",
			         ctx.capability_name, ctx.operation, reason);
			return core::SecurityAction::NeedConfirm;
		}
	}

	return core::SecurityAction::Pass;
}

// postHook 返回 Skip（当前版本不参与出站过滤，不干预出站裁决）。
core::SecurityAction SecurityFilter::postHook(const core::SecurityContext& /*ctx*/,
                                               nlohmann::json&              /*response*/,
                                               std::string&                 /*reason*/)
{
	return core::SecurityAction::Skip;
}

} // namespace security
} // namespace clawspan

// ─── 模块导出 ────────────────────────────────────────────────────────────────

CLAWSPAN_MODULE_EXPORT(clawspan::security::SecurityFilter)
