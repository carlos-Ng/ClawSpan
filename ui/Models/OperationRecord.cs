using System;

namespace ClawSpanUI.Models
{

// OperationRecord 表示一次操作的执行记录，用于操作日志展示。
public class OperationRecord
{
	// 操作名称，如 click / key_combo / set_value
	public string Operation { get; set; } = string.Empty;

	// 执行结果：allowed / denied / confirmed / cached
	public string Result { get; set; } = string.Empty;

	// 来源：auto_allow / rule_deny / user_confirm / fingerprint_cache
	public string Source { get; set; } = string.Empty;

	// 操作详情（可读文本，如目标元素名称、目标应用等）
	public string Detail { get; set; } = string.Empty;

	// 操作发生时间
	public DateTime Time { get; set; }

	// 结果是否为拒绝
	public bool IsDenied => Result == "denied";

	// 结果是否为允许（包含自动放行、缓存放行、用户允许）
	public bool IsAllowed => !IsDenied;
}

} // namespace ClawSpanUI.Models
