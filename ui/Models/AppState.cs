using System;
using System.Collections.Generic;

namespace ClawSpanUI.Models
{

// AppState 持有全局运行时状态，是 UI 与 Channel 之间的桥梁。
// 所有公开属性的写操作均通过专用方法完成，内部加锁保证线程安全。
// 事件在锁外触发，避免死锁。
public class AppState
{
	private readonly object _sync = new();

	// ─────────────────────────────────────────────────────────
	// 连接状态
	// ─────────────────────────────────────────────────────────

	// Channel 2 Named Pipe 是否已连接（即 ClawSpan UI 与 daemon 的确认通道）
	public bool ChannelConnected { get; private set; }

	// Daemon 是否在线（首次收到 status 消息后为 true）
	public bool DaemonRunning { get; private set; }

	// VM 状态: "running" / "stopped" / "starting"
	public string VmState { get; private set; } = "stopped";

	// OpenClaw Gateway 状态: "online" / "offline" / "unknown"
	public string OpenClawState { get; private set; } = "unknown";

	// 调用通道状态: "active" / "idle"
	public string ChannelState { get; private set; } = "idle";

	// ─────────────────────────────────────────────────────────
	// 任务状态
	// ─────────────────────────────────────────────────────────

	// 当前活动任务，null 表示无任务
	public TaskRecord? CurrentTask { get; private set; }

	// 历史任务列表（已结束的任务，按结束时间倒序）
	public List<TaskRecord> TaskHistory { get; } = new();

	// ─────────────────────────────────────────────────────────
	// 状态变更事件（在 UI 线程上订阅时需自行 Invoke 回主线程）
	// ─────────────────────────────────────────────────────────

	// 连接状态变更（ChannelConnected / DaemonRunning / VmState / OpenClawState / ChannelState 之一变化时触发）
	public event Action? OnConnectionChanged;

	// 新任务开始
	public event Action<TaskRecord>? OnTaskBegin;

	// 任务结束，参数为 task_id
	public event Action<string>? OnTaskEnd;

	// 新操作记录追加到当前任务
	public event Action<OperationRecord>? OnOperationLogged;

	// ─────────────────────────────────────────────────────────
	// 状态更新方法（由 MessageDispatcher 调用，运行在 Channel 线程）
	// ─────────────────────────────────────────────────────────

	// UpdateChannelConnected 更新 Channel 2 的连接状态。
	//
	// 入参:
	// - connected: true 表示已连接，false 表示已断开。
	public void UpdateChannelConnected(bool connected)
	{
		lock (_sync) {
			if (ChannelConnected == connected) {
				return;
			}
			ChannelConnected = connected;
			if (!connected) {
				DaemonRunning = false;
				VmState = "stopped";
				OpenClawState = "unknown";
				ChannelState = "idle";
			}
		}
		OnConnectionChanged?.Invoke();
	}

	// UpdateFromStatus 处理 status 消息，更新三维系统状态。
	public void UpdateFromStatus(string vm, string openclaw, string channel)
	{
		bool changed;
		lock (_sync) {
			changed = !DaemonRunning
			       || VmState != vm
			       || OpenClawState != openclaw
			       || ChannelState != channel;
			DaemonRunning = true;
			VmState = vm;
			OpenClawState = openclaw;
			ChannelState = channel;
		}
		if (changed) {
			OnConnectionChanged?.Invoke();
		}
	}

	// BeginTask 创建并激活新任务。
	//
	// 入参:
	// - taskId:          任务唯一 ID。
	// - rootDescription: Root Task 描述（用户原始意图）。
	//
	// 返回: 创建的 TaskRecord。
	public TaskRecord BeginTask(string taskId, string rootDescription)
	{
		var task = new TaskRecord {
			TaskId = taskId,
			RootDescription = rootDescription,
			StartTime = DateTime.Now,
		};
		lock (_sync) {
			CurrentTask = task;
		}
		OnTaskBegin?.Invoke(task);
		return task;
	}

	// EndTask 结束指定任务，将其移入历史记录。
	//
	// 入参:
	// - taskId: 要结束的任务 ID。
	public void EndTask(string taskId)
	{
		TaskRecord? task;
		lock (_sync) {
			task = CurrentTask;
			if (task == null || task.TaskId != taskId) {
				return;
			}
			task.EndTime = DateTime.Now;
			CurrentTask = null;
			TaskHistory.Insert(0, task);
		}
		OnTaskEnd?.Invoke(taskId);
	}

	// AppendOperation 向当前任务追加一条操作记录。
	// 若当前无活动任务，忽略此记录。
	//
	// 入参:
	// - record: 要追加的操作记录。
	public void AppendOperation(OperationRecord record)
	{
		TaskRecord? task;
		lock (_sync) {
			task = CurrentTask;
			if (task == null) {
				return;
			}
			task.Operations.Add(record);
		}
		OnOperationLogged?.Invoke(record);
	}

	// AddCachedFingerprint 向当前任务的缓存指纹列表中追加一条记录。
	// 若当前无活动任务，忽略此操作。
	//
	// 入参:
	// - fingerprint: Intent Fingerprint 字符串。
	public void AddCachedFingerprint(string fingerprint)
	{
		lock (_sync) {
			if (CurrentTask == null) {
				return;
			}
			if (!CurrentTask.CachedFingerprints.Contains(fingerprint)) {
				CurrentTask.CachedFingerprints.Add(fingerprint);
			}
		}
	}
}

} // namespace ClawSpanUI.Models
