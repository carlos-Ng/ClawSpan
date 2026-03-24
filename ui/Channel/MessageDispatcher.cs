using System;
using System.Text.Json;
using ClawSpanUI.Models;

namespace ClawSpanUI.Channel
{

// MessageDispatcher 解析 Channel 2 收到的 JSON 帧，按 type 字段分发到具体事件。
// 运行在 DaemonChannel 的后台线程，事件订阅者需自行 Invoke 回 UI 线程。
public class MessageDispatcher
{
	// 收到 status 消息
	public event Action<StatusMessage>? OnStatus;

	// 收到 task_begin 消息
	public event Action<TaskBeginMessage>? OnTaskBegin;

	// 收到 task_end 消息
	public event Action<TaskEndMessage>? OnTaskEnd;

	// 收到 op_log 消息
	public event Action<OpLogMessage>? OnOpLog;

	// 收到 confirm 请求消息
	public event Action<ConfirmMessage>? OnConfirm;

	// 解析失败或收到未知类型消息时触发（调试用）
	public event Action<string>? OnUnknownMessage;

	// ─────────────────────────────────────────────────────────
	// 公开方法
	// ─────────────────────────────────────────────────────────

	// Dispatch 解析一帧 JSON 并分发到对应事件。
	// 解析失败时触发 OnUnknownMessage，不抛出异常。
	//
	// 入参:
	// - json: 从 DaemonChannel 收到的原始 JSON 字符串。
	public void Dispatch(string json)
	{
		try {
			var doc = JsonDocument.Parse(json);
			if (!doc.RootElement.TryGetProperty("type", out var typeProp)) {
				OnUnknownMessage?.Invoke(json);
				return;
			}

			var type = typeProp.GetString() ?? string.Empty;
			DispatchByType(type, json);
		} catch (JsonException) {
			OnUnknownMessage?.Invoke(json);
		}
	}

	// ─────────────────────────────────────────────────────────
	// 内部：按 type 分发
	// ─────────────────────────────────────────────────────────

	// DispatchByType 根据消息类型字符串反序列化并触发对应事件。
	//
	// 入参:
	// - type: 消息 type 字段值。
	// - json: 原始 JSON 字符串。
	private void DispatchByType(string type, string json)
	{
		switch (type) {
		case "status":
			var status = Deserialize<StatusMessage>(json);
			if (status != null) {
				OnStatus?.Invoke(status);
			}
			break;

		case "task_begin":
			var taskBegin = Deserialize<TaskBeginMessage>(json);
			if (taskBegin != null) {
				OnTaskBegin?.Invoke(taskBegin);
			}
			break;

		case "task_end":
			var taskEnd = Deserialize<TaskEndMessage>(json);
			if (taskEnd != null) {
				OnTaskEnd?.Invoke(taskEnd);
			}
			break;

		case "op_log":
			var opLog = Deserialize<OpLogMessage>(json);
			if (opLog != null) {
				OnOpLog?.Invoke(opLog);
			}
			break;

		case "confirm":
			var confirm = Deserialize<ConfirmMessage>(json);
			if (confirm != null) {
				OnConfirm?.Invoke(confirm);
			}
			break;

		default:
			OnUnknownMessage?.Invoke(json);
			break;
		}
	}

	// Deserialize 将 JSON 反序列化为指定类型，失败时返回 null。
	//
	// 入参:
	// - json: 原始 JSON 字符串。
	//
	// 返回: 反序列化成功的对象，或 null。
	private static T? Deserialize<T>(string json) where T : class
	{
		try {
			return JsonSerializer.Deserialize<T>(json);
		} catch (Exception) {
			return null;
		}
	}
}

} // namespace ClawSpanUI.Channel
