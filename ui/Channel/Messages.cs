using System.Text.Json;
using System.Text.Json.Serialization;

namespace ClawSpanUI.Channel
{

// UI 事件通道所有消息类型定义（Daemon ↔ UI 双向通信协议）

// ─────────────────────────────────────────────────────────────
// Daemon → UI 方向
// ─────────────────────────────────────────────────────────────

// 消息基类，用于按 type 字段分发
public class BaseMessage
{
	[JsonPropertyName("type")]
	public string Type { get; set; } = string.Empty;
}

// status 消息 - daemon 推送当前系统状态
// 连接时立刻发送一次，状态变更时再次发送
public class StatusMessage : BaseMessage
{
	// VM 状态: "running" / "stopped" / "starting"
	[JsonPropertyName("vm")]
	public string Vm { get; set; } = string.Empty;

	// OpenClaw Gateway 状态: "online" / "offline" / "unknown"
	[JsonPropertyName("openclaw")]
	public string OpenClaw { get; set; } = string.Empty;

	// 调用通道状态: "active" / "idle"
	[JsonPropertyName("channel")]
	public string Channel { get; set; } = string.Empty;
}

// task_begin 消息 - 新任务开始
// 由 OpenClaw 插件在 before_prompt_build hook 中触发
public class TaskBeginMessage : BaseMessage
{
	[JsonPropertyName("task_id")]
	public string TaskId { get; set; } = string.Empty;

	// Root Task：用户原始意图，在 LLM 处理前捕获，不可篡改
	[JsonPropertyName("root_description")]
	public string RootDescription { get; set; } = string.Empty;
}

// task_end 消息 - 任务结束
// 由 OpenClaw 插件在 agent_end hook 中触发，本任务信任上下文同步失效
public class TaskEndMessage : BaseMessage
{
	[JsonPropertyName("task_id")]
	public string TaskId { get; set; } = string.Empty;
}

// op_log 消息 - 单次操作执行结果
// 每次操作经 SecurityChain 处理后推送
public class OpLogMessage : BaseMessage
{
	[JsonPropertyName("task_id")]
	public string TaskId { get; set; } = string.Empty;

	// 操作名称，如 click / key_combo / set_value
	[JsonPropertyName("operation")]
	public string Operation { get; set; } = string.Empty;

	// 执行结果：allowed / denied / confirmed / cached
	[JsonPropertyName("result")]
	public string Result { get; set; } = string.Empty;

	// 来源：auto_allow / rule_deny / user_confirm / fingerprint_cache
	[JsonPropertyName("source")]
	public string Source { get; set; } = string.Empty;

	// 操作详情，如目标应用名称、元素名称等可读文本
	[JsonPropertyName("detail")]
	public string Detail { get; set; } = string.Empty;

	[JsonPropertyName("timestamp")]
	public long Timestamp { get; set; }
}

// confirm 消息 - 请求用户确认某项操作
// daemon 在 SecurityChain 返回 NeedConfirm 后通过 UI 事件通道推送
public class ConfirmMessage : BaseMessage
{
	// 请求 ID，用于匹配 confirm_response（daemon 使用字符串 ID）
	[JsonPropertyName("confirm_id")]
	public string ConfirmId { get; set; } = string.Empty;

	[JsonPropertyName("task_id")]
	public string TaskId { get; set; } = string.Empty;

	// Root Task 描述，用于在弹窗中展示当前任务上下文
	[JsonPropertyName("root_description")]
	public string RootDescription { get; set; } = string.Empty;

	[JsonPropertyName("capability")]
	public string Capability { get; set; } = string.Empty;

	// 操作名称，如 click / set_value / key_combo
	[JsonPropertyName("operation")]
	public string Operation { get; set; } = string.Empty;

	// 操作参数原始 JSON，如 element_path / value / keys
	[JsonPropertyName("params")]
	public JsonElement Params { get; set; }

	// 安全规则中配置的拒绝/确认原因
	[JsonPropertyName("reason")]
	public string Reason { get; set; } = string.Empty;

	// Intent Fingerprint，格式：operation|app_bundle_id|ax_role
	// 用户勾选"本任务内不再询问"时，daemon 将此 fingerprint 加入缓存
	[JsonPropertyName("fingerprint")]
	public string Fingerprint { get; set; } = string.Empty;
}

// ─────────────────────────────────────────────────────────────
// UI → Daemon 方向
// ─────────────────────────────────────────────────────────────

// confirm_response 消息 - 用户确认结果
public class ConfirmResponse
{
	[JsonPropertyName("type")]
	public string Type { get; set; } = "confirm_response";

	// 对应 confirm 消息的 confirm_id（daemon 使用字符串 ID）
	[JsonPropertyName("confirm_id")]
	public string ConfirmId { get; set; } = string.Empty;

	// true = 用户允许，false = 用户拒绝
	[JsonPropertyName("confirmed")]
	public bool Confirmed { get; set; }

	// true = 用户勾选"本任务内相同操作不再询问"
	// daemon 将对应 fingerprint 加入本任务缓存
	[JsonPropertyName("trust_fingerprint")]
	public bool TrustFingerprint { get; set; }
}

} // namespace ClawSpanUI.Channel
