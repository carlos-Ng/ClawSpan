using System;
using System.Collections.Generic;
using System.Linq;

namespace ClawSpanUI.Models
{

// TaskRecord 表示一个完整的 Agent 任务，包含 Root Task 和全部操作记录。
// 对应 beginTask / endTask 的生命周期。
public class TaskRecord
{
	// 任务唯一 ID，由 OpenClaw 插件生成
	public string TaskId { get; set; } = string.Empty;

	// Root Task：用户原始意图描述，在 before_prompt_build 时捕获
	public string RootDescription { get; set; } = string.Empty;

	// 任务开始时间
	public DateTime StartTime { get; set; }

	// 任务结束时间，null 表示任务仍在进行
	public DateTime? EndTime { get; set; }

	// 任务是否仍在进行
	public bool IsActive => EndTime == null;

	// 本任务内已执行的操作记录（按时间顺序）
	public List<OperationRecord> Operations { get; } = new();

	// 本任务内已缓存的 Intent Fingerprint 列表
	// 格式：operation|app_bundle_id|ax_role
	public List<string> CachedFingerprints { get; } = new();

	// ─────────────────────────────────────────────────────────
	// 统计便捷属性
	// ─────────────────────────────────────────────────────────

	// 已允许操作次数（含自动放行、用户允许、缓存放行）
	public int AllowedCount => Operations.Count(o => o.IsAllowed);

	// 已拦截操作次数（规则拒绝 + 用户拒绝）
	public int DeniedCount => Operations.Count(o => o.IsDenied);

	// 总操作次数
	public int TotalCount => Operations.Count;
}

} // namespace ClawSpanUI.Models
