using System;
using System.Threading;
using System.Windows.Forms;

namespace ClawSpanUI
{

// Program 是应用入口，配置 WinForms 运行环境并启动 App。
internal static class Program
{
	private static Mutex _singleInstanceMutex;

	// Main 应用主入口，必须声明 STAThread 以使用 COM 和 WinForms。
	[STAThread]
	private static void Main()
	{
		// UI 单实例保护：已有实例运行时直接退出，避免多个托盘进程并发连接 daemon。
		bool createdNew = false;
		_singleInstanceMutex = new Mutex(true, @"Local\ClawSpanUI", out createdNew);
		if (!createdNew) {
			_singleInstanceMutex.Dispose();
			return;
		}

		// PerMonitorV2：WinForms 自行按 DPI 缩放每个控件坐标和字体，
		// 避免 SystemAware 模式下 OS 做位图拉伸导致文字被裁掉一半。
		Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);

		// 启用视觉样式（圆角按钮等现代 Windows 外观）
		Application.EnableVisualStyles();
		Application.SetCompatibleTextRenderingDefault(false);

		// 以 ApplicationContext 模式运行，不显示主窗口，而是托盘常驻
		Application.Run(new App());

		_singleInstanceMutex.ReleaseMutex();
		_singleInstanceMutex.Dispose();
	}
}

} // namespace ClawSpanUI
