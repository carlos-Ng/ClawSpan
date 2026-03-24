#pragma once

#include "core/base/core_config.h"
#include "common/error.h"

#include <memory>
#include <string>

namespace clawspan {
namespace daemon {

// DaemonConfig daemon 运行时的完整配置，由命令行参数与 TOML 配置合并而成。
//
// 合并优先级：命令行参数 > TOML 配置 > 程序内置默认值。
// 命令行参数通过非空字段（字符串）或 non-zero 值（整数）覆盖对应 TOML 字段。
//
// DEFAULT_* 常量同时作为 TOML 解析阶段判断"是否已被 CLI 覆盖"的基准值，
// 两处引用同一常量，避免不同步导致的覆盖逻辑错误。
struct DaemonConfig
{
	// ── 默认值常量（供结构体成员初始化与 parseToml 判断共用）────────────────────
	static constexpr const char* DEFAULT_CONFIG_PATH      = "clawspan.toml";
	static constexpr const char* DEFAULT_SOCKET_PATH      = "\\\\.\\pipe\\crew-shell-service";
	static constexpr const char* DEFAULT_LOG_LEVEL        = "info";
	static constexpr const char* DEFAULT_MODULE_DIR       = "";
	static constexpr int         DEFAULT_THREAD_POOL_SIZE = 8;

	// TOML 配置文件路径，由 CLI -c/--config 提供，或使用默认路径。
	std::string config_path = DEFAULT_CONFIG_PATH;

	// 前台运行模式：为 true 时日志输出到 stdout，不启动后台守护进程。
	bool foreground = false;

	// IPC 服务端监听的本地通道路径（Windows: Named Pipe 路径）。
	std::string socket_path = DEFAULT_SOCKET_PATH;

	// 工作线程池大小。
	int thread_pool_size = DEFAULT_THREAD_POOL_SIZE;

	// 日志级别（trace / debug / info / warn / error / critical / off）。
	std::string log_level = DEFAULT_LOG_LEVEL;

	// 模块 dll 所在目录，传入 CoreConfig 供 ModuleManager 使用。
	std::string module_dir = DEFAULT_MODULE_DIR;

	// ── UIService (Channel 2) 配置 ──────────────────────────────────────────
	static constexpr const char* DEFAULT_UI_PIPE_PATH    = "\\\\.\\pipe\\crew-shell-service-ui";
	static constexpr int         DEFAULT_UI_TIMEOUT_SECS = 60;
	static constexpr const char* DEFAULT_UI_TIMEOUT_MODE = "timeout_deny";

	// Channel 2 Named Pipe 路径。
	std::string ui_pipe_path    = DEFAULT_UI_PIPE_PATH;

	// 用户无响应时的超时时长（秒）；WAIT_FOREVER 时忽略。
	int         ui_timeout_secs = DEFAULT_UI_TIMEOUT_SECS;

	// 超时策略：wait_forever / timeout_deny / timeout_allow。
	std::string ui_timeout_mode = DEFAULT_UI_TIMEOUT_MODE;

	// ── VsockServer (Channel 3) 配置 ────────────────────────────────────────
	static constexpr uint32_t DEFAULT_VSOCK_PORT    = 100;
	static constexpr bool     DEFAULT_VSOCK_ENABLED = true;

	// AF_HYPERV vsock 监听端口（VM 侧 AF_VSOCK 连接端口）。
	uint32_t vsock_port    = DEFAULT_VSOCK_PORT;

	// 是否启用 VsockServer（false 时不监听 VM 连接，用于无 VM 的调试场景）。
	bool     vsock_enabled = DEFAULT_VSOCK_ENABLED;

	// ── VMM 配置 ────────────────────────────────────────────────────────────
	static constexpr const char* DEFAULT_VMM_DISTRO_NAME = "ClawSpan";
	static constexpr const char* DEFAULT_VMM_ROOTFS_PATH = "";
	static constexpr const char* DEFAULT_VMM_EXE_PATH    = "";
	static constexpr bool        DEFAULT_VMM_AUTO_START  = true;

	// WSL2 distro 名称。
	std::string vmm_distro_name = DEFAULT_VMM_DISTRO_NAME;

	// rootfs tarball 路径（用于首次自动导入，空字符串表示不自动导入）。
	std::string vmm_rootfs_path = DEFAULT_VMM_ROOTFS_PATH;

	// vmm.exe 可执行文件路径（空字符串时从 daemon 同目录查找）。
	std::string vmm_exe_path = DEFAULT_VMM_EXE_PATH;

	// 是否随 daemon 自动启动 vmm.exe（false 时不启动 vmm.exe）。
	bool vmm_auto_start = DEFAULT_VMM_AUTO_START;

	// core 层配置（模块列表），由 TOML [[modules]] 填充。
	core::CoreConfig core;
};

// Daemon 是后台守护进程的核心类，负责：
// - 解析并合并 TOML 配置与命令行参数
// - 初始化日志系统
// - 初始化 CapabilityService（动态加载所有模块）
// - 启动 IPC 服务端并注册所有 capability handler
// - 运行主循环并响应 SIGTERM / SIGINT 信号实现优雅停机
class Daemon
{
public:
	Daemon();
	~Daemon();

	Daemon(const Daemon&)            = delete;
	Daemon& operator=(const Daemon&) = delete;

	// init 完成 daemon 初始化：解析 TOML、合并 CLI 覆盖、初始化日志与 core 层。
	//
	// 入参:
	// - config: 已由 main.cc 预填命令行覆盖字段的 DaemonConfig；
	//           init 内部解析 TOML 后填充未覆盖的字段。
	//
	// 出参/返回:
	// - Status::Ok()：初始化成功。
	// - Status(error)：配置解析或模块加载失败。
	Status init(DaemonConfig config);

	// run 启动 IPC 服务并阻塞，直到收到 SIGTERM 或 SIGINT 后优雅停机。
	//
	// 须在 init 成功后调用。
	//
	// 出参/返回:
	// - true：正常退出（收到终止信号并完成 stop）。
	// - false：IPC 启动失败或其它原因提前返回，调用方应以非零退出码退出。
	bool run();

	// stop 触发优雅停机，通知 run() 退出主循环。
	//
	// 可在信号处理器或其它线程中安全调用（通过 atomic 标志实现）。
	void stop();

private:
	struct Implement;
	std::unique_ptr<Implement> implement_;
};

} // namespace daemon
} // namespace clawspan
