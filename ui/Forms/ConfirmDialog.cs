using System;
using System.Drawing;
using System.Text;
using System.Text.Json;
using System.Windows.Forms;
using ClawSpanUI.Channel;

namespace ClawSpanUI.Forms
{

// ConfirmDialog 是操作确认弹窗，以模态阻断方式显示。
// 用户必须点击「允许」或「拒绝」才能关闭，Agent 操作在此期间被挂起。
public class ConfirmDialog : Form
{
	// ─────────────────────────────────────────────────────────
	// 控件
	// ─────────────────────────────────────────────────────────
	private readonly Label _taskLabel;
	private readonly Label _taskDescLabel;
	// 以下 4 个 Label 由 MakeDetailRow 在 BuildLayout 内部赋值，不声明 readonly
	private Label _operationLabel;
	private Label _appLabel;
	private Label _targetLabel;
	private Label _reasonLabel;
	private readonly CheckBox _trustCheckBox;
	private readonly Button _denyButton;
	private readonly Button _allowButton;

	// ─────────────────────────────────────────────────────────
	// 公开结果属性
	// ─────────────────────────────────────────────────────────

	// 用户是否允许本次操作
	public bool Confirmed { get; private set; }

	// 用户是否勾选"本任务内相同操作不再询问"
	public bool TrustFingerprint { get; private set; }

	// ─────────────────────────────────────────────────────────
	// 构造
	// ─────────────────────────────────────────────────────────

	// ConfirmDialog 根据 confirm 消息构建并初始化各控件。
	//
	// 入参:
	// - message: daemon 推送的 confirm 消息，包含任务上下文和操作信息。
	public ConfirmDialog(ConfirmMessage message)
	{
		// 初始化控件实例
		_taskLabel = new Label();
		_taskDescLabel = new Label();
		_operationLabel = new Label();
		_appLabel = new Label();
		_targetLabel = new Label();
		_reasonLabel = new Label();
		_trustCheckBox = new CheckBox();
		_denyButton = new Button();
		_allowButton = new Button();

		// Dpi 模式：运行时 DPI / 96 = 缩放系数，正确配对 (96F, 96F)
		AutoScaleDimensions = new System.Drawing.SizeF(96F, 96F);
		AutoScaleMode = AutoScaleMode.Dpi;

		BuildLayout();
		PopulateContent(message);
		BindEvents();
	}

	// ─────────────────────────────────────────────────────────
	// 内部：布局构建
	// ─────────────────────────────────────────────────────────

	// BuildLayout 创建并定位所有控件。
	private void BuildLayout()
	{
		SuspendLayout();

		Text = "操作确认";
		FormBorderStyle = FormBorderStyle.FixedDialog;
		MaximizeBox = false;
		MinimizeBox = false;
		ShowInTaskbar = true;
		TopMost = true;
		StartPosition = FormStartPosition.CenterScreen;
		ClientSize = new Size(420, 336); // 原 310，加高 26px 避免按钮被裁
		BackColor = Color.White;

		// ── 当前任务区域 ──
		var taskSectionLabel = MakeSectionLabel("当前任务", 12);
		taskSectionLabel.Location = new Point(16, 14);
		Controls.Add(taskSectionLabel);

		// 任务描述文本框（只读，多行）
		_taskDescLabel.Location = new Point(16, 40); // section 高 22 + 4px 间距
		_taskDescLabel.Size = new Size(388, 44);
		_taskDescLabel.BorderStyle = BorderStyle.FixedSingle;
		_taskDescLabel.BackColor = Color.FromArgb(245, 247, 250);
		_taskDescLabel.Padding = new Padding(6, 6, 6, 6);
		_taskDescLabel.Font = new Font("微软雅黑", 9f);
		_taskDescLabel.AutoEllipsis = true;
		Controls.Add(_taskDescLabel);

		// ── 操作详情区域 ──
		var opSectionLabel = MakeSectionLabel("Agent 想要执行以下操作：", 96); // 40+44+12
		Controls.Add(opSectionLabel);

		// 操作、应用、目标、原因行
		int rowY = 122; // 96+22+4
		const int ROW_HEIGHT = 26; // 微软雅黑 8.5pt 行高约 20px，行间距 6px
		const int LABEL_WIDTH = 48;
		const int VALUE_X = 72;
		const int VALUE_WIDTH = 332;

		_operationLabel = MakeDetailRow("操作", rowY, LABEL_WIDTH, VALUE_X, VALUE_WIDTH);
		rowY += ROW_HEIGHT;
		_appLabel = MakeDetailRow("应用", rowY, LABEL_WIDTH, VALUE_X, VALUE_WIDTH);
		rowY += ROW_HEIGHT;
		_targetLabel = MakeDetailRow("目标", rowY, LABEL_WIDTH, VALUE_X, VALUE_WIDTH);
		rowY += ROW_HEIGHT;
		_reasonLabel = MakeDetailRow("原因", rowY, LABEL_WIDTH, VALUE_X, VALUE_WIDTH);

		// rowY 最终约 122 + 26*4 = 226

		// ── 信任选项 ──
		_trustCheckBox.Location = new Point(16, 234); // 226 + 8px 间距
		_trustCheckBox.Size = new Size(388, 24); // 高度容纳微软雅黑 9pt
		_trustCheckBox.Text = "本任务内相同操作不再询问";
		_trustCheckBox.Font = new Font("微软雅黑", 9f);
		_trustCheckBox.FlatStyle = FlatStyle.Flat;
		Controls.Add(_trustCheckBox);

		// ── 分割线 ──
		var separator = new Panel {
			Location = new Point(0, 266), // 234+24+8
			Size = new Size(420, 1),
			BackColor = Color.FromArgb(220, 220, 220),
		};
		Controls.Add(separator);

		// ── 按钮行 ──
		_denyButton.Location = new Point(16, 280);
		_denyButton.Size = new Size(100, 36);
		_denyButton.Text = "拒绝";
		_denyButton.Font = new Font("微软雅黑", 9.5f);
		_denyButton.FlatStyle = FlatStyle.System;
		_denyButton.DialogResult = DialogResult.Cancel;
		Controls.Add(_denyButton);

		_allowButton.Location = new Point(288, 280);
		_allowButton.Size = new Size(116, 36);
		_allowButton.Text = "允许  (Enter)";
		_allowButton.Font = new Font("微软雅黑", 9.5f, FontStyle.Bold);
		_allowButton.FlatStyle = FlatStyle.System;
		_allowButton.DialogResult = DialogResult.OK;
		Controls.Add(_allowButton);

		// 键盘快捷键
		AcceptButton = _allowButton;
		CancelButton = _denyButton;

		ResumeLayout(false);
	}

	// PopulateContent 将 confirm 消息的内容填充到各控件。
	//
	// 入参:
	// - message: 来自 daemon 的 confirm 消息。
	private void PopulateContent(ConfirmMessage message)
	{
		// 任务描述
		_taskDescLabel.Text = string.IsNullOrEmpty(message.RootDescription)
			? "（无活动任务）"
			: message.RootDescription;

		// 操作名称，映射为可读中文
		_operationLabel.Text = TranslateOperation(message.Operation);

		// 应用名称，从 params 提取
		_appLabel.Text = ExtractAppName(message.Params);

		// 目标元素，从 params 提取
		_targetLabel.Text = ExtractTarget(message.Params);

		// 原因
		_reasonLabel.Text = string.IsNullOrEmpty(message.Reason) ? "—" : message.Reason;

		// 若无 fingerprint，禁用信任选项
		_trustCheckBox.Enabled = !string.IsNullOrEmpty(message.Fingerprint);
	}

	// BindEvents 绑定按钮点击事件，记录用户决策。
	private void BindEvents()
	{
		_allowButton.Click += (_, _) => {
			Confirmed = true;
			TrustFingerprint = _trustCheckBox.Checked;
		};

		_denyButton.Click += (_, _) => {
			Confirmed = false;
			TrustFingerprint = false;
		};

		// 点击关闭按钮等同于拒绝
		FormClosing += (_, e) => {
			if (DialogResult == DialogResult.None) {
				Confirmed = false;
				TrustFingerprint = false;
				DialogResult = DialogResult.Cancel;
			}
		};
	}

	// ─────────────────────────────────────────────────────────
	// 内部：控件工厂
	// ─────────────────────────────────────────────────────────

	// MakeSectionLabel 创建区域标题 Label。
	//
	// 入参:
	// - text: 标题文本。
	// - y:    垂直坐标。
	private Label MakeSectionLabel(string text, int y)
	{
		return new Label {
			Text = text,
			Location = new Point(16, y),
			Size = new Size(388, 22), // 微软雅黑 8.5pt Bold 约需 20px
			Font = new Font("微软雅黑", 8.5f, FontStyle.Bold),
			ForeColor = Color.FromArgb(80, 80, 80),
			AutoSize = false,
		};
	}

	// MakeDetailRow 创建一行操作详情（键 + 值 Label），添加到 Controls 并返回值 Label。
	//
	// 入参:
	// - key:        左侧字段名（如"操作"）。
	// - y:          行垂直坐标。
	// - labelWidth: 键 Label 宽度。
	// - valueX:     值 Label 水平起始位置。
	// - valueWidth: 值 Label 宽度。
	//
	// 返回: 值 Label，调用方可后续设置 Text。
	private Label MakeDetailRow(
		string key, int y, int labelWidth, int valueX, int valueWidth)
	{
		var keyLabel = new Label {
			Text = key,
			Location = new Point(16, y + 2),
			Size = new Size(labelWidth, 20), // 微软雅黑 8.5pt 约需 20px
			Font = new Font("微软雅黑", 8.5f),
			ForeColor = Color.FromArgb(120, 120, 120),
			AutoSize = false,
		};
		Controls.Add(keyLabel);

		var valueLabel = new Label {
			Text = string.Empty,
			Location = new Point(valueX, y + 2),
			Size = new Size(valueWidth, 20),
			Font = new Font("微软雅黑", 8.5f),
			ForeColor = Color.FromArgb(30, 30, 30),
			AutoSize = false,
			AutoEllipsis = true,
		};
		Controls.Add(valueLabel);

		return valueLabel;
	}

	// ─────────────────────────────────────────────────────────
	// 内部：内容解析
	// ─────────────────────────────────────────────────────────

	// TranslateOperation 将操作名称映射为可读中文描述。
	//
	// 入参:
	// - operation: capability 操作名，如 click / set_value。
	//
	// 返回: 中文描述字符串。
	private static string TranslateOperation(string operation)
	{
		return operation switch {
			"click" => "点击按钮 / 元素",
			"double_click" => "双击元素",
			"right_click" => "右键点击",
			"set_value" => "输入文本",
			"key_press" => "按下按键",
			"key_combo" => "键盘快捷键",
			"focus" => "聚焦元素",
			"scroll" => "滚动页面",
			"activate_window" => "切换窗口",
			_ => operation,
		};
	}

	// ExtractAppName 从 params JsonElement 中提取应用名称。
	// 尝试解析 element_path 的第一段（窗口 ID 后的应用名）。
	//
	// 入参:
	// - params: confirm 消息中的 params 字段。
	//
	// 返回: 应用名称字符串，无法解析时返回 "—"。
	private static string ExtractAppName(JsonElement @params)
	{
		if (@params.ValueKind == JsonValueKind.Object
			&& @params.TryGetProperty("element_path", out var pathProp)) {
			var path = pathProp.GetString() ?? string.Empty;
			// element_path 格式：/w1_WindowId/AppName/...
			var parts = path.TrimStart('/').Split('/');
			if (parts.Length >= 2) {
				return parts[1];
			}
		}
		return "—";
	}

	// ExtractTarget 从 params 中提取操作目标的可读描述。
	// 优先提取 element_path 末段的元素名；inputText 时展示前 30 字符。
	//
	// 入参:
	// - params: confirm 消息中的 params 字段。
	//
	// 返回: 目标描述字符串，无法解析时返回 "—"。
	private static string ExtractTarget(JsonElement @params)
	{
		if (@params.ValueKind != JsonValueKind.Object) {
			return "—";
		}

		// 优先展示 element_path 的末段
		if (@params.TryGetProperty("element_path", out var pathProp)) {
			var path = pathProp.GetString() ?? string.Empty;
			var lastSlash = path.LastIndexOf('/');
			if (lastSlash >= 0 && lastSlash < path.Length - 1) {
				return path[(lastSlash + 1)..];
			}
		}

		// 输入文本操作展示 value 前缀
		if (@params.TryGetProperty("value", out var valueProp)) {
			var value = valueProp.GetString() ?? string.Empty;
			return value.Length > 30 ? value[..30] + "…" : value;
		}

		// 键盘操作展示 keys
		if (@params.TryGetProperty("keys", out var keysProp)) {
			return keysProp.GetString() ?? "—";
		}

		return "—";
	}
}

} // namespace ClawSpanUI.Forms
