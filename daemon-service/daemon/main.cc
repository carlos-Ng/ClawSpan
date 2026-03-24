#include "daemon.h"

#include "common/log.h"

#include <cxxopts.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#  include <windows.h>
#else
#  error "Platform not supported: add POSIX pthread_sigmask/sigemptyset implementation here"
#endif

// CLAWSPAN_VERSION 是 daemon 的版本字符串，由 CMake 注入或回退到默认值。
#ifndef CLAWSPAN_VERSION
#define CLAWSPAN_VERSION "0.1.1"
#endif

// blockTerminationSignals Windows 下无信号掩码概念，空实现占位。
//
// Windows 下终止信号通过 SetConsoleCtrlHandler 处理，注册在 Daemon::run() 内。
// 保留此函数使 main() 结构与 POSIX 版本保持对称，便于未来合并。
static void blockTerminationSignals()
{
#ifdef _WIN32
	// Windows 无进程级信号掩码，终止事件由 daemon.cc 中的 SetConsoleCtrlHandler 处理
#else
#  error "Platform not supported: add pthread_sigmask implementation here"
#endif
}

#ifdef _WIN32
static HANDLE g_single_instance_mutex = INVALID_HANDLE_VALUE;

static bool acquireSingleInstanceMutex()
{
	g_single_instance_mutex = ::CreateMutexA(
	    nullptr,
	    FALSE,
	    "Global\\ClawSpanDaemon");
	if (g_single_instance_mutex == nullptr || g_single_instance_mutex == INVALID_HANDLE_VALUE) {
		std::cerr << "error: failed to create single-instance mutex, GetLastError="
		          << ::GetLastError() << "\n";
		return false;
	}
	if (::GetLastError() == ERROR_ALREADY_EXISTS) {
		std::cerr << "error: another claw_span_service instance is already running\n";
		::CloseHandle(g_single_instance_mutex);
		g_single_instance_mutex = INVALID_HANDLE_VALUE;
		return false;
	}
	return true;
}

static void releaseSingleInstanceMutex()
{
	if (g_single_instance_mutex != INVALID_HANDLE_VALUE) {
		::CloseHandle(g_single_instance_mutex);
		g_single_instance_mutex = INVALID_HANDLE_VALUE;
	}
}
#endif

// parseArgs 解析命令行参数，将 CLI 覆盖值填入 DaemonConfig。
//
// 入参:
// - argc:   命令行参数个数。
// - argv:   命令行参数数组。
// - config: 输出参数，CLI 指定的字段将覆盖其默认值。
//
// 出参/返回:
// - true:  解析成功，调用方继续执行。
// - false: 遇到 --help 或 --version，已打印信息，调用方应退出（返回 0）。
static bool parseArgs(int argc, char** argv, clawspan::daemon::DaemonConfig& config)
{
	cxxopts::Options opts("claw_span_service", "ClawSpan daemon — secure capability host for AI agents");

	opts.add_options()
	    ("c,config",
	     "TOML 配置文件路径",
	     cxxopts::value<std::string>()->default_value(
	         clawspan::daemon::DaemonConfig::DEFAULT_CONFIG_PATH))
	    ("f,foreground",
	     "前台运行：日志输出到 stdout，不后台化",
	     cxxopts::value<bool>()->default_value("false"))
	    ("log-level",
	     "日志级别：trace / debug / info / warn / error / critical / off",
	     cxxopts::value<std::string>())
	    ("socket",
	     "覆盖 TOML 中的 IPC 管道路径",
	     cxxopts::value<std::string>())
	    ("module-dir",
	     "覆盖 TOML 中的模块 DLL 目录",
	     cxxopts::value<std::string>())
	    ("thread-pool-size",
	     "覆盖 TOML 中的 IPC 工作线程数",
	     cxxopts::value<int>())
	    ("version", "打印版本号后退出")
	    ("h,help",  "打印帮助信息后退出");

	cxxopts::ParseResult result = [&]() {
		try {
			return opts.parse(argc, argv);
		} catch (const cxxopts::OptionParseException& e) {
			std::cerr << "error: " << e.what() << "\n\n" << opts.help() << "\n";
			std::exit(EXIT_FAILURE);
		}
	}();

	if (result.count("help")) {
		std::cout << opts.help() << "\n";
		return false;
	}
	if (result.count("version")) {
		std::cout << "claw_span_service " << CLAWSPAN_VERSION << "\n";
		return false;
	}

	// 必选：配置文件路径（有默认值，总是存在）
	config.config_path = result["config"].as<std::string>();

	// 布尔标志
	config.foreground = result["foreground"].as<bool>();

	// 可选覆盖：CLI 指定则覆盖，未指定保留 DaemonConfig 的内置默认值
	if (result.count("log-level")) {
		config.log_level = result["log-level"].as<std::string>();
	}
	if (result.count("socket")) {
		config.socket_path = result["socket"].as<std::string>();
	}
	if (result.count("module-dir")) {
		config.module_dir = result["module-dir"].as<std::string>();
	}
	if (result.count("thread-pool-size")) {
		int pool_size = result["thread-pool-size"].as<int>();
		if (pool_size < 1) {
			std::cerr << "error: --thread-pool-size must be >= 1 (got " << pool_size << ")\n";
			std::exit(EXIT_FAILURE);
		}
		config.thread_pool_size = pool_size;
	}

	return true;
}

int main(int argc, char** argv)
{
	// 在创建任何线程之前调用占位函数，保持与 POSIX 版本对称的结构。
	// Windows 下无信号掩码，此处为空实现；SIGTERM/SIGINT 等效事件由
	// Daemon::run() 中的 SetConsoleCtrlHandler 处理。
	blockTerminationSignals();

	clawspan::daemon::DaemonConfig config;
	if (!parseArgs(argc, argv, config)) {
		return EXIT_SUCCESS;
	}

#ifdef _WIN32
	if (!acquireSingleInstanceMutex()) {
		return EXIT_FAILURE;
	}
#endif

	clawspan::daemon::Daemon daemon;
	auto status = daemon.init(config);
	if (!status.ok()) {
		LOG_ERROR("daemon init failed: {}", status.message);
#ifdef _WIN32
		releaseSingleInstanceMutex();
#endif
		return EXIT_FAILURE;
	}

	if (!daemon.run()) {
#ifdef _WIN32
		releaseSingleInstanceMutex();
#endif
		return EXIT_FAILURE;
	}

#ifdef _WIN32
	releaseSingleInstanceMutex();
#endif
	return EXIT_SUCCESS;
}
