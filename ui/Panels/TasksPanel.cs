using System;
using System.Drawing;
using System.Windows.Forms;
using ClawSpanUI.Models;

namespace ClawSpanUI.Panels
{

// TasksPanel 展示当前任务的操作记录、已缓存的 Intent Fingerprint
// 以及历史任务列表，对应主窗口 "任务" Tab。
public class TasksPanel : UserControl
{
	// ─────────────────────────────────────────────────────────
	// 控件
	// ─────────────────────────────────────────────────────────
	private readonly Label    _taskHeaderLabel;
	private readonly ListView _operationList;
	private readonly ListBox  _fingerprintList;
	private readonly ListView _historyList;

	private readonly AppState _state;

	private static readonly Color COLOR_ALLOWED = Color.FromArgb(40, 167, 69);
	private static readonly Color COLOR_DENIED  = Color.FromArgb(220, 53, 69);

	// ─────────────────────────────────────────────────────────
	// 构造
	// ─────────────────────────────────────────────────────────

	public TasksPanel(AppState state)
	{
		_state           = state;
		_taskHeaderLabel = new Label();
		_operationList   = new ListView();
		_fingerprintList = new ListBox();
		_historyList     = new ListView();

		// Dpi 模式：运行时 DPI / 96 = 缩放系数，正确配对 (96F, 96F)
		AutoScaleDimensions = new SizeF(96F, 96F);
		AutoScaleMode = AutoScaleMode.Dpi;

		BuildLayout();
		RefreshAll();

		_state.OnTaskBegin       += _      => SafeInvoke(RefreshAll);
		_state.OnTaskEnd         += _      => SafeInvoke(RefreshOnTaskEnd);
		_state.OnOperationLogged += record => SafeInvoke(() => AppendOperation(record));
	}

	// ─────────────────────────────────────────────────────────
	// 内部：布局构建
	// ─────────────────────────────────────────────────────────

	private void BuildLayout()
	{
		SuspendLayout();
		BackColor = Color.White;

		const int MARGIN = 16;

		// ── 当前任务标题（AutoSize，高度由字体决定）──
		_taskHeaderLabel.AutoSize  = true;
		_taskHeaderLabel.Location  = new Point(MARGIN, 14);
		_taskHeaderLabel.Font      = new Font("微软雅黑", 9.5f, FontStyle.Bold);
		_taskHeaderLabel.ForeColor = Color.FromArgb(40, 40, 40);
		_taskHeaderLabel.Anchor    = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
		Controls.Add(_taskHeaderLabel);

		// ── 操作记录（section y=44，list y=68）──
		Controls.Add(MakeSectionHeader("操作记录", MARGIN, 44));

		_operationList.Location    = new Point(MARGIN, 68);
		_operationList.Size        = new Size(528, 148);
		_operationList.View        = View.Details;
		_operationList.FullRowSelect  = true;
		_operationList.GridLines      = true;
		_operationList.HeaderStyle    = ColumnHeaderStyle.Nonclickable;
		_operationList.Font           = new Font("微软雅黑", 8.5f);
		_operationList.Anchor         = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
		_operationList.Columns.Add("时间", 70);
		_operationList.Columns.Add("结果", 90);
		_operationList.Columns.Add("操作", 90);
		_operationList.Columns.Add("详情", -2);
		Controls.Add(_operationList);

		// ── 缓存指纹（section y=228，list y=252）──
		Controls.Add(MakeSectionHeader("已缓存指纹（本任务自动放行）", MARGIN, 228));

		_fingerprintList.Location      = new Point(MARGIN, 252);
		_fingerprintList.Size          = new Size(528, 68);
		_fingerprintList.Font          = new Font("微软雅黑", 8.5f);
		_fingerprintList.BorderStyle   = BorderStyle.FixedSingle;
		_fingerprintList.SelectionMode = SelectionMode.None;
		_fingerprintList.Anchor        = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
		Controls.Add(_fingerprintList);

		// ── 历史任务（section y=332，list y=356）──
		Controls.Add(MakeSectionHeader("历史任务", MARGIN, 332));

		_historyList.Location   = new Point(MARGIN, 356);
		_historyList.Size       = new Size(528, 100);
		_historyList.View       = View.Details;
		_historyList.FullRowSelect  = true;
		_historyList.HeaderStyle    = ColumnHeaderStyle.Nonclickable;
		_historyList.Font           = new Font("微软雅黑", 8.5f);
		_historyList.Anchor         = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
		// 列顺序：任务ID | 开始时间 | 操作数 | 描述（描述放最后自动填充剩余宽度）
		_historyList.Columns.Add("任务", 72);
		_historyList.Columns.Add("时间", 72);
		_historyList.Columns.Add("次数", 72);
		_historyList.Columns.Add("描述", -2);
		Controls.Add(_historyList);

		// Resize 时同步更新 ListView 宽度与末列宽度
		Resize += (_, _) => OnPanelResize();
		// VisibleChanged：Tab 首次切换到此面板时触发，确保初始列宽正确设置
		VisibleChanged += (_, _) => { if (Visible) OnPanelResize(); };

		ResumeLayout(false);
	}

	// OnPanelResize 在面板尺寸变化时调整控件宽度与 ListView 末列宽度。
	private void OnPanelResize()
	{
		const int MARGIN = 16;
		int w = Width - MARGIN * 2;
		if (w <= 0) return;

		_operationList.Width   = w;
		_fingerprintList.Width = w;
		_historyList.Width     = w;

		int sv = SystemInformation.VerticalScrollBarWidth + 4;

		int detailW = w - 70 - 90 - 90 - sv;
		if (detailW > 0 && _operationList.Columns.Count == 4)
			_operationList.Columns[3].Width = detailW;

		// 历史任务列顺序：任务(52) | 开始(72) | 操作数(62) | 描述(剩余)
		int descW = w - 52 - 72 - 62 - sv;
		if (descW > 0 && _historyList.Columns.Count == 4)
			_historyList.Columns[3].Width = descW;
	}

	// ─────────────────────────────────────────────────────────
	// 内部：数据刷新
	// ─────────────────────────────────────────────────────────

	private void RefreshAll()
	{
		RefreshCurrentTask();
		RefreshHistory();
	}

	private void RefreshCurrentTask()
	{
		var task = _state.CurrentTask;

		if (task == null) {
			_taskHeaderLabel.Text      = "[ 无活动任务 ]";
			_taskHeaderLabel.ForeColor = Color.FromArgb(160, 160, 160);
			_operationList.Items.Clear();
			_fingerprintList.Items.Clear();
			return;
		}

		_taskHeaderLabel.Text      = $"任务 #{task.TaskId}  ·  进行中    {task.RootDescription}";
		_taskHeaderLabel.ForeColor = Color.FromArgb(40, 40, 40);

		_operationList.Items.Clear();
		foreach (var op in task.Operations)
			_operationList.Items.Add(BuildOperationItem(op));
		ScrollOperationListToEnd();

		_fingerprintList.Items.Clear();
		foreach (var fp in task.CachedFingerprints)
			_fingerprintList.Items.Add("  · " + fp);
	}

	private void AppendOperation(OperationRecord record)
	{
		var task = _state.CurrentTask;
		if (task == null) return;

		_taskHeaderLabel.Text = $"任务 #{task.TaskId}  ·  进行中    {task.RootDescription}";
		_operationList.Items.Add(BuildOperationItem(record));
		ScrollOperationListToEnd();

		_fingerprintList.Items.Clear();
		foreach (var fp in task.CachedFingerprints)
			_fingerprintList.Items.Add("  · " + fp);
	}

	private void RefreshOnTaskEnd()
	{
		_taskHeaderLabel.Text      = "[ 无活动任务 ]";
		_taskHeaderLabel.ForeColor = Color.FromArgb(160, 160, 160);
		RefreshHistory();
	}

	private void RefreshHistory()
	{
		_historyList.Items.Clear();
		foreach (var task in _state.TaskHistory) {
			// 列顺序：任务ID | 开始时间 | 操作数 | 描述
			var item = new ListViewItem($"#{task.TaskId}");
			item.SubItems.Add(task.StartTime.ToString("HH:mm"));
			item.SubItems.Add($"{task.TotalCount} 次");
			item.SubItems.Add(task.RootDescription);
			_historyList.Items.Add(item);
		}
	}

	// ─────────────────────────────────────────────────────────
	// 内部：工具方法
	// ─────────────────────────────────────────────────────────

	private static ListViewItem BuildOperationItem(OperationRecord record)
	{
		var resultText = TranslateResult(record.Result, record.Source);
		var item = new ListViewItem(record.Time.ToString("HH:mm:ss"));
		item.SubItems.Add(resultText);
		item.SubItems.Add(record.Operation);
		item.SubItems.Add(record.Detail);
		item.ForeColor = record.IsDenied ? COLOR_DENIED : COLOR_ALLOWED;
		return item;
	}

	private static string TranslateResult(string result, string source)
	{
		return (result, source) switch {
			("denied",    _)                    => "✗ 已拦截",
			("allowed",   "fingerprint_cache")  => "✓ 缓存放行",
			("allowed",   "user_confirm")       => "✓ 用户允许",
			("allowed",   _)                    => "✓ 自动放行",
			("confirmed", _)                    => "✓ 用户允许",
			_                                   => result,
		};
	}

	private void ScrollOperationListToEnd()
	{
		if (_operationList.Items.Count > 0)
			_operationList.EnsureVisible(_operationList.Items.Count - 1);
	}

	// MakeSectionHeader 创建 AutoSize 区域标题 Label。
	private static Label MakeSectionHeader(string text, int x, int y)
	{
		return new Label {
			Text      = text,
			Location  = new Point(x, y),
			AutoSize  = true,
			Font      = new Font("微软雅黑", 8.5f, FontStyle.Bold),
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
