using System;
using System.Drawing;
using System.Windows.Forms;
using ClawSpanUI.Models;
using ClawSpanUI.Panels;

namespace ClawSpanUI.Forms
{

// MainForm 是 ClawSpan 主窗口，包含 "状态" 和 "任务" 两个 Tab。
// 关闭时隐藏而非退出，通过系统托盘重新打开。
public class MainForm : Form
{
	private readonly TabControl _tabControl;
	private readonly TabPage _statusTab;
	private readonly TabPage _tasksTab;
	private readonly StatusPanel _statusPanel;
	private readonly TasksPanel _tasksPanel;
	private readonly Label _statusBarLabel;
	private readonly AppState _state;

	// ─────────────────────────────────────────────────────────
	// 构造
	// ─────────────────────────────────────────────────────────

	// MainForm 构建主窗口及两个 Tab 面板。
	//
	// 入参:
	// - state: 全局应用状态，传递给各 Panel。
	public MainForm(AppState state)
	{
		_tabControl = new TabControl();
		_statusTab = new TabPage();
		_tasksTab = new TabPage();
		_statusPanel = new StatusPanel(state);
		_tasksPanel = new TasksPanel(state);
		_statusBarLabel = new Label();
		_state = state;

		// Dpi 模式：运行时 DPI / 96 = 缩放系数，正确配对 (96F, 96F)
		AutoScaleDimensions = new System.Drawing.SizeF(96F, 96F);
		AutoScaleMode = AutoScaleMode.Dpi;

		BuildLayout();

		// 订阅连接状态更新状态栏
		state.OnConnectionChanged += () => SafeInvoke(UpdateStatusBar);
	}

	// ─────────────────────────────────────────────────────────
	// 内部：布局构建
	// ─────────────────────────────────────────────────────────

	// BuildLayout 初始化窗口属性并组装所有控件。
	private void BuildLayout()
	{
		SuspendLayout();

		Text = "ClawSpan";
		FormBorderStyle = FormBorderStyle.Sizable;
		MinimumSize = new Size(700, 580);
		Size = new Size(760, 640);
		StartPosition = FormStartPosition.CenterScreen;
		BackColor = Color.White;
		// 关闭时隐藏，不退出应用
		ShowInTaskbar = false;

		// ── TabControl ──
		_statusTab.Text = "  状态  ";
		_statusTab.BackColor = Color.White;
		_tasksTab.Text = "  任务  ";
		_tasksTab.BackColor = Color.White;

		_statusPanel.Dock = DockStyle.Fill;
		_tasksPanel.Dock = DockStyle.Fill;
		_statusTab.Controls.Add(_statusPanel);
		_tasksTab.Controls.Add(_tasksPanel);

		_tabControl.Controls.Add(_statusTab);
		_tabControl.Controls.Add(_tasksTab);
		_tabControl.Dock = DockStyle.Fill;
		_tabControl.Font = new Font("微软雅黑", 9.5f);
		Controls.Add(_tabControl);

		// ── 状态栏 ──
		// 用 TableLayoutPanel 做两列：左列状态文字（Fill），右列版本号（固定70px）。
		// 两列都用 Dock=Fill + TextAlign 垂直居中，彻底避免 Label 高度裁字问题。
		var statusBar = new TableLayoutPanel {
			Dock        = DockStyle.Bottom,
			Height      = 30,
			BackColor   = Color.FromArgb(240, 240, 240),
			ColumnCount = 2,
			RowCount    = 1,
			Padding     = new Padding(8, 0, 8, 0),
		};
		statusBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));
		statusBar.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 64f));
		statusBar.RowStyles.Add(new RowStyle(SizeType.Percent, 100f));

		// 状态文字：充满左列，文字垂直居中
		_statusBarLabel.Dock      = DockStyle.Fill;
		_statusBarLabel.Font      = new Font("微软雅黑", 8f);
		_statusBarLabel.ForeColor = Color.FromArgb(80, 80, 80);
		_statusBarLabel.Text      = "正在连接…";
		_statusBarLabel.TextAlign = ContentAlignment.MiddleLeft;
		_statusBarLabel.AutoSize  = false;
		statusBar.Controls.Add(_statusBarLabel, 0, 0);

		// 版本号：充满右列，文字垂直居中右对齐
		var versionLabel = new Label {
			Dock      = DockStyle.Fill,
			Font      = new Font("微软雅黑", 8f),
			ForeColor = Color.FromArgb(160, 160, 160),
			Text      = "v0.1.1",
			TextAlign = ContentAlignment.MiddleRight,
			AutoSize  = false,
		};
		statusBar.Controls.Add(versionLabel, 1, 0);

		Controls.Add(statusBar);

		ResumeLayout(false);
	}

	// ─────────────────────────────────────────────────────────
	// 窗口行为
	// ─────────────────────────────────────────────────────────

	// OnFormClosing 拦截关闭事件，改为隐藏窗口（托盘常驻）。
	protected override void OnFormClosing(FormClosingEventArgs e)
	{
		// 仅用户点击关闭时隐藏；应用主动关闭时允许退出
		if (e.CloseReason == CloseReason.UserClosing) {
			e.Cancel = true;
			Hide();
			ShowInTaskbar = false;
		} else {
			base.OnFormClosing(e);
		}
	}

	// BringToFront 显示并激活主窗口。
	public new void BringToFront()
	{
		ShowInTaskbar = true;
		Show();
		WindowState = FormWindowState.Normal;
		Activate();
		base.BringToFront();
	}

	// ─────────────────────────────────────────────────────────
	// 内部：状态栏刷新
	// ─────────────────────────────────────────────────────────

	// UpdateStatusBar 根据当前连接状态更新状态栏文本。
	private void UpdateStatusBar()
	{
		// 此方法可能从后台线程触发，SafeInvoke 已在订阅处负责调度
		var state = _state; // 从构造中捕获，避免 null 引用
		if (!state.ChannelConnected) {
			_statusBarLabel.Text      = "未连接到 Daemon";
			_statusBarLabel.ForeColor = Color.FromArgb(180, 60, 60);
		} else if (!state.DaemonRunning) {
			_statusBarLabel.Text      = "正在连接…";
			_statusBarLabel.ForeColor = Color.FromArgb(140, 100, 0);
		} else if (state.VmState == "starting") {
			_statusBarLabel.Text      = "Daemon 运行中 · VM 启动中";
			_statusBarLabel.ForeColor = Color.FromArgb(140, 100, 0);
		} else if (state.VmState != "running") {
			_statusBarLabel.Text      = "Daemon 运行中 · VM 已停止";
			_statusBarLabel.ForeColor = Color.FromArgb(180, 60, 60);
		} else if (state.OpenClawState == "online") {
			var channelText = state.ChannelState == "active" ? " · 工作中" : "";
			_statusBarLabel.Text      = $"● 就绪{channelText}";
			_statusBarLabel.ForeColor = Color.FromArgb(40, 120, 40);
		} else {
			_statusBarLabel.Text      = "VM 运行中 · OpenClaw 离线";
			_statusBarLabel.ForeColor = Color.FromArgb(80, 80, 80);
		}
	}

	// SafeInvoke 将 action 调度到 UI 线程执行。
	//
	// 入参:
	// - action: 要在 UI 线程执行的操作。
	private void SafeInvoke(Action action)
	{
		if (IsDisposed) {
			return;
		}
		if (InvokeRequired) {
			Invoke(action);
		} else {
			action();
		}
	}
}

} // namespace ClawSpanUI.Forms
