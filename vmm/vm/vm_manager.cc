// vm_manager.cc — WslVMManager 实现
//
// 通过 WSL2 COM API（IWSLService / IWSLDistribution）或 wslapi.h / wsl.exe
// 管理 distro 生命周期：创建、启动、停止、快照、恢复、销毁。
//
// 适配自 AI-agent-sec src/vm/vm_manager.cc，主要变更：
//   - 命名空间：enclave::vm → clawspan::vmm
//   - 返回值：bool/wstring → Status（含结构化错误码）
//   - 安装目录：%LOCALAPPDATA%\agent-enclave → %LOCALAPPDATA%\ClawSpan
//   - 日志：全量 LOG_INFO / LOG_ERROR / LOG_WARN
//   - lastCreateHr() + getLastWslDiagnostics() 合并为 lastDiagnostics()

#include "vmm/vm_manager.h"
#include "vm/wsl_com_interfaces.h"
#include "common/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include "vm/wslapi_loader.h"
#include <shlobj.h>

#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace clawspan {
namespace vmm {

// ── 内部工具函数 ─────────────────────────────────────────────────────────────

// wtoUtf8 将宽字符串转换为 UTF-8（用于日志输出）。
static std::string wtoUtf8(const std::wstring& ws)
{
	if (ws.empty()) { return {}; }
	const int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
	                                     nullptr, 0, nullptr, nullptr);
	if (len <= 0) { return "(conversion failed)"; }
	std::string out(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
	                    &out[0], len, nullptr, nullptr);
	return out;
}

// getUserProfileDir 返回 %USERPROFILE% 目录路径（宽字符）。
static std::wstring getUserProfileDir()
{
	wchar_t buf[MAX_PATH] = {};
	if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, buf))) {
		return buf;
	}
	return L"";
}

// runWslExe 通过 CreateProcess 同步执行 wsl.exe 并等待结束，返回退出码。
static DWORD runWslExe(const std::wstring& args)
{
	std::wstring cmd = L"wsl.exe " + args;
	STARTUPINFOW        si{};
	PROCESS_INFORMATION pi{};
	si.cb = sizeof(si);

	BOOL ok = CreateProcessW(nullptr,
	                         cmd.data(),
	                         nullptr,
	                         nullptr,
	                         FALSE,
	                         CREATE_NO_WINDOW,
	                         nullptr,
	                         nullptr,
	                         &si,
	                         &pi);
	if (!ok) {
		LOG_ERROR("CreateProcessW failed for wsl.exe, error={}", GetLastError());
		return GetLastError();
	}

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD exit_code = 1;
	GetExitCodeProcess(pi.hProcess, &exit_code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return exit_code;
}

// runWslExeWithOutput 执行 wsl.exe 并捕获 stdout 输出。
static DWORD runWslExeWithOutput(const std::wstring& args, std::wstring& output)
{
	output.clear();

	SECURITY_ATTRIBUTES sa{};
	sa.nLength        = sizeof(sa);
	sa.bInheritHandle = TRUE;

	HANDLE read_pipe  = INVALID_HANDLE_VALUE;
	HANDLE write_pipe = INVALID_HANDLE_VALUE;
	if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
		return GetLastError();
	}
	// 子进程只继承 write 端
	SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

	std::wstring cmd = L"wsl.exe " + args;
	STARTUPINFOW si{};
	si.cb         = sizeof(si);
	si.dwFlags    = STARTF_USESTDHANDLES;
	si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = write_pipe;
	si.hStdError  = write_pipe;

	PROCESS_INFORMATION pi{};
	BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
	                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	CloseHandle(write_pipe); // 父进程关闭 write 端

	if (!ok) {
		CloseHandle(read_pipe);
		return GetLastError();
	}

	// 读取输出
	char buf[4096];
	DWORD bytesRead;
	std::string raw;
	while (ReadFile(read_pipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
		raw.append(buf, bytesRead);
	}
	CloseHandle(read_pipe);

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD exit_code = 1;
	GetExitCodeProcess(pi.hProcess, &exit_code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	// wsl.exe 输出是 UTF-16LE
	if (raw.size() >= 2) {
		// 检查 BOM
		const auto* wdata = reinterpret_cast<const wchar_t*>(raw.data());
		size_t wlen = raw.size() / sizeof(wchar_t);
		if (wlen > 0 && wdata[0] == 0xFEFF) {
			wdata++;
			wlen--;
		}
		output.assign(wdata, wlen);
	}
	return exit_code;
}

// writeWslConfig 将自定义内核路径写入 %USERPROFILE%\.wslconfig 的 [wsl2] 段。
// 若文件不存在则创建；若 [wsl2] 段已有 kernel= 行则替换，否则追加。
static bool writeWslConfig(const std::wstring& kernel_path)
{
	std::wstring profile_dir = getUserProfileDir();
	if (profile_dir.empty()) {
		LOG_ERROR("failed to get user profile directory");
		return false;
	}

	const std::wstring config_path = profile_dir + L"\\.wslconfig";

	// 读取现有内容
	std::wifstream fin(config_path);
	std::vector<std::wstring> lines;
	bool in_wsl2 = false;
	bool kernel_written = false;

	if (fin.is_open()) {
		std::wstring line;
		while (std::getline(fin, line)) {
			if (line == L"[wsl2]") {
				in_wsl2 = true;
				lines.push_back(line);
				continue;
			}
			if (!line.empty() && line[0] == L'[') {
				in_wsl2 = false;
			}
			// 在 [wsl2] 段内替换 kernel= 行
			if (in_wsl2 && line.find(L"kernel=") == 0) {
				lines.push_back(L"kernel=" + kernel_path);
				kernel_written = true;
				continue;
			}
			lines.push_back(line);
		}
		fin.close();
	}

	// 若没有替换到，追加 [wsl2] 段和 kernel= 行
	if (!kernel_written) {
		bool has_wsl2_section = false;
		for (const auto& l : lines) {
			if (l == L"[wsl2]") {
				has_wsl2_section = true;
				break;
			}
		}
		if (!has_wsl2_section) {
			lines.push_back(L"");
			lines.push_back(L"[wsl2]");
		}
		lines.push_back(L"kernel=" + kernel_path);
	}

	// 写回文件
	std::wofstream fout(config_path, std::ios::trunc);
	if (!fout.is_open()) {
		LOG_ERROR("failed to write .wslconfig");
		return false;
	}
	for (const auto& l : lines) {
		fout << l << L"\n";
	}
	LOG_INFO(".wslconfig updated with custom kernel path");
	return true;
}

// readPipeInto 从管道读取所有可用数据并追加到 out（按 UTF-16 LE 解码为 UTF-8）。
static void readPipeInto(HANDLE h, std::string& out)
{
	std::vector<char> raw;
	char buf[1024];
	DWORD n = 0;
	while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
		raw.insert(raw.end(), buf, buf + n);
	}
	if (raw.empty()) { return; }
	// 若为 UTF-16 LE（BOM 或偶数长度且可解析），转为 UTF-8
	const bool has_bom = (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF
	                      && static_cast<unsigned char>(raw[1]) == 0xFE);
	const bool likely_utf16 = has_bom || (raw.size() >= 2 && (raw.size() % 2) == 0);
	if (likely_utf16) {
		const wchar_t* wptr = reinterpret_cast<const wchar_t*>(raw.data());
		const size_t wlen = (raw.size() / 2) - (has_bom ? 1 : 0);
		if (has_bom) { wptr += 1; }
		const int mblen = WideCharToMultiByte(CP_UTF8, 0, wptr, static_cast<int>(wlen),
		                                      nullptr, 0, nullptr, nullptr);
		if (mblen > 0) {
			std::string u8(mblen, '\0');
			WideCharToMultiByte(CP_UTF8, 0, wptr, static_cast<int>(wlen), &u8[0], mblen, nullptr, nullptr);
			out += u8;
			return;
		}
	}
	out.append(raw.begin(), raw.end());
}

// ── 内部工具函数（清理卡住的 WSL 注册表条目）──────────────────────────────────

// cleanupStuckDistro 检查 WSL 注册表，若目标 distro 存在且 State != 1（已安装），
// 则通过 wsl.exe --unregister 将其删除，避免后续 --import 被阻塞（HRESULT 0x8000000d）。
static void cleanupStuckDistro(const std::wstring& name)
{
	static const wchar_t* kLxssKey =
	    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Lxss";
	HKEY hLxss = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kLxssKey, 0, KEY_READ, &hLxss) != ERROR_SUCCESS) {
		return;
	}

	// 枚举所有子键（每个子键是一个 GUID，对应一个 WSL distro）
	wchar_t subKeyName[64]{};
	DWORD idx = 0;
	while (true) {
		DWORD nameLen = static_cast<DWORD>(std::size(subKeyName));
		LONG ret = RegEnumKeyExW(hLxss, idx++, subKeyName, &nameLen,
		                         nullptr, nullptr, nullptr, nullptr);
		if (ret == ERROR_NO_MORE_ITEMS) break;
		if (ret != ERROR_SUCCESS) continue;

		HKEY hSub = nullptr;
		if (RegOpenKeyExW(hLxss, subKeyName, 0, KEY_READ, &hSub) != ERROR_SUCCESS) continue;

		// 读取 DistributionName
		wchar_t distName[256]{};
		DWORD distLen = sizeof(distName);
		DWORD type = 0;
		LONG r1 = RegQueryValueExW(hSub, L"DistributionName", nullptr, &type,
		                           reinterpret_cast<BYTE*>(distName), &distLen);

		// 读取 State（1 = Installed，其他值 = 安装中/转换中/卸载中）
		DWORD state = 0;
		DWORD stateSize = sizeof(state);
		LONG r2 = RegQueryValueExW(hSub, L"State", nullptr, nullptr,
		                           reinterpret_cast<BYTE*>(&state), &stateSize);
		RegCloseKey(hSub);

		if (r1 != ERROR_SUCCESS || r2 != ERROR_SUCCESS) continue;
		if (name != distName) continue;

		if (state != 1) {
			LOG_WARN("found stuck distro registry entry (State={}), attempting cleanup", state);
			// 找到卡住的条目，先尝试 wsl.exe --unregister（优雅清理）
			std::wstring cmd = L"wsl.exe --unregister \"" + name + L"\"";
			STARTUPINFOW si{ sizeof(si) };
			PROCESS_INFORMATION pi{};
			if (CreateProcessW(nullptr, const_cast<wchar_t*>(cmd.c_str()),
			                   nullptr, nullptr, FALSE,
			                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
				WaitForSingleObject(pi.hProcess, 15000);
				CloseHandle(pi.hProcess);
				CloseHandle(pi.hThread);
			}
			// 无论 wsl --unregister 是否成功，重新检查 key 是否仍存在且 State != 1
			// 若是，则直接强制删除（SHDeleteKeyW 可递归删除含子键的 key）
			HKEY hCheck = nullptr;
			if (RegOpenKeyExW(hLxss, subKeyName, 0, KEY_READ, &hCheck) == ERROR_SUCCESS) {
				DWORD checkState = 0;
				DWORD sz = sizeof(checkState);
				RegQueryValueExW(hCheck, L"State", nullptr, nullptr,
				                 reinterpret_cast<BYTE*>(&checkState), &sz);
				RegCloseKey(hCheck);
				if (checkState != 1) {
					std::wstring fullPath = std::wstring(kLxssKey) + L"\\" + subKeyName;
					SHDeleteKeyW(HKEY_CURRENT_USER, fullPath.c_str());
					LOG_WARN("force-deleted stuck registry key for distro");
				}
			}
		}
		break;
	}
	RegCloseKey(hLxss);
}

// ── WslVMManager 实现类 ───────────────────────────────────────────────────────

// WslVMManager 通过 WSL2 COM API（IWSLService/IWSLDistribution）管理 distro 生命周期。
// 若 COM 接口不可用（旧 Windows 版本），自动回退到 wslapi.h 和 wsl.exe 命令行。
class WslVMManager : public VMManagerInterface
{
public:
	WslVMManager();
	~WslVMManager() override;

	Status                   createDistro(const DistroConfig& config) override;
	Status                   startDistro(const std::wstring& name) override;
	Status                   stopDistro(const std::wstring& name) override;
	Status                   destroyDistro(const std::wstring& name) override;
	DistroState              getDistroState(const std::wstring& name) override;
	void*                    runCommand(const std::wstring& name,
	                                    const std::wstring& command) override;
	std::vector<std::wstring> listDistros() override;
	Status                   snapshotDistro(const std::wstring& name,
	                                        const std::wstring& snapshot_dir,
	                                        const std::wstring& snapshot_name,
	                                        std::wstring&        out_path) override;
	Status                   restoreFromSnapshot(const DistroConfig& cfg,
	                                              const std::wstring& snapshot_path) override;
	std::string              lastDiagnostics() const override;

private:
	// tryGetComService 尝试通过 CoCreateInstance 获取 IWSLService。
	// 返回 true 表示 com_service_ 已就绪。
	bool tryGetComService();

	// createDistroViaCom 使用 IWSLService::RegisterDistribution 创建 distro。
	bool createDistroViaCom(const DistroConfig& config);

	// createDistroViaApi 使用 wslapi.h WslRegisterDistribution 创建 distro。
	bool createDistroViaApi(const DistroConfig& config);

	// runBootstrapCommand 在新 distro 内执行初始化脚本。
	bool runBootstrapCommand(const DistroConfig& config);

	// createDistroViaWslExe 使用 wsl.exe --import 命令创建 distro（第三备用路径）。
	bool createDistroViaWslExe(const DistroConfig& config);

	// cleanupKeepalive 终止并关闭 keepalive 进程句柄。
	void cleanupKeepalive();

	IWSLService* com_service_       = nullptr;
	bool         com_init_attempted_ = false;
	bool         com_available_      = false;
	HRESULT      last_hr_            = S_OK;   // 最近操作的 HRESULT
	std::string  last_diagnostics_;            // wsl.exe 最后一次 stderr/stdout

	// keepalive 进程：在 distro 内运行 sleep infinity，句柄同时用于探活。
	// startDistro 时创建，stopDistro 时销毁。句柄无效 = distro 未启动或已退出。
	HANDLE       keepalive_process_  = INVALID_HANDLE_VALUE;
};

// ── WslVMManager 构造/析构 ────────────────────────────────────────────────────

WslVMManager::WslVMManager()
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	LOG_INFO("WslVMManager initialized");
}

WslVMManager::~WslVMManager()
{
	cleanupKeepalive();
	if (com_service_) {
		com_service_->Release();
		com_service_ = nullptr;
	}
	CoUninitialize();
	LOG_INFO("WslVMManager destroyed");
}

void WslVMManager::cleanupKeepalive()
{
	if (keepalive_process_ != INVALID_HANDLE_VALUE) {
		TerminateProcess(keepalive_process_, 0);
		WaitForSingleObject(keepalive_process_, 3000);
		CloseHandle(keepalive_process_);
		keepalive_process_ = INVALID_HANDLE_VALUE;
	}
}

// tryGetComService 尝试通过 COM 获取 IWSLService 接口。
bool WslVMManager::tryGetComService()
{
	if (com_init_attempted_) {
		return com_available_;
	}
	com_init_attempted_ = true;

	HRESULT hr = CoCreateInstance(CLSID_WslService,
	                              nullptr,
	                              CLSCTX_LOCAL_SERVER,
	                              IID_IWSLService,
	                              reinterpret_cast<void**>(&com_service_));
	com_available_ = SUCCEEDED(hr) && com_service_ != nullptr;
	if (com_available_) {
		LOG_INFO("WSL COM service acquired");
	} else {
		LOG_WARN("WSL COM service unavailable (hr=0x{:08x}), will fall back to wslapi/wsl.exe",
		         static_cast<unsigned>(hr));
	}
	return com_available_;
}

// ── 私有辅助方法 ──────────────────────────────────────────────────────────────

bool WslVMManager::createDistroViaCom(const DistroConfig& config)
{
	BSTR error_msg = nullptr;
	const auto flags_com = static_cast<WslDistributionFlags>(
		0);
	HRESULT hr = com_service_->RegisterDistribution(
		config.name.c_str(),
		config.rootfs_path.c_str(),
		config.default_uid,
		flags_com,
		&error_msg);

	if (error_msg) {
		// 将 COM 错误消息转为 UTF-8 追加到 diagnostics
		int len = WideCharToMultiByte(CP_UTF8, 0, error_msg, -1, nullptr, 0, nullptr, nullptr);
		if (len > 0) {
			std::string msg(len - 1, '\0');
			WideCharToMultiByte(CP_UTF8, 0, error_msg, -1, &msg[0], len, nullptr, nullptr);
			last_diagnostics_ += msg;
		}
		SysFreeString(error_msg);
	}
	if (FAILED(hr)) {
		last_hr_ = hr;
		LOG_ERROR("createDistroViaCom failed (hr=0x{:08x})", static_cast<unsigned>(hr));
	}
	return SUCCEEDED(hr);
}

bool WslVMManager::createDistroViaApi(const DistroConfig& config)
{
	HRESULT hr = WslRegisterDistribution(config.name.c_str(),
	                                     config.rootfs_path.c_str());
	last_hr_ = hr;
	if (FAILED(hr)) {
		LOG_ERROR("WslRegisterDistribution failed (hr=0x{:08x})", static_cast<unsigned>(hr));
		return false;
	}

	// 配置 UID 和标志
	const auto flags_api = static_cast<int>(
		0);
	hr = WslConfigureDistribution(
		config.name.c_str(),
		config.default_uid,
		flags_api);
	if (FAILED(hr)) {
		last_hr_ = hr;
		LOG_WARN("WslConfigureDistribution failed (hr=0x{:08x}), distro registered but unconfigured",
		         static_cast<unsigned>(hr));
	}
	return SUCCEEDED(hr);
}

bool WslVMManager::createDistroViaWslExe(const DistroConfig& config)
{
	last_diagnostics_.clear();

	// 确定安装目录
	std::wstring install_dir = config.install_dir;
	if (install_dir.empty()) {
		wchar_t local_app[MAX_PATH]{};
		if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_app))) {
			last_hr_ = HRESULT_FROM_WIN32(GetLastError());
			LOG_ERROR("failed to get LOCALAPPDATA");
			return false;
		}
		install_dir = std::wstring(local_app)
		            + L"\\ClawSpan\\distros\\" + config.name;
	}
	SHCreateDirectoryExW(nullptr, install_dir.c_str(), nullptr);

	// 构造命令：wsl.exe --import <name> <install_dir> <rootfs>
	std::wstring cmd = L"wsl.exe --import \"" + config.name
	                 + L"\" \"" + install_dir
	                 + L"\" \"" + config.rootfs_path + L"\"";

	SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
	HANDLE hOutR = nullptr, hOutW = nullptr, hErrR = nullptr, hErrW = nullptr;
	if (!CreatePipe(&hOutR, &hOutW, &sa, 0) || !CreatePipe(&hErrR, &hErrW, &sa, 0)) {
		last_hr_ = HRESULT_FROM_WIN32(GetLastError());
		LOG_ERROR("CreatePipe failed for wsl.exe import");
		return false;
	}
	SetHandleInformation(hOutW, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	SetHandleInformation(hErrW, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(hErrR, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si{sizeof(si)};
	si.dwFlags     = STARTF_USESTDHANDLES;
	si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput  = hOutW;
	si.hStdError   = hErrW;
	PROCESS_INFORMATION pi{};

	std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
	cmd_buf.push_back(L'\0');

	BOOL ok = CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr,
	                         TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	CloseHandle(hOutW);
	CloseHandle(hErrW);
	hOutW = hErrW = nullptr;

	if (!ok) {
		CloseHandle(hOutR);
		CloseHandle(hErrR);
		last_hr_ = HRESULT_FROM_WIN32(GetLastError());
		LOG_ERROR("CreateProcessW failed for wsl.exe --import");
		return false;
	}

	WaitForSingleObject(pi.hProcess, 120'000);
	DWORD ec = 1;
	GetExitCodeProcess(pi.hProcess, &ec);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	readPipeInto(hOutR, last_diagnostics_);
	if (!last_diagnostics_.empty()) { last_diagnostics_ += "\n"; }
	readPipeInto(hErrR, last_diagnostics_);
	CloseHandle(hOutR);
	CloseHandle(hErrR);

	if (ec != 0) {
		last_hr_ = HRESULT_FROM_WIN32(ec);
		LOG_ERROR("wsl.exe --import exited with code {}", ec);
		return false;
	}
	last_hr_ = S_OK;
	LOG_INFO("wsl.exe --import succeeded");
	return true;
}

bool WslVMManager::runBootstrapCommand(const DistroConfig& config)
{
	if (config.bootstrap_cmd.empty()) {
		return true;
	}
	LOG_INFO("running bootstrap command in distro");
	void* handle = runCommand(config.name, config.bootstrap_cmd);
	if (handle == INVALID_HANDLE_VALUE) {
		LOG_ERROR("bootstrap command launch failed");
		return false;
	}
	CloseHandle(static_cast<HANDLE>(handle));
	return true;
}

// ── 公开方法实现 ──────────────────────────────────────────────────────────────

Status WslVMManager::createDistro(const DistroConfig& config)
{
	LOG_INFO("createDistro: name={}", wtoUtf8(config.name));

	// 若已存在同名 distro（State=1），拒绝重复创建
	if (WslIsDistributionRegistered(config.name.c_str())) {
		LOG_WARN("distro already registered");
		return {Status::ALREADY_EXISTS, "distro already registered"};
	}

	// 清理可能残留的卡住注册表条目（State != 1），防止 --import 被 0x8000000d 阻塞
	cleanupStuckDistro(config.name);

	// 若指定了自定义内核，写入 .wslconfig
	if (!config.kernel_path.empty()) {
		if (!writeWslConfig(config.kernel_path)) {
			return {Status::INTERNAL_ERROR, "failed to write .wslconfig"};
		}
	}

	// 优先使用 wsl.exe --import（Store 版 WSL 下 COM/API 常返回 E_PENDING）。
	// 若 WSL 返回"正在进行此分发的安装/卸载/转换"(0x8000000d)，则等待后重试。
	constexpr int kMaxRetries = 8;
	constexpr int kRetryDelayMs = 20000;
	bool registered = false;
	for (int attempt = 0; attempt < kMaxRetries && !registered; ++attempt) {
		if (attempt > 0) {
			LOG_INFO("retrying wsl.exe --import (attempt {}/{})", attempt + 1, kMaxRetries);
			Sleep(kRetryDelayMs);
		}
		registered = createDistroViaWslExe(config);
		if (!registered && (last_diagnostics_.find("0x8000000d") != std::string::npos
		                    || last_diagnostics_.find("正在进行") != std::string::npos)) {
			// 后台有分发在安装/转换，继续重试
			continue;
		}
		if (!registered) { break; }
	}
	if (!registered) {
		const HRESULT hr_wslexe = last_hr_;
		if (tryGetComService()) {
			registered = createDistroViaCom(config);
		}
		if (!registered) {
			registered = createDistroViaApi(config);
		}
		if (!registered && hr_wslexe != S_OK) {
			last_hr_ = hr_wslexe;
		}
	}

	if (!registered) {
		LOG_ERROR("all createDistro paths failed (hr=0x{:08x})", static_cast<unsigned>(last_hr_));
		return {Status::IO_ERROR, "distro import failed: " + last_diagnostics_};
	}

	// 执行 bootstrap 脚本
	if (!runBootstrapCommand(config)) {
		LOG_WARN("bootstrap command failed, distro registered but not bootstrapped");
		// distro 已注册但 bootstrap 失败，不视为致命错误
	}

	LOG_INFO("createDistro succeeded");
	return Status::Ok();
}

Status WslVMManager::startDistro(const std::wstring& name)
{
	LOG_INFO("startDistro: {}", wtoUtf8(name));

	if (!WslIsDistributionRegistered(name.c_str())) {
		LOG_WARN("distro not registered");
		return {Status::NOT_FOUND, "distro not registered"};
	}

	// 清理旧的 keepalive 进程（如果有）
	cleanupKeepalive();

	// 清扫 distro 内残留的 sleep infinity 进程（防止 daemon 崩溃后遗留孤儿）。
	// pkill 失败（distro 未运行 / 无残留）不影响后续流程。
	// 注意：这条命令本身会短暂唤醒 distro（如果它没在跑），正好是我们要的效果。
	runWslExe(L"-d " + name + L" -- /usr/bin/pkill -x sleep");

	// 策略：启动一个 `wsl.exe -d <name> -- sleep infinity` 后台进程。
	// 该进程有两个作用：
	//   1. 维持 WSL interop session，防止 distro 被 idle 回收
	//   2. 句柄 keepalive_process_ 用于 getDistroState() 探活
	//      ── 句柄存活 = distro 在运行，句柄退出 = distro 真的停了
	std::wstring cmd = L"wsl.exe -d " + name + L" -- sleep infinity";
	STARTUPINFOW        si{};
	PROCESS_INFORMATION pi{};
	si.cb = sizeof(si);

	BOOL ok = CreateProcessW(nullptr,
	                         cmd.data(),
	                         nullptr,
	                         nullptr,
	                         FALSE,
	                         CREATE_NO_WINDOW,
	                         nullptr,
	                         nullptr,
	                         &si,
	                         &pi);
	if (!ok) {
		LOG_ERROR("startDistro: CreateProcessW failed, error={}", GetLastError());
		return {Status::IO_ERROR, "failed to launch keepalive process"};
	}

	CloseHandle(pi.hThread);
	keepalive_process_ = pi.hProcess;

	// 等待短暂时间，确认进程没有立即退出（比如 distro 不存在的情况）
	DWORD wait = WaitForSingleObject(keepalive_process_, 2000);
	if (wait == WAIT_OBJECT_0) {
		// 进程已退出 — 启动失败
		DWORD exit_code = 1;
		GetExitCodeProcess(keepalive_process_, &exit_code);
		CloseHandle(keepalive_process_);
		keepalive_process_ = INVALID_HANDLE_VALUE;
		LOG_ERROR("startDistro: keepalive exited immediately (exit_code={})", exit_code);
		return {Status::IO_ERROR, "distro failed to start"};
	}

	// WAIT_TIMEOUT — 进程仍在运行，说明 distro 启动成功
	LOG_INFO("startDistro succeeded (keepalive pid={})", pi.dwProcessId);
	return Status::Ok();
}

Status WslVMManager::stopDistro(const std::wstring& name)
{
	LOG_INFO("stopDistro: {}", wtoUtf8(name));

	// 先清理 keepalive 进程
	cleanupKeepalive();

	// 尝试 COM 路径
	if (tryGetComService()) {
		IWSLDistribution* dist = nullptr;
		HRESULT hr = com_service_->GetDistributionByName(name.c_str(), &dist);
		if (SUCCEEDED(hr) && dist) {
			hr = dist->Terminate();
			dist->Release();
			if (SUCCEEDED(hr)) {
				LOG_INFO("stopDistro succeeded via COM");
				return Status::Ok();
			}
		}
	}

	// 回退：wsl.exe --terminate <name>
	std::wstring args = L"--terminate " + name;
	if (runWslExe(args) == 0) {
		LOG_INFO("stopDistro succeeded via wsl.exe");
		return Status::Ok();
	}

	LOG_ERROR("stopDistro failed");
	return {Status::IO_ERROR, "failed to stop distro"};
}

Status WslVMManager::destroyDistro(const std::wstring& name)
{
	LOG_INFO("destroyDistro: {}", wtoUtf8(name));

	if (!WslIsDistributionRegistered(name.c_str())) {
		LOG_INFO("distro not registered, nothing to destroy");
		return Status::Ok(); // 已不存在，视为成功
	}

	// 先确保已停止
	stopDistro(name);

	// 尝试 COM 路径
	if (tryGetComService()) {
		HRESULT hr = com_service_->UnregisterDistribution(name.c_str());
		if (SUCCEEDED(hr)) {
			LOG_INFO("destroyDistro succeeded via COM");
			return Status::Ok();
		}
	}

	// 回退：wsl.exe --unregister <name>
	std::wstring args = L"--unregister " + name;
	if (runWslExe(args) == 0) {
		LOG_INFO("destroyDistro succeeded via wsl.exe");
		return Status::Ok();
	}

	LOG_ERROR("destroyDistro failed");
	return {Status::IO_ERROR, "failed to destroy distro"};
}

DistroState WslVMManager::getDistroState(const std::wstring& name)
{
	if (!WslIsDistributionRegistered(name.c_str())) {
		return DistroState::NOT_REGISTERED;
	}

	// 优先通过 keepalive 进程句柄判断：
	// 句柄存活 = distro 在运行（sleep infinity 在 distro 内跑着）
	// 句柄退出 = distro 真的停了（WSL 终止了 distro，sleep 被杀）
	if (keepalive_process_ != INVALID_HANDLE_VALUE) {
		DWORD exit_code = 0;
		if (GetExitCodeProcess(keepalive_process_, &exit_code) &&
		    exit_code == STILL_ACTIVE) {
			return DistroState::RUNNING;
		}
		// keepalive 已退出 — distro 停了
		CloseHandle(keepalive_process_);
		keepalive_process_ = INVALID_HANDLE_VALUE;
		return DistroState::STOPPED;
	}

	// 没有 keepalive 句柄（首次启动前或句柄已清理），回退到 COM / wsl.exe 检查

	// 尝试通过 COM 获取精确状态
	if (tryGetComService()) {
		IWSLDistribution* dist = nullptr;
		HRESULT hr = com_service_->GetDistributionByName(name.c_str(), &dist);
		if (SUCCEEDED(hr) && dist) {
			DWORD state = 0;
			hr = dist->GetState(&state);
			dist->Release();
			if (SUCCEEDED(hr)) {
				// state: 0=Stopped, 1=Running
				return (state == 1) ? DistroState::RUNNING : DistroState::STOPPED;
			}
		}
	}

	// 回退：通过 wsl.exe -l --running 检查 distro 是否在运行列表中
	std::wstring running_output;
	DWORD ec = runWslExeWithOutput(L"-l --running", running_output);
	if (ec == 0 && running_output.find(name) != std::wstring::npos) {
		return DistroState::RUNNING;
	}
	return DistroState::STOPPED;
}

void* WslVMManager::runCommand(const std::wstring& name, const std::wstring& command)
{
	HANDLE process = INVALID_HANDLE_VALUE;

	// 尝试 COM 路径
	if (tryGetComService()) {
		IWSLDistribution* dist = nullptr;
		HRESULT hr = com_service_->GetDistributionByName(name.c_str(), &dist);
		if (SUCCEEDED(hr) && dist) {
			HANDLE nul = GetStdHandle(STD_INPUT_HANDLE);
			hr = dist->Launch(command.c_str(), FALSE, nul, nul, nul, &process);
			dist->Release();
			if (SUCCEEDED(hr) && process != nullptr) {
				return process;
			}
		}
	}

	// 回退：WslLaunch（wslapi.h）
	HANDLE stdin_h  = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE stdout_h = GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE stderr_h = GetStdHandle(STD_ERROR_HANDLE);
	HRESULT hr = WslLaunch(name.c_str(),
	                       command.c_str(),
	                       FALSE,
	                       stdin_h,
	                       stdout_h,
	                       stderr_h,
	                       &process);
	if (FAILED(hr)) {
		LOG_ERROR("WslLaunch failed (hr=0x{:08x})", static_cast<unsigned>(hr));
		return INVALID_HANDLE_VALUE;
	}
	return process;
}

std::vector<std::wstring> WslVMManager::listDistros()
{
	std::vector<std::wstring> result;

	// 尝试 COM 路径
	if (tryGetComService()) {
		BSTR*  names = nullptr;
		ULONG  count = 0;
		HRESULT hr = com_service_->GetDistributions(&names, &count);
		if (SUCCEEDED(hr) && names) {
			for (ULONG i = 0; i < count; ++i) {
				if (names[i]) {
					result.push_back(names[i]);
					SysFreeString(names[i]);
				}
			}
			CoTaskMemFree(names);
			return result;
		}
	}

	// 回退：解析 wsl.exe --list --quiet 输出
	HANDLE read_pipe  = nullptr;
	HANDLE write_pipe = nullptr;
	SECURITY_ATTRIBUTES sa{};
	sa.nLength              = sizeof(sa);
	sa.bInheritHandle       = TRUE;
	sa.lpSecurityDescriptor = nullptr;

	if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
		LOG_ERROR("CreatePipe failed for listDistros");
		return result;
	}

	STARTUPINFOW si{};
	PROCESS_INFORMATION pi{};
	si.cb         = sizeof(si);
	si.dwFlags    = STARTF_USESTDHANDLES;
	si.hStdOutput = write_pipe;
	si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
	si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

	std::wstring cmd = L"wsl.exe --list --quiet";
	BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
	                         TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	CloseHandle(write_pipe);

	if (ok) {
		WaitForSingleObject(pi.hProcess, INFINITE);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);

		// 读取输出（UTF-16 格式）
		std::vector<wchar_t> buf(4096, L'\0');
		DWORD bytes_read = 0;
		ReadFile(read_pipe, buf.data(), static_cast<DWORD>(buf.size() * sizeof(wchar_t)),
		         &bytes_read, nullptr);
		// 跳过 BOM（如有），按换行分割
		std::wstring output(buf.data(), bytes_read / sizeof(wchar_t));
		std::wistringstream ss(output);
		std::wstring line;
		while (std::getline(ss, line)) {
			if (!line.empty() && line.back() == L'\r') {
				line.pop_back();
			}
			if (!line.empty()) {
				result.push_back(line);
			}
		}
	}
	CloseHandle(read_pipe);
	return result;
}

Status WslVMManager::snapshotDistro(const std::wstring& name,
                                     const std::wstring& snapshot_dir,
                                     const std::wstring& snapshot_name,
                                     std::wstring&        out_path)
{
	LOG_INFO("snapshotDistro: {}", wtoUtf8(name));
	out_path.clear();

	if (!WslIsDistributionRegistered(name.c_str())) {
		LOG_ERROR("distro not registered, cannot snapshot");
		return {Status::NOT_FOUND, "distro not registered"};
	}

	// 导出前先终止，确保文件系统一致
	stopDistro(name);

	// 确保目标目录存在
	CreateDirectoryW(snapshot_dir.c_str(), nullptr);

	const std::wstring base = snapshot_name.empty() ? name : snapshot_name;

	// 优先尝试 VHDX（直接复制虚拟磁盘，速度快，WSL2 2.0+ 支持）
	const std::wstring vhd_path = snapshot_dir + L"\\" + base + L".vhdx";
	std::wstring args = L"--export --vhd \"" + name + L"\" \"" + vhd_path + L"\"";
	if (runWslExe(args) == 0) {
		out_path = vhd_path;
		LOG_INFO("snapshot exported as VHDX");
		return Status::Ok();
	}

	// 回退：tar 包（所有 WSL2 版本均支持）
	const std::wstring tar_path = snapshot_dir + L"\\" + base + L".tar";
	args = L"--export \"" + name + L"\" \"" + tar_path + L"\"";
	if (runWslExe(args) == 0) {
		out_path = tar_path;
		LOG_INFO("snapshot exported as tar");
		return Status::Ok();
	}

	LOG_ERROR("snapshotDistro failed (both VHDX and tar export failed)");
	return {Status::IO_ERROR, "snapshot export failed"};
}

Status WslVMManager::restoreFromSnapshot(const DistroConfig& cfg,
                                          const std::wstring& snapshot_path)
{
	LOG_INFO("restoreFromSnapshot: name={}", wtoUtf8(cfg.name));

	// 先销毁同名 distro（如有）
	if (WslIsDistributionRegistered(cfg.name.c_str())) {
		LOG_INFO("destroying existing distro before restore");
		auto st = destroyDistro(cfg.name);
		if (!st.ok()) {
			return st;
		}
	}

	// 确定安装目录
	std::wstring install_dir = cfg.install_dir;
	if (install_dir.empty()) {
		wchar_t local_app[MAX_PATH]{};
		if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_app))) {
			LOG_ERROR("failed to get LOCALAPPDATA");
			return {Status::IO_ERROR, "failed to get LOCALAPPDATA"};
		}
		install_dir = std::wstring(local_app)
		            + L"\\ClawSpan\\distros\\" + cfg.name;
	}
	// 递归创建目录
	SHCreateDirectoryExW(nullptr, install_dir.c_str(), nullptr);

	// 根据扩展名选择导入方式
	const bool is_vhd = (snapshot_path.size() >= 5 &&
	                     _wcsicmp(snapshot_path.c_str() + snapshot_path.size() - 5,
	                              L".vhdx") == 0);

	std::wstring args;
	if (is_vhd) {
		args = L"--import --vhd \"" + cfg.name + L"\" \""
		     + install_dir + L"\" \"" + snapshot_path + L"\"";
	} else {
		args = L"--import \"" + cfg.name + L"\" \""
		     + install_dir + L"\" \"" + snapshot_path + L"\"";
	}

	const DWORD ec = runWslExe(args);
	last_hr_ = (ec == 0) ? S_OK : HRESULT_FROM_WIN32(ec);

	if (ec != 0) {
		LOG_ERROR("restoreFromSnapshot failed (wsl.exe exit code {})", ec);
		return {Status::IO_ERROR, "snapshot restore failed"};
	}

	LOG_INFO("restoreFromSnapshot succeeded");
	return Status::Ok();
}

std::string WslVMManager::lastDiagnostics() const
{
	// 合并 HRESULT 和 wsl.exe 输出
	std::string result;
	if (last_hr_ != S_OK) {
		char buf[64];
		snprintf(buf, sizeof(buf), "HRESULT=0x%08lx", static_cast<unsigned long>(last_hr_));
		result = buf;
	}
	if (!last_diagnostics_.empty()) {
		if (!result.empty()) { result += "; "; }
		result += last_diagnostics_;
	}
	return result;
}

// ── 工厂函数 ─────────────────────────────────────────────────────────────────

std::unique_ptr<VMManagerInterface> createVMManager()
{
	return std::make_unique<WslVMManager>();
}

} // namespace vmm
} // namespace clawspan
