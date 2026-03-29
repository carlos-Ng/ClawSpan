using System;
using System.Drawing;
using System.Text.Json;
using System.Windows.Forms;
using ClawSpanUI.Channel;
using ClawSpanUI.Forms;
using ClawSpanUI.Models;

namespace ClawSpanUI
{

// App 继承 ApplicationContext，作为托盘应用的主控制器。
// 负责：托盘图标生命周期、UI 事件通道启动、消息分发到 UI、确认弹窗显示。
public class App : ApplicationContext
{
	private readonly NotifyIcon _trayIcon;
	private readonly MainForm _mainForm;
	private readonly AppState _state;
	private readonly DaemonChannel _channel;
	private readonly MessageDispatcher _dispatcher;

	// 托盘图标：正常（绿）/ 待确认（黄）/ 离线（灰）
	private readonly Icon _iconNormal;
	private readonly Icon _iconAlert;
	private readonly Icon _iconOffline;

	// ─────────────────────────────────────────────────────────
	// 构造
	// ─────────────────────────────────────────────────────────

	// App 初始化所有组件并启动后台通信通道。
	public App()
	{
		_state = new AppState();
		_channel = new DaemonChannel();
		_dispatcher = new MessageDispatcher();
		_mainForm = new MainForm(_state);

		_iconNormal = CreateCircleIcon(Color.FromArgb(40, 167, 69));
		_iconAlert = CreateCircleIcon(Color.FromArgb(255, 193, 7));
		_iconOffline = CreateCircleIcon(Color.FromArgb(150, 150, 150));

		_trayIcon = BuildTrayIcon();

		WireChannelEvents();
		WireDispatcherEvents();

		// 强制创建 MainForm 窗口句柄，确保后台线程触发的 Invoke / SafeInvoke 能正常调度到 UI 线程。
		// 若句柄未创建，_mainForm.Invoke() 会抛 InvalidOperationException，
		// 该异常会被 DaemonChannel.RunConnectLoop 的 catch(Exception) 吞掉，导致连接被误判为失败。
		_ = _mainForm.Handle;

		_channel.Start();
	}

	// ─────────────────────────────────────────────────────────
	// 内部：事件绑定
	// ─────────────────────────────────────────────────────────

	// WireChannelEvents 将 DaemonChannel 的事件绑定到本地处理逻辑。
	private void WireChannelEvents()
	{
		_channel.OnConnectionChanged += connected => {
			_state.UpdateChannelConnected(connected);
			UpdateTrayIcon();
		};

		_channel.OnFrameReceived += json => {
			_dispatcher.Dispatch(json);
		};
	}

	// WireDispatcherEvents 将 MessageDispatcher 的各类型事件绑定到状态更新逻辑。
	private void WireDispatcherEvents()
	{
		_dispatcher.OnStatus += msg => {
			_state.UpdateFromStatus(msg.Vm, msg.OpenClaw, msg.Channel);
			UpdateTrayIcon();
		};

		_dispatcher.OnTaskBegin += msg => {
			_state.BeginTask(msg.TaskId, msg.RootDescription);
		};

		_dispatcher.OnTaskEnd += msg => {
			_state.EndTask(msg.TaskId);
		};

		_dispatcher.OnOpLog += msg => {
			var record = new OperationRecord {
				Operation = msg.Operation,
				Result = msg.Result,
				Source = msg.Source,
				Detail = msg.Detail,
				Time = DateTimeOffset.FromUnixTimeSeconds(msg.Timestamp).LocalDateTime,
			};
			_state.AppendOperation(record);

			// 若用户在弹窗中勾选了信任，op_log 的 source 会是 fingerprint_cache
			// 对应 fingerprint 由 daemon 在 confirm 响应后自行缓存，此处同步到 UI 状态
			if (msg.Source == "fingerprint_cache" && !string.IsNullOrEmpty(msg.Detail)) {
				_state.AddCachedFingerprint(msg.Detail);
			}
		};

		// confirm 消息必须在 UI 线程上处理（显示模态弹窗）
		_dispatcher.OnConfirm += msg => {
			_mainForm.Invoke(() => HandleConfirmRequest(msg));
		};
	}

	// ─────────────────────────────────────────────────────────
	// 内部：确认弹窗处理
	// ─────────────────────────────────────────────────────────

	// HandleConfirmRequest 在 UI 线程上显示确认弹窗，等待用户响应后发送结果。
	// 此方法是阻断式的：调用方（Channel 线程通过 Invoke 调用）在用户响应前挂起。
	//
	// 入参:
	// - message: 来自 daemon 的 confirm 消息。
	private void HandleConfirmRequest(ConfirmMessage message)
	{
		// 待确认时切换为警告图标并闪烁任务栏
		_trayIcon.Icon = _iconAlert;
		_trayIcon.Text = "ClawSpan — 需要操作确认";

		var dlg = new ConfirmDialog(message);
		dlg.ShowDialog();

		// 还原为正常图标
		UpdateTrayIcon();

		// 向 daemon 发送确认结果（异步写，不阻塞 UI）
		var response = new ConfirmResponse {
			ConfirmId = message.ConfirmId,
			Confirmed = dlg.Confirmed,
			TrustFingerprint = dlg.TrustFingerprint,
		};
		var json = JsonSerializer.Serialize(response);
		_ = _channel.SendAsync(json);
	}

	// ─────────────────────────────────────────────────────────
	// 内部：托盘图标
	// ─────────────────────────────────────────────────────────

	// BuildTrayIcon 创建并配置系统托盘图标及其右键菜单。
	//
	// 返回: 配置完成的 NotifyIcon 实例。
	private NotifyIcon BuildTrayIcon()
	{
		var menu = new ContextMenuStrip();
		menu.Items.Add("打开主界面", null, (_, _) => OpenMainWindow());
		menu.Items.Add(new ToolStripSeparator());

		var statusItem = new ToolStripMenuItem("Daemon 未连接") {
			Enabled = false,
			ForeColor = Color.FromArgb(120, 120, 120),
		};
		menu.Items.Add(statusItem);
		menu.Items.Add(new ToolStripSeparator());
		menu.Items.Add("退出", null, (_, _) => ExitApp());

		var icon = new NotifyIcon {
			Icon = _iconOffline,
			Text = "ClawSpan",
			Visible = true,
			ContextMenuStrip = menu,
		};

		// 单击或双击托盘图标均打开主界面
		icon.Click += (_, _) => OpenMainWindow();

		// 保存状态项引用以便后续更新文本
		_state.OnConnectionChanged += () => {
			_mainForm.Invoke(() => {
				statusItem.Text = _state.DaemonRunning ? "● Daemon 运行中" : "○ Daemon 未连接";
			});
		};

		return icon;
	}

	// UpdateTrayIcon 根据当前状态更新托盘图标和提示文本。
	private void UpdateTrayIcon()
	{
		_mainForm.Invoke(() => {
			if (!_state.ChannelConnected) {
				_trayIcon.Icon = _iconOffline;
				_trayIcon.Text = "ClawSpan — 未连接";
			} else if (_state.VmState == "running" && _state.OpenClawState == "online") {
				_trayIcon.Icon = _iconNormal;
				_trayIcon.Text = _state.ChannelState == "active"
					? "ClawSpan — 工作中"
					: "ClawSpan — 就绪";
			} else if (_state.VmState == "starting") {
				_trayIcon.Icon = _iconNormal;
				_trayIcon.Text = "ClawSpan — VM 启动中";
			} else {
				_trayIcon.Icon = _iconOffline;
				_trayIcon.Text = _state.VmState == "stopped"
					? "ClawSpan — VM 已停止"
					: "ClawSpan — OpenClaw 离线";
			}
		});
	}

	// OpenMainWindow 将主窗口显示并置于前台。
	private void OpenMainWindow()
	{
		_mainForm.BringToFront();
	}

	// ExitApp 清理资源并退出应用。
	private void ExitApp()
	{
		_trayIcon.Visible = false;
		_channel.Stop();
		_channel.Dispose();
		_iconNormal.Dispose();
		_iconAlert.Dispose();
		_iconOffline.Dispose();

		Application.Exit();
	}

	// ─────────────────────────────────────────────────────────
	// 内部：图标生成
	// ─────────────────────────────────────────────────────────

	// CreateCircleIcon 以指定颜色绘制 16×16 圆形图标。
	// 运行时动态生成，无需外部 .ico 文件。
	//
	// 入参:
	// - color: 圆形填充颜色。
	//
	// 返回: 生成的 Icon 实例（调用方负责 Dispose）。
	private static Icon CreateCircleIcon(Color color)
	{
		var bitmap = new Bitmap(16, 16);
		using (var g = Graphics.FromImage(bitmap)) {
			g.Clear(Color.Transparent);
			g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
			using (var brush = new SolidBrush(color)) {
				g.FillEllipse(brush, 1, 1, 13, 13);
			}
		}
		var handle = bitmap.GetHicon();
		bitmap.Dispose();
		return Icon.FromHandle(handle);
	}
}

} // namespace ClawSpanUI
