#pragma once

// vmm_app.h — vmm.exe 核心应用类
//
// VmmApp 负责 WSL2 distro 的生命周期管理：
//   - 通过 Channel 1 Named Pipe 连接 daemon，上报 distro 状态
//   - 接收 daemon 的管理命令（启动/停止/快照等）
//   - Watchdog 监控 distro 健康状态
//
// 注意：VM 数据通路（capability 调用）由 daemon 直接通过 VsockServer 处理，
// vmm.exe 不参与数据转发。

#include "common/error.h"

#include <memory>
#include <string>

namespace clawspan {
namespace vmm {

// VmmConfig — vmm.exe 运行时配置，由命令行参数填充。
struct VmmConfig
{
	// distro 名称（必选）
	std::string distro_name;

	// daemon Channel 1 Named Pipe 路径（用于管理通道）
	std::string daemon_pipe = "\\\\.\\pipe\\crew-shell-service";

	// 日志级别
	std::string log_level = "info";

	// 前台运行模式
	bool foreground = false;
};

// VmmApp — vmm.exe 主应用。
//
// 使用方式：
//   VmmApp app;
//   app.init(config);
//   app.run();  // 阻塞直到收到停止信号
class VmmApp
{
public:
	VmmApp();
	~VmmApp();

	VmmApp(const VmmApp&)            = delete;
	VmmApp& operator=(const VmmApp&) = delete;

	// init 初始化：连接 daemon 管理通道、注册自身。
	Status init(VmmConfig config);

	// run 阻塞主线程，运行 watchdog 循环直到收到停止信号。
	bool run();

	// stop 触发优雅停机。
	void stop();

private:
	struct Implement;
	std::unique_ptr<Implement> impl_;
};

} // namespace vmm
} // namespace clawspan
