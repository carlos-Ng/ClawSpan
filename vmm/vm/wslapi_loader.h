#pragma once

// wslapi_loader.h — wslapi.dll 动态加载封装
//
// Windows SDK 的 wslapi.h 声明了函数但不提供导入库（.lib）。
// 此头文件通过 LoadLibrary / GetProcAddress 在运行时加载这些函数。
// 若机器未安装 WSL 或 DLL 不存在，所有函数指针为 nullptr，调用方需检查。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>

namespace clawspan {
namespace vmm {

// ── 函数指针类型 ─────────────────────────────────────────────────────────────

using WslIsDistributionRegistered_t = BOOL(WINAPI*)(PCWSTR distributionName);

using WslRegisterDistribution_t = HRESULT(WINAPI*)(PCWSTR distributionName,
                                                     PCWSTR tarGzFilename);

using WslConfigureDistribution_t = HRESULT(WINAPI*)(PCWSTR distributionName,
                                                      ULONG defaultUID,
                                                      int distributionFlags);

using WslLaunch_t = HRESULT(WINAPI*)(PCWSTR distributionName,
                                      PCWSTR command,
                                      BOOL   useCurrentWorkingDirectory,
                                      HANDLE stdIn,
                                      HANDLE stdOut,
                                      HANDLE stdErr,
                                      HANDLE* process);

// ── WslApi 单例 ─────────────────────────────────────────────────────────────

struct WslApi
{
	HMODULE                         module = nullptr;
	WslIsDistributionRegistered_t   IsDistributionRegistered = nullptr;
	WslRegisterDistribution_t       RegisterDistribution     = nullptr;
	WslConfigureDistribution_t      ConfigureDistribution    = nullptr;
	WslLaunch_t                     Launch                   = nullptr;

	static WslApi& instance()
	{
		static WslApi api;
		return api;
	}

	bool available() const { return module != nullptr; }

private:
	WslApi()
	{
		module = ::LoadLibraryW(L"wslapi.dll");
		if (!module) { return; }

		IsDistributionRegistered = reinterpret_cast<WslIsDistributionRegistered_t>(
			::GetProcAddress(module, "WslIsDistributionRegistered"));
		RegisterDistribution = reinterpret_cast<WslRegisterDistribution_t>(
			::GetProcAddress(module, "WslRegisterDistribution"));
		ConfigureDistribution = reinterpret_cast<WslConfigureDistribution_t>(
			::GetProcAddress(module, "WslConfigureDistribution"));
		Launch = reinterpret_cast<WslLaunch_t>(
			::GetProcAddress(module, "WslLaunch"));
	}

	~WslApi()
	{
		if (module) {
			::FreeLibrary(module);
			module = nullptr;
		}
	}

	WslApi(const WslApi&) = delete;
	WslApi& operator=(const WslApi&) = delete;
};

} // namespace vmm
} // namespace clawspan

// ── 内联替换宏 ─────────────────────────────────────────────────────────────
// 让已有的 WslIsDistributionRegistered(...) 等裸调用不报链接错误。
// 若 DLL 不可用，行为等价于返回失败。

inline BOOL WslIsDistributionRegistered(PCWSTR name)
{
	auto& api = clawspan::vmm::WslApi::instance();
	return api.IsDistributionRegistered ? api.IsDistributionRegistered(name) : FALSE;
}

inline HRESULT WslRegisterDistribution(PCWSTR name, PCWSTR tarGz)
{
	auto& api = clawspan::vmm::WslApi::instance();
	return api.RegisterDistribution ? api.RegisterDistribution(name, tarGz)
	                                : E_NOTIMPL;
}

inline HRESULT WslConfigureDistribution(PCWSTR name, ULONG uid, int flags)
{
	auto& api = clawspan::vmm::WslApi::instance();
	return api.ConfigureDistribution ? api.ConfigureDistribution(name, uid, flags)
	                                 : E_NOTIMPL;
}

inline HRESULT WslLaunch(PCWSTR name, PCWSTR cmd, BOOL useCwd,
                          HANDLE stdIn, HANDLE stdOut, HANDLE stdErr,
                          HANDLE* process)
{
	auto& api = clawspan::vmm::WslApi::instance();
	return api.Launch ? api.Launch(name, cmd, useCwd, stdIn, stdOut, stdErr, process)
	                  : E_NOTIMPL;
}
