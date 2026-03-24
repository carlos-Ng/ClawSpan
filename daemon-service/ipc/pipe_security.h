#pragma once

#ifdef _WIN32

#include "common/log.h"

#include <string>
#include <vector>
#include <windows.h>
#include <sddl.h>

namespace clawspan {
namespace ipc {

// PipeSecurityContext 为 Named Pipe 构造仅允许当前用户访问的 DACL。
//
// 背景：CreateNamedPipeA 安全属性为 nullptr 时使用进程默认 DACL，
// 允许任意本地进程（包括其他用户或低权限进程）连接，对处理 UI 自动化的
// 代理服务存在权限提升攻击面。
//
// 本结构通过 SDDL 构建当前用户专属 DACL：
//   D:P(A;;GA;;;{current_user_sid})
//   D:P  = 受保护 DACL，不继承父对象权限
//   A    = Access Allowed ACE
//   GA   = GENERIC_ALL（通用完全权限）
//   ;;;{sid} = 仅授予当前登录用户
//
// RAII 设计：析构时自动释放 LocalAlloc 分配的安全描述符。
struct PipeSecurityContext
{
	SECURITY_ATTRIBUTES  sa{};
	PSECURITY_DESCRIPTOR psd = nullptr;

	PipeSecurityContext() noexcept = default;

	~PipeSecurityContext()
	{
		if (psd) {
			::LocalFree(psd);
			psd = nullptr;
		}
	}

	PipeSecurityContext(const PipeSecurityContext&)            = delete;
	PipeSecurityContext& operator=(const PipeSecurityContext&) = delete;

	// init 读取当前进程用户 SID，通过 SDDL 构建安全描述符。
	// 成功返回 true；失败时 ptr() 返回 nullptr，CreateNamedPipeA 将使用系统默认。
	bool init()
	{
		// 获取进程访问令牌
		HANDLE token = nullptr;
		if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
			return false;
		}

		// 查询 TokenUser 所需缓冲区大小并读取
		DWORD len = 0;
		::GetTokenInformation(token, TokenUser, nullptr, 0, &len);
		std::vector<BYTE> buf(len);
		bool ok = !!::GetTokenInformation(token, TokenUser, buf.data(), len, &len);
		::CloseHandle(token);
		if (!ok || buf.empty()) {
			return false;
		}

		PSID user_sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;

		// 将 SID 转为字符串形式，拼装 SDDL
		LPSTR sid_str = nullptr;
		if (!::ConvertSidToStringSidA(user_sid, &sid_str)) {
			return false;
		}
		std::string sddl = std::string("D:P(A;;GA;;;") + sid_str + ")";
		::LocalFree(sid_str);

		// 从 SDDL 构建安全描述符（LocalAlloc 分配，析构时释放）
		if (!::ConvertStringSecurityDescriptorToSecurityDescriptorA(
		        sddl.c_str(), SDDL_REVISION_1, &psd, nullptr)) {
			psd = nullptr;
			return false;
		}

		sa.nLength              = sizeof(SECURITY_ATTRIBUTES);
		sa.lpSecurityDescriptor = psd;
		sa.bInheritHandle       = FALSE;
		return true;
	}

	// ptr 返回指向 SECURITY_ATTRIBUTES 的指针。
	// init() 失败时返回 nullptr，CreateNamedPipeA 将退化为系统默认安全属性。
	SECURITY_ATTRIBUTES* ptr() noexcept { return psd ? &sa : nullptr; }
};

} // namespace ipc
} // namespace clawspan

#endif // _WIN32
