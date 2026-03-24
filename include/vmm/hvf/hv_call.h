#pragma once

// ─── 平台守卫 ─────────────────────────────────────────────────────────────────
// 本文件是从 macOS VMM 开发阶段保留的遗留代码，依赖 macOS Hypervisor.framework。
// Windows 构建中不会包含此头文件；若意外被引用，通过 #error 提前暴露问题。
#ifdef _WIN32
#  error "hv_call.h is a macOS-only legacy file and must not be included in Windows builds."
#endif
#ifndef __APPLE__
#  error "hv_call.h requires macOS Hypervisor.framework and is only supported on Apple platforms."
#endif

#include "common/types.h"
#include "common/error.h"

#include <Hypervisor/hv_vm.h>
#include <Hypervisor/hv_types.h>
#include <Hypervisor/hv_error.h>

namespace clawspan {

// HvfCall 用于将 HVF 返回码映射到 Result。
//
// 入参:
// - rc:   HVF 返回码。
// - what: 静态错误信息（需保证生命周期）。
//
// 出参/返回:
// - Result<void>::Ok() 或 Result<void>::Error(...)。
inline Result<void> HvfCall(hv_return_t rc, const char* what)
{
	if (rc == HV_SUCCESS) {
		return Result<void>::Ok();
	}
	return Result<void>::Error(Status::INTERNAL_ERROR, what);
}

// HVF_CALL 用于简化 HVF 调用的错误处理。
#define HVF_CALL(expr) ::clawspan::HvfCall((expr), #expr " failed")

} // namespace clawspan

