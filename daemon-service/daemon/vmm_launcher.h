#pragma once

// vmm_launcher.h -- vmm.exe 进程管理
//
// VmmLauncher 封装 vmm.exe 子进程的启动、监控和停止。
// 由 Daemon 在 init 阶段创建并在 stop 阶段销毁。
//
// 功能：
//   - 查找并启动 vmm.exe 子进程（CreateProcess）
//   - 后台线程监控进程退出，意外退出时自动重启
//   - 优雅停机：发送 Ctrl+Break → 等待退出 → 超时 TerminateProcess

#include "common/error.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace clawspan {
namespace daemon {

// VmmLauncherConfig -- vmm.exe 启动参数。
struct VmmLauncherConfig
{
	// vmm.exe 可执行文件路径（空字符串时自动查找 daemon 同目录下的 vmm.exe）。
	std::string exe_path;

	// WSL2 distro 名称，传给 vmm.exe --distro。
	std::string distro_name = "ClawSpan";

	// daemon VM 控制通道 Named Pipe 路径，传给 vmm.exe --daemon-pipe。
	std::string daemon_pipe = "\\\\.\\pipe\\crew-shell-service";

	// 日志级别，传给 vmm.exe --log-level。
	std::string log_level = "info";

	// vmm.exe 意外退出后重启前的等待时间（毫秒）。
	uint32_t restart_delay_ms = 3000;

	// vmm.exe 停机等待超时（毫秒），超时后 TerminateProcess。
	uint32_t stop_timeout_ms = 5000;

	// 最大连续快速重启次数（防止无限循环）。
	uint32_t max_rapid_restarts = 5;
};

// VmmLauncher 管理 vmm.exe 子进程的完整生命周期。
//
// 使用方式：
//   VmmLauncher launcher;
//   launcher.start(config);   // 启动 vmm.exe + 监控线程
//   ...
//   launcher.stop();          // 优雅停机
class VmmLauncher
{
public:
	VmmLauncher();
	~VmmLauncher();

	VmmLauncher(const VmmLauncher&)            = delete;
	VmmLauncher& operator=(const VmmLauncher&) = delete;

	// start 启动 vmm.exe 子进程并开启监控线程。
	//
	// 入参:
	// - config: vmm.exe 启动配置
	//
	// 出参/返回:
	// - Status::Ok():            成功启动
	// - Status(IO_ERROR):       vmm.exe 未找到或 CreateProcess 失败
	Status start(VmmLauncherConfig config);

	// stop 优雅停止 vmm.exe 子进程。
	//
	// 流程：
	//   1. 向 vmm.exe 发送 Ctrl+Break 信号
	//   2. 等待进程退出（stop_timeout_ms）
	//   3. 超时未退出则 TerminateProcess
	void stop();

	// isRunning 返回 vmm.exe 是否正在运行。
	bool isRunning() const;

private:
	struct Implement;
	std::unique_ptr<Implement> impl_;
};

} // namespace daemon
} // namespace clawspan
