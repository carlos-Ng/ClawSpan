#pragma once

#include "common/types.h"
#include "common/error.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace clawspan {
namespace log {

// Mode 描述日志输出目标：控制台（stderr）或滚动文件。
enum class Mode : u8
{
	CONSOLE,
	FILE,
};

// Config 保存日志系统的初始化参数。
struct Config
{
	spdlog::level::level_enum level       = spdlog::level::info;
	Mode                      output_mode = Mode::CONSOLE;
	std::string               log_path;   // 仅当 output_mode == Mode::FILE 时使用
};

// toLower 将字符串转换为全小写副本。
//
// 入参:
// - s: 输入字符串视图。
//
// 出参/返回:
// - 全小写的 std::string。
inline std::string toLower(std::string_view s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	return out;
}

// modeFromString 将字符串解析为 Mode 枚举值，大小写不敏感。
//
// 入参:
// - s:        输入字符串（"console" / "file"）。
// - fallback: 解析失败时的默认值。
//
// 出参/返回:
// - 对应的 Mode 枚举值，或 fallback。
inline Mode modeFromString(std::string_view s, Mode fallback = Mode::CONSOLE)
{
	auto v = toLower(s);
	if (v == "console") {
		return Mode::CONSOLE;
	}
	if (v == "file") {
		return Mode::FILE;
	}
	return fallback;
}

// levelFromString 将字符串解析为 spdlog 日志级别，大小写不敏感。
//
// 入参:
// - s:        级别名称字符串（trace/debug/info/warn/warning/error/critical/off）。
// - fallback: 解析失败时的默认级别。
//
// 出参/返回:
// - 对应的 spdlog::level::level_enum，或 fallback。
inline spdlog::level::level_enum levelFromString(
	std::string_view s,
	spdlog::level::level_enum fallback = spdlog::level::info)
{
	auto v = toLower(s);
	if (v == "trace") {
		return spdlog::level::trace;
	}
	if (v == "debug") {
		return spdlog::level::debug;
	}
	if (v == "info") {
		return spdlog::level::info;
	}
	if (v == "warn" || v == "warning") {
		return spdlog::level::warn;
	}
	if (v == "error") {
		return spdlog::level::err;
	}
	if (v == "critical") {
		return spdlog::level::critical;
	}
	if (v == "off") {
		return spdlog::level::off;
	}
	return fallback;
}

// defaultPattern 返回统一的日志格式串（含源文件与行号）。
//
// 出参/返回:
// - 格式串字符串字面量。
inline const char* defaultPattern()
{
	// 包含源文件 + 行号（%s:%#），需通过 SPDLOG_* 宏调用才能展开。
	return "[%H:%M:%S.%e] [%l] %s:%# %v";
}

// stateMutex 返回日志系统的全局状态互斥锁（Meyer's singleton）。
//
// 出参/返回:
// - 全局 std::mutex 引用。
inline std::mutex& stateMutex()
{
	static std::mutex m;
	return m;
}

// currentLogger 返回当前激活的 spdlog logger 引用（Meyer's singleton）。
//
// 出参/返回:
// - 全局 shared_ptr<spdlog::logger> 引用。
inline std::shared_ptr<spdlog::logger>& currentLogger()
{
	static std::shared_ptr<spdlog::logger> lg;
	return lg;
}

// isConfigured 返回日志系统是否已完成显式初始化的标志引用。
//
// 出参/返回:
// - 全局 bool 引用。
inline bool& isConfigured()
{
	static bool v = false;
	return v;
}

// installDefaultConsoleLogger 在用户未调用 init() 前安装一个控制台兜底 logger。
// 不设置 isConfigured()，允许后续的 init() 覆盖。
inline void installDefaultConsoleLogger()
{
	auto sinks = std::vector<spdlog::sink_ptr>{};
	sinks.emplace_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());

	auto lg = std::make_shared<spdlog::logger>("clawspan", sinks.begin(), sinks.end());
	lg->set_level(spdlog::level::info);
	lg->set_pattern(defaultPattern());
	lg->flush_on(spdlog::level::info);

	spdlog::set_default_logger(lg);
	currentLogger() = std::move(lg);
}

// init 根据 Config 初始化日志系统，只有第一次调用生效。
//
// 入参:
// - cfg: 日志配置，含级别、输出模式和文件路径。
inline void init(const Config& cfg)
{
	std::lock_guard<std::mutex> lock(stateMutex());

	// 已完成显式配置，保留第一次的配置不覆盖
	if (isConfigured()) {
		return;
	}

	std::vector<spdlog::sink_ptr> sinks;
	sinks.reserve(1);

	if (cfg.output_mode == Mode::FILE) {
		if (!cfg.log_path.empty()) {
			sinks.emplace_back(
			    std::make_shared<spdlog::sinks::basic_file_sink_mt>(cfg.log_path, true));
		} else {
			// mode=FILE 但未提供路径，降级为控制台输出
			sinks.emplace_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
		}
	} else {
		sinks.emplace_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
	}

	auto lg = std::make_shared<spdlog::logger>("clawspan", sinks.begin(), sinks.end());
	lg->set_level(cfg.level);
	lg->set_pattern(defaultPattern());
	lg->flush_on(spdlog::level::info);

	spdlog::set_default_logger(lg);
	currentLogger() = std::move(lg);
	isConfigured()  = true;
}

// logger 返回当前 logger，若尚未初始化则安装兜底控制台 logger。
//
// 出参/返回:
// - 当前激活的 shared_ptr<spdlog::logger>。
inline std::shared_ptr<spdlog::logger> logger()
{
	std::lock_guard<std::mutex> lock(stateMutex());
	if (!currentLogger()) {
		installDefaultConsoleLogger();
	}
	return currentLogger();
}

// setLevel 动态修改当前 logger 的日志级别。
//
// 入参:
// - level: 新的 spdlog 日志级别。
inline void setLevel(spdlog::level::level_enum level)
{
	logger()->set_level(level);
}

} // namespace log
} // namespace clawspan

// 统一日志宏（须通过宏调用以展开 %s:%# 源文件与行号）。
#define LOG_TRACE(...)    do { ::clawspan::log::logger(); SPDLOG_TRACE(__VA_ARGS__);    } while (0)
#define LOG_DEBUG(...)    do { ::clawspan::log::logger(); SPDLOG_DEBUG(__VA_ARGS__);    } while (0)
#define LOG_INFO(...)     do { ::clawspan::log::logger(); SPDLOG_INFO(__VA_ARGS__);     } while (0)
#define LOG_WARN(...)     do { ::clawspan::log::logger(); SPDLOG_WARN(__VA_ARGS__);     } while (0)
#define LOG_ERROR(...)    do { ::clawspan::log::logger(); SPDLOG_ERROR(__VA_ARGS__);    } while (0)
#define LOG_CRITICAL(...) do { ::clawspan::log::logger(); SPDLOG_CRITICAL(__VA_ARGS__); } while (0)
