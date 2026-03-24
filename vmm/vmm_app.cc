#include "vmm_app.h"

#include "vmm/vm_manager.h"

#include "common/log.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#ifdef _WIN32
#  include <windows.h>
#else
#  error "Platform not supported"
#endif

namespace clawspan {
namespace vmm {

// ─── g_vmm_shutdown_event ──────────────────────────────────────────────────

#ifdef _WIN32
static HANDLE g_vmm_shutdown_event = INVALID_HANDLE_VALUE;

static BOOL WINAPI vmmConsoleCtrlHandler(DWORD ctrl_type)
{
	switch (ctrl_type) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		LOG_INFO("vmm: received ctrl event {}, shutting down", ctrl_type);
		if (g_vmm_shutdown_event != INVALID_HANDLE_VALUE) {
			::SetEvent(g_vmm_shutdown_event);
		}
		return TRUE;
	default:
		return FALSE;
	}
}
#endif

// ─── Watchdog 配置 ──────────────────────────────────────────────────────────

static constexpr uint32_t WATCHDOG_INTERVAL_MS    = 5000;   // 状态检查间隔 5s
static constexpr uint32_t DISTRO_RESTART_DELAY_MS = 3000;   // 重启前等待 3s
static constexpr uint32_t MAX_RESTART_ATTEMPTS    = 5;      // 最大连续重启尝试

// ─── Implement ──────────────────────────────────────────────────────────────

struct VmmApp::Implement
{
	VmmConfig                            config_;
	std::atomic<bool>                    running_{false};
	std::unique_ptr<VMManagerInterface>  vm_manager_;
	std::wstring                         distro_name_w_;

	// Watchdog 状态
	DistroState  last_known_state_ = DistroState::NOT_REGISTERED;
	uint32_t     restart_attempts_ = 0;
	std::chrono::steady_clock::time_point last_restart_time_;

	// ── OpenClaw 启动辅助 ──────────────────────────────────────────────

	// ensureOpenClawRunning 在 distro 内执行 `openclaw daemon start`。
	// 调用是幂等的：若服务已在运行，openclaw 会立即返回成功。
	// wait_for_boot_ms：distro 刚从 STOPPED 启动时，需要等 systemd 初始化；
	//                   若 distro 之前已 RUNNING，传 0 跳过等待。
	void ensureOpenClawRunning(uint32_t wait_for_boot_ms = 0)
	{
		if (!vm_manager_) { return; }

		if (wait_for_boot_ms > 0) {
			LOG_INFO("vmm: waiting {}ms for distro to boot before starting openclaw ...",
			         wait_for_boot_ms);
			std::this_thread::sleep_for(std::chrono::milliseconds(wait_for_boot_ms));
		}

		LOG_INFO("vmm: ensuring openclaw daemon is running in '{}'", config_.distro_name);

		// runCommand 启动命令并返回进程句柄（不等待）
		HANDLE h = static_cast<HANDLE>(
			vm_manager_->runCommand(
				distro_name_w_,
				// 不再使用 sudo，避免 secure_path 导致找不到 openclaw；
				// 通过登录 shell 显式补齐 PNPM_HOME/PATH，保证命令可见。
				L"bash -lc \"export PNPM_HOME=/usr/local/share/pnpm; export PATH=$PNPM_HOME:$PATH; openclaw daemon start\""));

		if (h == INVALID_HANDLE_VALUE) {
			LOG_WARN("vmm: failed to launch 'openclaw daemon start' in distro");
			return;
		}

		// 等待命令完成（最多 15 秒）
		DWORD result = WaitForSingleObject(h, 15000);
		if (result == WAIT_TIMEOUT) {
			LOG_WARN("vmm: 'openclaw daemon start' timed out (15s)");
		} else {
			DWORD exit_code = 0;
			GetExitCodeProcess(h, &exit_code);
			if (exit_code == 0) {
				LOG_INFO("vmm: openclaw daemon started (or was already running)");
			} else {
				LOG_WARN("vmm: 'openclaw daemon start' exited with code {}", exit_code);
			}
		}
		CloseHandle(h);
	}

	// ── Watchdog 单次检查 ──────────────────────────────────────────────

	void watchdogCheck()
	{
		if (!vm_manager_) {
			return;
		}

		DistroState state = vm_manager_->getDistroState(distro_name_w_);

		if (state == last_known_state_) {
			// 状态未变化，正常运行时重置重启计数
			if (state == DistroState::RUNNING) {
				auto now = std::chrono::steady_clock::now();
				auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
					now - last_restart_time_).count();
				// 稳定运行 60 秒以上，重置计数
				if (elapsed > 60) {
					restart_attempts_ = 0;
				}
			}
			return;
		}

		// 状态变化
		const char* state_str = "unknown";
		switch (state) {
		case DistroState::RUNNING:        state_str = "running";        break;
		case DistroState::STOPPED:        state_str = "stopped";        break;
		case DistroState::NOT_REGISTERED: state_str = "not_registered"; break;
		}

		LOG_INFO("vmm: distro '{}' state changed: {} -> {}",
		         config_.distro_name,
		         last_known_state_ == DistroState::RUNNING ? "running" :
		         last_known_state_ == DistroState::STOPPED ? "stopped" : "not_registered",
		         state_str);

		DistroState prev_state = last_known_state_;
		last_known_state_ = state;

		// 如果 distro 从 RUNNING 变成 STOPPED，尝试重启
		if (prev_state == DistroState::RUNNING &&
			state == DistroState::STOPPED) {

			if (restart_attempts_ >= MAX_RESTART_ATTEMPTS) {
				LOG_ERROR("vmm: distro '{}' crashed {} times, giving up auto-restart",
				          config_.distro_name, restart_attempts_);
				return;
			}

			restart_attempts_++;
			LOG_WARN("vmm: distro '{}' stopped unexpectedly, restarting "
			         "(attempt {}/{})",
			         config_.distro_name,
			         restart_attempts_, MAX_RESTART_ATTEMPTS);

			auto status = vm_manager_->startDistro(distro_name_w_);
			last_restart_time_ = std::chrono::steady_clock::now();

			if (status.ok()) {
				LOG_INFO("vmm: distro '{}' restarted successfully",
				         config_.distro_name);
				last_known_state_ = DistroState::RUNNING;
				// distro 意外停止后重启，需要重新拉起 openclaw
				ensureOpenClawRunning(3000);
			} else {
				LOG_ERROR("vmm: failed to restart distro '{}': {}",
				          config_.distro_name, status.message);
			}
		}
	}
};

// ─── VmmApp 公开接口 ────────────────────────────────────────────────────────

VmmApp::VmmApp()
	: impl_(std::make_unique<Implement>())
{}

VmmApp::~VmmApp()
{
	stop();
}

Status VmmApp::init(VmmConfig config)
{
	impl_->config_ = std::move(config);
	const auto& cfg = impl_->config_;

	// 初始化日志
	log::Config log_cfg;
	log_cfg.level       = log::levelFromString(cfg.log_level);
	log_cfg.output_mode = cfg.foreground ? log::Mode::CONSOLE : log::Mode::FILE;
	log::init(log_cfg);

	LOG_INFO("vmm: starting for distro '{}', daemon pipe '{}'",
	         cfg.distro_name, cfg.daemon_pipe);

	// 转换 distro_name 到 wstring（VMManagerInterface 使用 wstring）
	impl_->distro_name_w_ = std::wstring(cfg.distro_name.begin(),
	                                      cfg.distro_name.end());

	// 初始化 WslVMManager
	impl_->vm_manager_ = createVMManager();
	if (!impl_->vm_manager_) {
		LOG_ERROR("vmm: failed to create VMManager");
		return Status(Status::INTERNAL_ERROR, "failed to create VMManager");
	}

	// 检查 distro 当前状态
	impl_->last_known_state_ =
		impl_->vm_manager_->getDistroState(impl_->distro_name_w_);

	const char* state_str = "unknown";
	switch (impl_->last_known_state_) {
	case DistroState::RUNNING:        state_str = "running";        break;
	case DistroState::STOPPED:        state_str = "stopped";        break;
	case DistroState::NOT_REGISTERED: state_str = "not_registered"; break;
	}
	LOG_INFO("vmm: distro '{}' current state: {}",
	         cfg.distro_name, state_str);

	// 如果 distro 已注册但未运行，启动它
	bool just_started = false;
	if (impl_->last_known_state_ == DistroState::STOPPED) {
		LOG_INFO("vmm: starting distro '{}'", cfg.distro_name);
		auto status = impl_->vm_manager_->startDistro(impl_->distro_name_w_);
		if (status.ok()) {
			impl_->last_known_state_ = DistroState::RUNNING;
			just_started = true;
			LOG_INFO("vmm: distro '{}' started", cfg.distro_name);
		} else {
			LOG_WARN("vmm: failed to start distro '{}': {}",
			         cfg.distro_name, status.message);
		}
	}

	// 确保 openclaw gateway 正在运行
	// 冷启动：等 3 秒让 systemd 完成初始化；distro 已运行时直接触发（幂等）
	if (impl_->last_known_state_ == DistroState::RUNNING) {
		impl_->ensureOpenClawRunning(just_started ? 3000 : 0);
	}

	LOG_INFO("vmm: initialized (watchdog + VM lifecycle management)");
	return Status::Ok();
}

bool VmmApp::run()
{
#ifdef _WIN32
	g_vmm_shutdown_event = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (g_vmm_shutdown_event == INVALID_HANDLE_VALUE) {
		LOG_ERROR("vmm: failed to create shutdown event: {}", ::GetLastError());
		return false;
	}
	if (!::SetConsoleCtrlHandler(vmmConsoleCtrlHandler, TRUE)) {
		LOG_ERROR("vmm: failed to register console ctrl handler: {}", ::GetLastError());
		::CloseHandle(g_vmm_shutdown_event);
		g_vmm_shutdown_event = INVALID_HANDLE_VALUE;
		return false;
	}
#endif

	impl_->running_.store(true);
	LOG_INFO("vmm: watchdog running (distro '{}', interval={}ms)",
	         impl_->config_.distro_name, WATCHDOG_INTERVAL_MS);

	// Watchdog 循环：定期检查 distro 状态，等待 shutdown event 超时返回
#ifdef _WIN32
	while (impl_->running_.load()) {
		DWORD wait = ::WaitForSingleObject(
			g_vmm_shutdown_event, WATCHDOG_INTERVAL_MS);

		if (wait == WAIT_OBJECT_0) {
			// shutdown 信号
			LOG_INFO("vmm: shutdown event received");
			break;
		}

		if (wait == WAIT_TIMEOUT) {
			// 超时 → 执行 watchdog 检查
			impl_->watchdogCheck();
		} else {
			LOG_ERROR("vmm: WaitForSingleObject unexpected result: {}", wait);
			break;
		}
	}

	::SetConsoleCtrlHandler(vmmConsoleCtrlHandler, FALSE);
	::CloseHandle(g_vmm_shutdown_event);
	g_vmm_shutdown_event = INVALID_HANDLE_VALUE;
#endif

	stop();
	return true;
}

void VmmApp::stop()
{
	if (!impl_->running_.exchange(false)) {
		return;
	}
	LOG_INFO("vmm: stopping");

#ifdef _WIN32
	// 触发 shutdown event（如果 run() 还在循环中）
	if (g_vmm_shutdown_event != INVALID_HANDLE_VALUE) {
		::SetEvent(g_vmm_shutdown_event);
	}
#endif

	// 停止 distro（优雅关闭 WSL VM）
	if (impl_->vm_manager_ &&
		impl_->last_known_state_ == DistroState::RUNNING) {
		LOG_INFO("vmm: stopping distro '{}'", impl_->config_.distro_name);
		auto status = impl_->vm_manager_->stopDistro(impl_->distro_name_w_);
		if (status.ok()) {
			LOG_INFO("vmm: distro '{}' stopped", impl_->config_.distro_name);
		} else {
			LOG_WARN("vmm: failed to stop distro '{}': {}",
			         impl_->config_.distro_name, status.message);
		}
	}

	LOG_INFO("vmm: stopped");
}

} // namespace vmm
} // namespace clawspan
