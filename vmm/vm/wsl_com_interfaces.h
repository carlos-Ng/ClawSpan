#pragma once

// wsl_com_interfaces.h — WSL2 内部 COM 接口声明
//
// WSL2 通过 wslservice.exe 暴露 COM 接口用于编程式 distro 管理。
// 这些接口在 Windows 11 SDK (22621+) 的 wslservice.h 中定义；
// 此处提供向后兼容的手工声明，在旧 SDK 版本下也可编译。
//
// 参考：https://learn.microsoft.com/en-us/windows/wsl/wsl-config
// CLSID/IID 来自 Windows SDK wslservice.h（仅用于 Windows 11 22H2+）

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>
#include <winerror.h>

namespace clawspan {
namespace vmm {

// ── WSL2 COM CLSID / IID ──────────────────────────────────────────────────

// CLSID_WslService — wslservice.exe 暴露的 WSL 服务 COM 对象
// {B7461D4B-9674-4107-9B5E-F8EC71B74BEC}
static const CLSID CLSID_WslService = {
	0xB7461D4B,
	0x9674,
	0x4107,
	{0x9B, 0x5E, 0xF8, 0xEC, 0x71, 0xB7, 0x4B, 0xEC}
};

// IID_IWSLService — IWSLService 接口 GUID
// {55BF5B9B-F46E-4CDB-9BC5-EC21B4640F85}
static const IID IID_IWSLService = {
	0x55BF5B9B,
	0xF46E,
	0x4CDB,
	{0x9B, 0xC5, 0xEC, 0x21, 0xB4, 0x64, 0x0F, 0x85}
};

// IID_IWSLDistribution — IWSLDistribution 接口 GUID
// {F7F19D70-A4E4-4D11-9C5C-8A2D8B3B0AC4}
static const IID IID_IWSLDistribution = {
	0xF7F19D70,
	0xA4E4,
	0x4D11,
	{0x9C, 0x5C, 0x8A, 0x2D, 0x8B, 0x3B, 0x0A, 0xC4}
};

// ── WSL2 Distribution Flags ──────────────────────────────────────────────────

// WslDistributionFlags — WSL2 发行版配置标志位
// 与 wslapi.h 中的 WSL_DISTRIBUTION_FLAGS 保持一致
enum WslDistributionFlags : ULONG
{
	WSL_DISTRIBUTION_FLAGS_NONE                 = 0x0,
	WSL_DISTRIBUTION_FLAGS_ENABLE_INTEROP       = 0x1,
	WSL_DISTRIBUTION_FLAGS_APPEND_NT_PATH       = 0x2,
	WSL_DISTRIBUTION_FLAGS_ENABLE_DRIVE_MOUNTING = 0x4,
};

// ── IWSLDistribution COM 接口 ─────────────────────────────────────────────

// IWSLDistribution 提供单个 WSL2 distro 的操作接口。
// 通过 IWSLService::GetDistributionByName() 获取。
MIDL_INTERFACE("F7F19D70-A4E4-4D11-9C5C-8A2D8B3B0AC4")
IWSLDistribution : public IUnknown
{
public:
	// GetName 返回 distro 的注册名称（调用方负责 SysFreeString）
	virtual HRESULT STDMETHODCALLTYPE GetName(BSTR* name) = 0;

	// GetId 返回 distro 的唯一 GUID
	virtual HRESULT STDMETHODCALLTYPE GetId(GUID* id) = 0;

	// Launch 在 distro 内启动进程
	//
	// command:                  要执行的命令字符串（传给 /bin/sh -c）
	// useCurrentWorkingDir:     是否使用当前工作目录
	// stdIn/stdOut/stdErr:      进程标准 I/O 句柄
	// process:                  输出参数，进程句柄（调用方负责 CloseHandle）
	virtual HRESULT STDMETHODCALLTYPE Launch(PCWSTR  command,
	                                         BOOL    useCurrentWorkingDir,
	                                         HANDLE  stdIn,
	                                         HANDLE  stdOut,
	                                         HANDLE  stdErr,
	                                         HANDLE* process) = 0;

	// Terminate 终止 distro 的所有进程
	virtual HRESULT STDMETHODCALLTYPE Terminate() = 0;

	// GetState 返回 distro 当前状态（0=Stopped, 1=Running, 2=Installing）
	virtual HRESULT STDMETHODCALLTYPE GetState(DWORD* state) = 0;

	// Configure 更新 distro 的 UID 和标志
	virtual HRESULT STDMETHODCALLTYPE Configure(ULONG               defaultUID,
	                                             WslDistributionFlags flags) = 0;
};

// ── IWSLService COM 接口 ──────────────────────────────────────────────────

// IWSLService 提供 WSL2 服务级别的操作：注册/获取/删除 distro。
// 通过 CoCreateInstance(CLSID_WslService, ...) 获取。
MIDL_INTERFACE("55BF5B9B-F46E-4CDB-9BC5-EC21B4640F85")
IWSLService : public IUnknown
{
public:
	// RegisterDistribution 从 rootfs tarball 注册新 distro
	//
	// name:         新 distro 的唯一名称
	// tarGzFilename: rootfs .tar.gz 文件的绝对路径
	// defaultUID:   默认用户 UID
	// flags:        配置标志（见 WslDistributionFlags）
	// errorMessage: 失败时写入错误描述（调用方负责 SysFreeString，可为 nullptr）
	virtual HRESULT STDMETHODCALLTYPE RegisterDistribution(PCWSTR               name,
	                                                       PCWSTR               tarGzFilename,
	                                                       ULONG                defaultUID,
	                                                       WslDistributionFlags flags,
	                                                       BSTR*                errorMessage) = 0;

	// GetDistributionByName 查找已注册的 distro
	//
	// name:         目标 distro 名称
	// distribution: 成功时写入 IWSLDistribution 接口指针
	virtual HRESULT STDMETHODCALLTYPE GetDistributionByName(PCWSTR             name,
	                                                        IWSLDistribution** distribution) = 0;

	// UnregisterDistribution 注销并删除 distro（含虚拟磁盘）
	virtual HRESULT STDMETHODCALLTYPE UnregisterDistribution(PCWSTR name) = 0;

	// GetDistributions 枚举所有已注册 distro（返回名称数组，调用方释放）
	virtual HRESULT STDMETHODCALLTYPE GetDistributions(BSTR** names, ULONG* count) = 0;
};

} // namespace vmm
} // namespace clawspan
