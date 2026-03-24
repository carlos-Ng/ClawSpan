#pragma once

// vm_manager.h — WSL2 VM 生命周期管理接口
//
// 通过 WSL2 COM API（IWSLService / IWSLDistribution）或 wslapi.h 编程式管理 distro：
// 创建（从 Ubuntu rootfs tarball 导入）、启动、停止、快照、恢复、销毁。
//
// 使用方式：
//   auto mgr = clawspan::vmm::createVMManager();
//   mgr->createDistro(cfg);
//   mgr->startDistro(L"agent-enclave-vm");

#include "common/status.h"

#include <memory>
#include <string>
#include <vector>

namespace clawspan {
namespace vmm {

// ── DistroConfig ─────────────────────────────────────────────────────────────

// DistroConfig 描述创建一个 WSL2 distro 所需的全部参数。
struct DistroConfig
{
	// name: distro 唯一标识名称（如 "agent-enclave-vm"）
	std::wstring name;

	// rootfs_path: Ubuntu rootfs .tar.gz 文件的绝对路径
	std::wstring rootfs_path;

	// install_dir: distro 虚拟磁盘的安装目录（空字符串由 WSL 自动管理）
	std::wstring install_dir;

	// kernel_path: 自定义内核镜像路径（空字符串使用默认内核）
	// 写入 %USERPROFILE%\.wslconfig [wsl2] kernel= 字段
	std::wstring kernel_path;

	// default_uid: distro 内默认用户 UID（通常为 1000）
	uint32_t default_uid = 1000;

	// bootstrap_cmd: distro 首次启动后自动执行的初始化命令（空字符串不执行）
	// 例: L"/opt/enclave/bootstrap/setup.sh"
	std::wstring bootstrap_cmd;
};

// ── DistroState ───────────────────────────────────────────────────────────────

// DistroState 描述 WSL2 distro 的当前生命周期状态。
enum class DistroState
{
	NOT_REGISTERED, // distro 未在 WSL 中注册
	STOPPED,        // 已注册但当前未运行
	RUNNING,        // 正在运行（WSL2 虚拟机已启动）
};

// ── VMManagerInterface ────────────────────────────────────────────────────────

// VMManagerInterface 定义 WSL2 VM 生命周期管理的统一接口。
// 通过 createVMManager() 工厂函数获取实现实例。
//
// 所有宽字符参数（wstring）使用 Windows 原生 UTF-16 LE 编码，
// 与 WSL2 API / CreateProcessW 保持一致。
class VMManagerInterface
{
public:
	virtual ~VMManagerInterface() = default;

	// createDistro 从 rootfs tarball 导入并注册 WSL2 distro。
	// 优先使用 wsl.exe --import；COM 和 wslapi.h 作为备用路径。
	// 若指定 kernel_path，写入 %USERPROFILE%\.wslconfig [wsl2] kernel=。
	// 若指定 bootstrap_cmd，在 distro 首次启动后异步执行。
	//
	// 入参:
	// - config: distro 配置
	//
	// 出参/返回:
	// - Status::Ok():               成功创建
	// - Status(ALREADY_EXISTS):     同名 distro 已注册
	// - Status(IO_ERROR):           导入失败（wsl.exe 错误码或 COM 失败）
	// - Status(INTERNAL_ERROR):     .wslconfig 写入失败
	virtual Status createDistro(const DistroConfig& config) = 0;

	// startDistro 触发指定 distro 启动（若已运行则无操作）。
	//
	// 入参:
	// - name: distro 名称
	//
	// 出参/返回:
	// - Status::Ok():       已成功启动或原本已在运行
	// - Status(NOT_FOUND):  distro 未注册
	// - Status(IO_ERROR):   启动失败
	virtual Status startDistro(const std::wstring& name) = 0;

	// stopDistro 终止指定 distro 的所有进程并关闭 WSL2 VM。
	//
	// 入参:
	// - name: distro 名称
	//
	// 出参/返回:
	// - Status::Ok():      成功终止或已经处于停止状态
	// - Status(IO_ERROR):  终止失败
	virtual Status stopDistro(const std::wstring& name) = 0;

	// destroyDistro 注销并删除指定 distro（包括虚拟磁盘）。
	// 调用前应先 stopDistro。
	//
	// 入参:
	// - name: distro 名称
	//
	// 出参/返回:
	// - Status::Ok():      成功注销（或 distro 原本不存在）
	// - Status(IO_ERROR):  注销失败
	virtual Status destroyDistro(const std::wstring& name) = 0;

	// getDistroState 查询指定 distro 的生命周期状态。
	//
	// 入参:
	// - name: distro 名称
	//
	// 出参/返回: DistroState 枚举值
	virtual DistroState getDistroState(const std::wstring& name) = 0;

	// runCommand 在指定 distro 内异步执行命令，返回 Windows 进程句柄。
	// 调用方负责 CloseHandle（返回非 INVALID_HANDLE_VALUE 时）。
	//
	// 入参:
	// - name:    distro 名称
	// - command: 在 distro 内执行的 shell 命令字符串（传给 /bin/bash -c）
	//
	// 出参/返回:
	// - 非 INVALID_HANDLE_VALUE: 成功，返回子进程句柄
	// - INVALID_HANDLE_VALUE:    失败
	virtual void* runCommand(const std::wstring& name,
	                         const std::wstring& command) = 0;

	// listDistros 返回当前所有已注册的 WSL2 distro 名称列表。
	//
	// 出参/返回: distro 名称列表（可能为空）
	virtual std::vector<std::wstring> listDistros() = 0;

	// snapshotDistro 将 distro 导出为快照文件（优先 VHDX，回退 .tar）。
	// 导出前自动终止 distro，导出后 distro 保持注册状态。
	//
	// 入参:
	// - name:          distro 名称
	// - snapshot_dir:  快照存储目录（Windows 绝对路径，不存在时自动创建）
	// - snapshot_name: 快照文件名（不含扩展名）；空字符串时使用 distro 名称
	//
	// 出参/返回:
	// - Status::Ok() + result 含快照文件完整路径（.vhdx 或 .tar）
	// - Status(IO_ERROR): 导出失败
	virtual Status snapshotDistro(const std::wstring& name,
	                              const std::wstring& snapshot_dir,
	                              const std::wstring& snapshot_name,
	                              std::wstring&        out_path) = 0;

	// restoreFromSnapshot 从快照文件恢复 distro。
	// 若同名 distro 已存在则先销毁再导入，导入后不自动启动。
	//
	// 入参:
	// - cfg:           distro 配置（使用 name 和 install_dir）
	// - snapshot_path: 快照文件完整路径（.vhdx → --import --vhd；其余 → --import）
	//
	// 出参/返回:
	// - Status::Ok():     成功恢复
	// - Status(IO_ERROR): 失败（文件不存在、wsl.exe 报错等）
	virtual Status restoreFromSnapshot(const DistroConfig& cfg,
	                                   const std::wstring& snapshot_path) = 0;

	// lastDiagnostics 返回最近一次 wsl.exe 调用的 stderr/stdout 输出。
	// 在 createDistro 或 restoreFromSnapshot 失败后可据此排查原因。
	//
	// 出参/返回: 诊断文本（UTF-8），成功时可能为空
	virtual std::string lastDiagnostics() const = 0;
};

// createVMManager 工厂函数，返回平台相关的 VMManager 实现实例。
//
// 出参/返回:
// - 非 nullptr: 成功，返回 WslVMManager 实例
// - nullptr:    不在 Windows 平台（不应发生）
std::unique_ptr<VMManagerInterface> createVMManager();

} // namespace vmm
} // namespace clawspan
