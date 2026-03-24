#include "vmm_launcher.h"

#include "common/log.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#  include <windows.h>
#else
#  error "Platform not supported"
#endif

namespace clawspan {
namespace daemon {

// ── resolveExePath ──────────────────────────────────────────────────────────

/// 解析 claw_span_vmm.exe 路径。如果 config.exe_path 非空则直接使用，
/// 否则在 daemon 同目录查找 claw_span_vmm.exe。
static std::string resolveExePath(const std::string& configured_path)
{
	if (!configured_path.empty()) {
		return configured_path;
	}

	// 从 daemon（当前进程）的可执行文件路径推导 claw_span_vmm.exe 位置
	char buf[MAX_PATH] = {};
	DWORD len = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		return "";
	}

	std::filesystem::path daemon_dir = std::filesystem::path(buf).parent_path();
	std::filesystem::path vmm_path  = daemon_dir / "claw_span_vmm.exe";

	if (std::filesystem::exists(vmm_path)) {
		return vmm_path.string();
	}
	return "";
}

// ── Implement ───────────────────────────────────────────────────────────────

struct VmmLauncher::Implement
{
	VmmLauncherConfig    config_;
	std::string          resolved_exe_path_;
	std::atomic<bool>    running_{false};
	std::atomic<bool>    should_monitor_{false};
	std::thread          monitor_thread_;
	std::mutex           proc_mutex_;

#ifdef _WIN32
	HANDLE               process_handle_ = INVALID_HANDLE_VALUE;
	DWORD                process_id_     = 0;
	HANDLE               stop_event_     = INVALID_HANDLE_VALUE;
	HANDLE               job_handle_     = INVALID_HANDLE_VALUE;
#endif

	uint32_t             rapid_restart_count_ = 0;
	std::chrono::steady_clock::time_point last_start_time_;

	// ── 启动 claw_span_vmm.exe 子进程 ──────────────────────────────────

	bool launchProcess()
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(proc_mutex_);

		// 构建命令行
		std::string cmd = "\"" + resolved_exe_path_ + "\""
			+ " --distro " + config_.distro_name
			+ " --daemon-pipe \"" + config_.daemon_pipe + "\""
			+ " --log-level " + config_.log_level;

		LOG_INFO("vmm_launcher: starting claw_span_vmm.exe: {}", cmd);

		STARTUPINFOA si = {};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi = {};

		// 使用 CREATE_NEW_PROCESS_GROUP 使 claw_span_vmm.exe 可以接收 Ctrl+Break
		BOOL ok = ::CreateProcessA(
			nullptr,
			cmd.data(),
			nullptr, nullptr,
			FALSE,
			CREATE_NEW_PROCESS_GROUP,
			nullptr, nullptr,
			&si, &pi);

		if (!ok) {
			LOG_ERROR("vmm_launcher: CreateProcess failed: {}", ::GetLastError());
			return false;
		}

		// 将 vmm 进程放入 Job Object，确保 daemon 异常退出时 vmm 不会变成孤儿进程。
		if (job_handle_ == INVALID_HANDLE_VALUE) {
			job_handle_ = ::CreateJobObjectA(nullptr, nullptr);
			if (job_handle_ == INVALID_HANDLE_VALUE) {
				LOG_WARN("vmm_launcher: CreateJobObject failed: {}", ::GetLastError());
			} else {
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
				info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				if (!::SetInformationJobObject(
				        job_handle_,
				        JobObjectExtendedLimitInformation,
				        &info,
				        sizeof(info))) {
					LOG_WARN("vmm_launcher: SetInformationJobObject failed: {}", ::GetLastError());
				}
			}
		}
		if (job_handle_ != INVALID_HANDLE_VALUE) {
			if (!::AssignProcessToJobObject(job_handle_, pi.hProcess)) {
				LOG_WARN("vmm_launcher: AssignProcessToJobObject failed: {}", ::GetLastError());
			}
		}

		process_handle_ = pi.hProcess;
		process_id_     = pi.dwProcessId;
		running_.store(true);
		last_start_time_ = std::chrono::steady_clock::now();

		// 不需要线程句柄
		::CloseHandle(pi.hThread);

		LOG_INFO("vmm_launcher: claw_span_vmm.exe started, pid={}", process_id_);
		return true;
#else
		return false;
#endif
	}

	// ── 监控循环 ─────────────────────────────────────────────────────────

	void monitorLoop()
	{
#ifdef _WIN32
		LOG_INFO("vmm_launcher: monitor thread started");

		while (should_monitor_.load()) {
			HANDLE handles[2];
			int handle_count = 0;

			{
				std::lock_guard<std::mutex> lock(proc_mutex_);
				if (process_handle_ != INVALID_HANDLE_VALUE) {
					handles[handle_count++] = process_handle_;
				}
			}

			if (stop_event_ != INVALID_HANDLE_VALUE) {
				handles[handle_count++] = stop_event_;
			}

			if (handle_count == 0) {
				// 无进程也无停止事件，短暂等待后重试
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				continue;
			}

			DWORD wait_result = ::WaitForMultipleObjects(
				handle_count, handles, FALSE, INFINITE);

			if (!should_monitor_.load()) {
				break;
			}

			// 检查是否是 stop_event 触发
			if (stop_event_ != INVALID_HANDLE_VALUE &&
				wait_result == WAIT_OBJECT_0 + handle_count - 1) {
				LOG_INFO("vmm_launcher: stop event received in monitor");
				break;
			}

			// 进程退出
			{
				std::lock_guard<std::mutex> lock(proc_mutex_);
				if (process_handle_ != INVALID_HANDLE_VALUE) {
					DWORD exit_code = 0;
					::GetExitCodeProcess(process_handle_, &exit_code);
					LOG_WARN("vmm_launcher: claw_span_vmm.exe exited, code={}, pid={}",
					         exit_code, process_id_);

					::CloseHandle(process_handle_);
					process_handle_ = INVALID_HANDLE_VALUE;
					process_id_     = 0;
					running_.store(false);
				}
			}

			if (!should_monitor_.load()) {
				break;
			}

			// 检查快速重启计数
			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
				now - last_start_time_).count();

			if (elapsed < 10) {
				rapid_restart_count_++;
				if (rapid_restart_count_ >= config_.max_rapid_restarts) {
					LOG_ERROR(
						"vmm_launcher: {} rapid restarts in a row, giving up",
						rapid_restart_count_);
					break;
				}
			} else {
				rapid_restart_count_ = 0;
			}

			// 等待重启延迟
			LOG_INFO("vmm_launcher: restarting in {}ms ...",
			         config_.restart_delay_ms);

			DWORD delay_result = WAIT_TIMEOUT;
			if (stop_event_ != INVALID_HANDLE_VALUE) {
				delay_result = ::WaitForSingleObject(
					stop_event_, config_.restart_delay_ms);
			} else {
				std::this_thread::sleep_for(
					std::chrono::milliseconds(config_.restart_delay_ms));
			}

			if (!should_monitor_.load() || delay_result == WAIT_OBJECT_0) {
				break;
			}

			// 重启
			if (!launchProcess()) {
				LOG_ERROR("vmm_launcher: restart failed, retrying ...");
			}
		}

		LOG_INFO("vmm_launcher: monitor thread exiting");
#endif
	}

	// ── 停止进程 ─────────────────────────────────────────────────────────

	void stopProcess()
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(proc_mutex_);

		if (process_handle_ == INVALID_HANDLE_VALUE) {
			return;
		}

		LOG_INFO("vmm_launcher: sending Ctrl+Break to claw_span_vmm.exe pid={}",
		         process_id_);

		// 向 claw_span_vmm.exe 的进程组发送 CTRL_BREAK_EVENT
		if (!::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process_id_)) {
			LOG_WARN("vmm_launcher: GenerateConsoleCtrlEvent failed: {}",
			         ::GetLastError());
		}

		// 等待进程退出
		DWORD wait = ::WaitForSingleObject(
			process_handle_, config_.stop_timeout_ms);

		if (wait == WAIT_TIMEOUT) {
		LOG_WARN("vmm_launcher: claw_span_vmm.exe did not exit within {}ms, "
		         "terminating",
			         config_.stop_timeout_ms);
			::TerminateProcess(process_handle_, 1);
			::WaitForSingleObject(process_handle_, 1000);
		}

		DWORD exit_code = 0;
		::GetExitCodeProcess(process_handle_, &exit_code);
		LOG_INFO("vmm_launcher: claw_span_vmm.exe stopped, exit_code={}", exit_code);

		::CloseHandle(process_handle_);
		process_handle_ = INVALID_HANDLE_VALUE;
		process_id_     = 0;
		running_.store(false);
#endif
	}
};

// ── VmmLauncher 公开接口 ────────────────────────────────────────────────────

VmmLauncher::VmmLauncher()
	: impl_(std::make_unique<Implement>())
{}

VmmLauncher::~VmmLauncher()
{
	stop();
}

Status VmmLauncher::start(VmmLauncherConfig config)
{
	if (impl_->should_monitor_.load()) {
		LOG_WARN("vmm_launcher: already started");
		return Status::Ok();
	}

	impl_->config_ = std::move(config);

	// 解析 claw_span_vmm.exe 路径
	impl_->resolved_exe_path_ = resolveExePath(impl_->config_.exe_path);
	if (impl_->resolved_exe_path_.empty()) {
		LOG_ERROR("vmm_launcher: claw_span_vmm.exe not found");
		return Status(Status::IO_ERROR, "claw_span_vmm.exe not found");
	}

	if (!std::filesystem::exists(impl_->resolved_exe_path_)) {
		LOG_ERROR("vmm_launcher: claw_span_vmm.exe does not exist at '{}'",
		          impl_->resolved_exe_path_);
		return Status(Status::IO_ERROR,
		              "claw_span_vmm.exe does not exist: " + impl_->resolved_exe_path_);
	}

	LOG_INFO("vmm_launcher: using claw_span_vmm.exe at '{}'", impl_->resolved_exe_path_);

#ifdef _WIN32
	// 创建停止事件
	impl_->stop_event_ = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
#endif

	// 启动 vmm.exe 子进程
	if (!impl_->launchProcess()) {
		LOG_ERROR("vmm_launcher: initial launch failed");
#ifdef _WIN32
		if (impl_->stop_event_ != INVALID_HANDLE_VALUE) {
			::CloseHandle(impl_->stop_event_);
			impl_->stop_event_ = INVALID_HANDLE_VALUE;
		}
	if (impl_->job_handle_ != INVALID_HANDLE_VALUE) {
		::CloseHandle(impl_->job_handle_);
		impl_->job_handle_ = INVALID_HANDLE_VALUE;
	}
#endif
		return Status(Status::IO_ERROR, "claw_span_vmm.exe launch failed");
	}

	// 启动监控线程
	impl_->should_monitor_.store(true);
	impl_->monitor_thread_ = std::thread([this]() {
		impl_->monitorLoop();
	});

	return Status::Ok();
}

void VmmLauncher::stop()
{
	if (!impl_->should_monitor_.exchange(false)) {
		return;
	}

	LOG_INFO("vmm_launcher: stopping");

#ifdef _WIN32
	// 通知监控线程退出
	if (impl_->stop_event_ != INVALID_HANDLE_VALUE) {
		::SetEvent(impl_->stop_event_);
	}
#endif

	// 等待监控线程结束
	if (impl_->monitor_thread_.joinable()) {
		impl_->monitor_thread_.join();
	}

	// 停止 vmm.exe 进程
	impl_->stopProcess();

#ifdef _WIN32
	if (impl_->stop_event_ != INVALID_HANDLE_VALUE) {
		::CloseHandle(impl_->stop_event_);
		impl_->stop_event_ = INVALID_HANDLE_VALUE;
	}
	if (impl_->job_handle_ != INVALID_HANDLE_VALUE) {
		::CloseHandle(impl_->job_handle_);
		impl_->job_handle_ = INVALID_HANDLE_VALUE;
	}
#endif

	LOG_INFO("vmm_launcher: stopped");
}

bool VmmLauncher::isRunning() const
{
	return impl_->running_.load();
}

} // namespace daemon
} // namespace clawspan
