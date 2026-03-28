// main.cc — claw_span_vmm.exe 入口点
//
// 每个 WSL2 distro 对应一个 claw_span_vmm.exe 实例，负责 VM 生命周期管理：
//   - 通过 VM 控制通道 Named Pipe 连接 daemon，上报 distro 状态
//   - 接收 daemon 的管理命令（启动/停止/快照等）
//   - Watchdog 监控 distro 健康状态
//
// 注意：VM 数据通路（capability 调用）由 daemon 的 VsockServer 直接处理，
// claw_span_vmm.exe 不参与数据转发。
//
// 用法:
//   claw_span_vmm.exe --distro ClawSpan [--daemon-pipe \\.\pipe\crew-shell-service]
//                      [--log-level info] [-f]

#include "vmm_app.h"

#include "common/log.h"

#include <cxxopts.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

#ifndef CLAWSPAN_VERSION
#define CLAWSPAN_VERSION "0.1.1"
#endif

static bool parseArgs(int argc, char** argv, clawspan::vmm::VmmConfig& config)
{
	cxxopts::Options opts("vmm", "ClawSpan VMM — VM lifecycle manager for WSL2 distro");

	opts.add_options()
		("d,distro",
		 "WSL2 distro 名称（必选）",
		 cxxopts::value<std::string>())
		("daemon-pipe",
		 "daemon VM 控制通道 Named Pipe 路径",
		 cxxopts::value<std::string>()->default_value("\\\\.\\pipe\\crew-shell-service"))
		("log-level",
		 "日志级别：trace / debug / info / warn / error / critical / off",
		 cxxopts::value<std::string>()->default_value("info"))
		("f,foreground",
		 "前台运行：日志输出到 stdout",
		 cxxopts::value<bool>()->default_value("false"))
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
		std::cout << "vmm " << CLAWSPAN_VERSION << "\n";
		return false;
	}

	// --distro 是必选参数
	if (!result.count("distro")) {
		std::cerr << "error: --distro is required\n\n" << opts.help() << "\n";
		std::exit(EXIT_FAILURE);
	}

	config.distro_name = result["distro"].as<std::string>();
	config.daemon_pipe = result["daemon-pipe"].as<std::string>();
	config.log_level   = result["log-level"].as<std::string>();
	config.foreground  = result["foreground"].as<bool>();

	return true;
}

int main(int argc, char** argv)
{
	clawspan::vmm::VmmConfig config;
	if (!parseArgs(argc, argv, config)) {
		return EXIT_SUCCESS;
	}

	clawspan::vmm::VmmApp app;
	auto status = app.init(std::move(config));
	if (!status.ok()) {
		LOG_ERROR("vmm init failed: {}", status.message);
		return EXIT_FAILURE;
	}

	if (!app.run()) {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
