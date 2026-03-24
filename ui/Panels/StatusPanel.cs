using System;
using System.Drawing;
using System.Windows.Forms;
using ClawSpanUI.Models;

namespace ClawSpanUI.Panels
{

// StatusPanel 展示当前连接状态与活动任务概览，对应主窗口 "状态" Tab。
public class StatusPanel : UserControl
{
	// ─────────────────────────────────────────────────────────
	// 控件
	// ─────────────────────────────────────────────────────────

	// 每个指示器由两个 Label 组成：左列显示 "●/○ 名称"，右列显示状态文字
	private readonly Label _daemonDot;
	private readonly Label _daemonStatus;
	private readonly Label _vmDot;
	private readonly Label _vmStatus;
	private readonly Label _openclawDot;
	private readonly Label _openclawStatus;
	private readonly Label _channelDot;
	private readonly Label _channelStatus;

	private readonly Label _taskIdLabel;
	private readonly Label _taskDescLabel;
	private readonly Label _taskTimeLabel;
	private readonly Label _taskStatsLabel;
	private readonly Panel _taskCard;
	private readonly Label _noTaskLabel;

	private readonly AppState _state;

	private static readonly Color COLOR_OK      = Color.FromArgb(40, 167, 69);
	private static readonly Color COLOR_OFF     = Color.FromArgb(150, 150, 150);
	private static readonly Color COLOR_WARN    = Color.FromArgb(255, 165, 0);
	private static readonly Color COLOR_ACTIVE  = Color.FromArgb(0, 123, 255);

	// ─────────────────────────────────────────────────────────
	// 构造
	// ─────────────────────────────────────────────────────────

	public StatusPanel(AppState state)
	{
		_state = state;

		_daemonDot     = new Label();
		_daemonStatus  = new Label();
		_vmDot         = new Label();
		_vmStatus      = new Label();
		_openclawDot    = new Label();
		_openclawStatus = new Label();
		_channelDot    = new Label();
		_channelStatus = new Label();
		_taskIdLabel   = new Label();
		_taskDescLabel = new Label();
		_taskTimeLabel = new Label();
		_taskStatsLabel = new Label();
		_taskCard    = new Panel();
		_noTaskLabel = new Label();

		// Dpi 模式配合 (96F,96F) 才是正确搭配：运行时 DPI / 96 = 缩放系数
		AutoScaleDimensions = new SizeF(96F, 96F);
		AutoScaleMode = AutoScaleMode.Dpi;

		BuildLayout();
		RefreshConnection();
		RefreshTask();

		_state.OnConnectionChanged += () => SafeInvoke(RefreshConnection);
		_state.OnTaskBegin         += _  => SafeInvoke(RefreshTask);
		_state.OnTaskEnd           += _  => SafeInvoke(RefreshTask);
		_state.OnOperationLogged   += _  => SafeInvoke(RefreshTaskStats);

		// 面板首次可见时再刷新一次，防止连接事件在面板可见前已触发但未能更新 UI
		VisibleChanged += (_, _) => { if (Visible) { RefreshConnection(); RefreshTask(); } };
	}

	// ─────────────────────────────────────────────────────────
	// 内部：布局构建
	// ─────────────────────────────────────────────────────────

	private void BuildLayout()
	{
		SuspendLayout();
		BackColor = Color.White;

		const int LEFT     = 16;
		const int STATUS_X = 200; // 右侧状态文字固定起始列（适配最宽名称 "OpenClaw"，含 DPI 余量）
		const int ROW_STEP = 30;  // 行间距，AutoSize 后实际行高约 22px，留 8px 间距

		// ── 连接状态 ──
		Controls.Add(MakeSectionHeader("连接状态", LEFT, 14));

		int y = 44;
		AddIndicatorRow(_daemonDot,  _daemonStatus,  "Daemon",  LEFT, STATUS_X, y); y += ROW_STEP;
		AddIndicatorRow(_vmDot,      _vmStatus,      "VM",      LEFT, STATUS_X, y); y += ROW_STEP;
		AddIndicatorRow(_openclawDot, _openclawStatus, "OpenClaw", LEFT, STATUS_X, y); y += ROW_STEP;
		AddIndicatorRow(_channelDot, _channelStatus, "Channel", LEFT, STATUS_X, y);

		// 连接区块底部 ≈ 44 + 30*3 + 22 = 156，留 16px 间距 → 172
		// ── 当前任务 ──
		Controls.Add(MakeSectionHeader("当前任务", LEFT, 172));

		// AutoSize=true 的 Label 高度由字体决定，Y=200 留足空间
		_noTaskLabel.AutoSize  = true;
		_noTaskLabel.Location  = new Point(LEFT, 200);
		_noTaskLabel.Text      = "[ 无活动任务 ]";
		_noTaskLabel.Font      = new Font("微软雅黑", 9f);
		_noTaskLabel.ForeColor = Color.FromArgb(160, 160, 160);
		Controls.Add(_noTaskLabel);

		// 任务卡片（有任务时显示）
		_taskCard.Location    = new Point(LEFT, 196);
		_taskCard.Size        = new Size(360, 120);
		_taskCard.BorderStyle = BorderStyle.FixedSingle;
		_taskCard.BackColor   = Color.FromArgb(248, 249, 250);
		_taskCard.Visible     = false;
		Controls.Add(_taskCard);

		// 卡片内部 Label 全部 AutoSize=true
		_taskIdLabel.AutoSize  = true;
		_taskIdLabel.Location  = new Point(8, 8);
		_taskIdLabel.Font      = new Font("微软雅黑", 8.5f, FontStyle.Bold);
		_taskIdLabel.ForeColor = Color.FromArgb(60, 60, 60);
		_taskCard.Controls.Add(_taskIdLabel);

		// 描述可能换行，保留固定高度容纳两行
		_taskDescLabel.AutoSize     = false;
		_taskDescLabel.Location     = new Point(8, 34);
		_taskDescLabel.Size         = new Size(344, 42);
		_taskDescLabel.Font         = new Font("微软雅黑", 9f);
		_taskDescLabel.ForeColor    = Color.FromArgb(30, 30, 30);
		_taskDescLabel.AutoEllipsis = true;
		_taskCard.Controls.Add(_taskDescLabel);

		_taskTimeLabel.AutoSize  = true;
		_taskTimeLabel.Location  = new Point(8, 82);
		_taskTimeLabel.Font      = new Font("微软雅黑", 8f);
		_taskTimeLabel.ForeColor = Color.FromArgb(120, 120, 120);
		_taskCard.Controls.Add(_taskTimeLabel);

		_taskStatsLabel.AutoSize  = true;
		_taskStatsLabel.Location  = new Point(184, 82);
		_taskStatsLabel.Font      = new Font("微软雅黑", 8f);
		_taskStatsLabel.ForeColor = Color.FromArgb(80, 80, 80);
		_taskCard.Controls.Add(_taskStatsLabel);

		ResumeLayout(false);
	}

	// AddIndicatorRow 创建双列指示器行并加入 Controls。
	private void AddIndicatorRow(
		Label dotLabel, Label statusLabel, string name,
		int leftX, int statusX, int y)
	{
		var font = new Font("微软雅黑", 9f);

		// 左列：AutoSize，不限宽度
		dotLabel.AutoSize  = true;
		dotLabel.Location  = new Point(leftX, y);
		dotLabel.Font      = font;
		dotLabel.Text      = $"○  {name}";
		dotLabel.ForeColor = COLOR_OFF;
		Controls.Add(dotLabel);

		// 右列：AutoSize，固定 X 坐标确保不与左列重叠
		statusLabel.AutoSize  = true;
		statusLabel.Location  = new Point(statusX, y);
		statusLabel.Font      = font;
		statusLabel.Text      = "未连接";
		statusLabel.ForeColor = COLOR_OFF;
		Controls.Add(statusLabel);
	}

	// ─────────────────────────────────────────────────────────
	// 内部：状态刷新
	// ─────────────────────────────────────────────────────────

	private void RefreshConnection()
	{
		SetIndicator(_daemonDot, _daemonStatus, _state.DaemonRunning, "Daemon", "运行中", "未连接");

		// VM 状态：running=绿, starting=橙, stopped=灰
		var vmRunning = _state.VmState == "running";
		var vmStarting = _state.VmState == "starting";
		SetIndicatorEx(_vmDot, _vmStatus, "VM",
			vmRunning  ? COLOR_OK   : vmStarting ? COLOR_WARN : COLOR_OFF,
			vmRunning  ? "运行中"   : vmStarting ? "启动中"   : "已停止",
			vmRunning || vmStarting);

		// OpenClaw 状态：online=绿, offline=灰, unknown=灰
		var ocOnline = _state.OpenClawState == "online";
		SetIndicatorEx(_openclawDot, _openclawStatus, "OpenClaw",
			ocOnline ? COLOR_OK : COLOR_OFF,
			ocOnline ? "在线"   : _state.OpenClawState == "unknown" ? "未知" : "离线",
			ocOnline);

		// Channel 状态：active=蓝, idle=灰
		var chActive = _state.ChannelState == "active";
		SetIndicatorEx(_channelDot, _channelStatus, "Channel",
			chActive ? COLOR_ACTIVE : COLOR_OFF,
			chActive ? "工作中" : "空闲",
			chActive);
	}

	private void RefreshTask()
	{
		var task = _state.CurrentTask;
		bool hasTask = task != null;

		_noTaskLabel.Visible = !hasTask;
		_taskCard.Visible    = hasTask;

		if (task == null) return;

		_taskIdLabel.Text   = $"任务 #{task.TaskId}";
		_taskDescLabel.Text = task.RootDescription;
		_taskTimeLabel.Text = $"开始时间  {task.StartTime:HH:mm:ss}";
		RefreshTaskStats();
	}

	private void RefreshTaskStats()
	{
		var task = _state.CurrentTask;
		if (task == null) return;
		_taskStatsLabel.Text = $"已执行 {task.AllowedCount}    已拦截 {task.DeniedCount}";
	}

	// ─────────────────────────────────────────────────────────
	// 内部：工具方法
	// ─────────────────────────────────────────────────────────

	private static void SetIndicator(
		Label dotLabel, Label statusLabel,
		bool active, string name, string onText, string offText)
	{
		SetIndicatorEx(dotLabel, statusLabel, name,
			active ? COLOR_OK : COLOR_OFF,
			active ? onText : offText,
			active);
	}

	private static void SetIndicatorEx(
		Label dotLabel, Label statusLabel, string name,
		Color color, string text, bool filled)
	{
		dotLabel.Text      = filled ? $"●  {name}" : $"○  {name}";
		dotLabel.ForeColor = color;
		statusLabel.Text      = text;
		statusLabel.ForeColor = color;
	}

	// MakeSectionHeader 创建 AutoSize 区域标题 Label（高度由字体自动确定）。
	private static Label MakeSectionHeader(string text, int x, int y)
	{
		return new Label {
			Text      = text,
			Location  = new Point(x, y),
			AutoSize  = true,
			Font      = new Font("微软雅黑", 9f, FontStyle.Bold),
			ForeColor = Color.FromArgb(80, 80, 80),
		};
	}

	private void SafeInvoke(Action action)
	{
		if (IsDisposed) return;
		if (InvokeRequired) Invoke(action);
		else action();
	}
}

} // namespace ClawSpanUI.Panels
