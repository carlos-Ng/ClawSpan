#pragma once

#include "common/types.h"

#include <string>
#include <utility>

namespace clawspan {

// Status 携带操作结果码与可选的附加说明。
//
// 两种消息模式：
//   - 静态字符串（默认）：message 指向字面量或 statusMessage() 返回值，零分配。
//   - 动态字符串（带 reason 的安全/确认拒绝等场景）：内部持有 std::string，
//     message 指向其 c_str()，生命周期与 Status 对象绑定，无需外部 thread_local。
struct Status {
	enum Code : i32 {
		OK = 0,

		// ── 通用 ──────────────────────────────────────────────────────────────────
		UNKNOWN,
		INVALID_ARGUMENT,
		NOT_SUPPORTED,
		OUT_OF_RANGE,
		IO_ERROR,
		INTERNAL_ERROR,

		// ── VMM / Hypervisor ──────────────────────────────────────────────────────
		ADDRESS_CONFLICT,
		INVALID_ADDRESS,
		DEVICE_NOT_FOUND,
		DEVICE_NOT_READY,
		HYPERVISOR_ERROR,
		VCPU_SHUTDOWN,
		VCPU_INTERRUPTED,

		// ── Accessibility / GUI capability ────────────────────────────────────────
		ACCESSIBILITY_DENIED,  // OS 辅助功能权限未授予（需用户在系统设置中开启）
		ELEMENT_NOT_FOUND,     // element_path 无法定位到对应元素
		WINDOW_NOT_FOUND,      // window_id 对应的窗口不存在或已关闭

		// ── Security / Hook ───────────────────────────────────────────────────────
		OPERATION_DENIED,      // 安全策略（pre-hook）自动拒绝，无需用户交互
		CONFIRM_TIMEOUT,       // 用户确认弹窗等待超时
		CONFIRM_DENIED,        // 用户在确认弹窗中明确拒绝
		CONFIRM_REQUIRED,      // 需要用户确认（Phase 1 占位，Phase 2 实现弹窗通道）

		// ── Core / Module ─────────────────────────────────────────────────────────
		CONFIG_PARSE_ERROR,    // 配置文件解析失败（toml 格式错误或文件不可读）
		NOT_INITIALIZED,       // 服务或模块未初始化即被调用
		CAPABILITY_NOT_FOUND,  // 请求的能力模块未注册或未加载
		MODULE_LOAD_ERROR,     // 动态库加载失败（dlopen/dlsym 错误）

		// ── 通用资源 ─────────────────────────────────────────────────────────────
		ALREADY_EXISTS,        // 同名资源已存在（如 WSL distro 重复创建）
		NOT_FOUND,             // 指定资源不存在
	};

	Code        code;
	const char* message;  // 始终有效；静态路径指向字面量，动态路径指向 message_storage_

	// 静态消息构造（零堆分配，适用于常量字符串）。
	// 注：Status 含 std::string 成员，无法标记 constexpr（std::string 非字面类型）。
	Status(Code in_code = OK, const char* in_message = "") noexcept
	    : code(in_code), message(in_message)
	{}

	// 动态消息构造（接管 std::string，message 指向内部 c_str()）。
	// 用于安全拒绝 reason、确认拒绝等需要携带运行时字符串的场景，
	// 取代了原来依赖 thread_local 缓冲区保持 message 指针有效的脆弱方案。
	Status(Code in_code, std::string dynamic_message)
	    : code(in_code)
	    , message_storage_(std::move(dynamic_message))
	{
		message = message_storage_.empty()
		              ? ""
		              : message_storage_.c_str();
	}

	// 拷贝构造：确保 message 指向本对象内部存储，而非源对象。
	Status(const Status& other)
	    : code(other.code)
	    , message_storage_(other.message_storage_)
	{
		if (!message_storage_.empty()) {
			message = message_storage_.c_str();
		} else {
			message = other.message;
		}
	}

	// 移动构造：接管动态存储，message 随之更新。
	Status(Status&& other) noexcept
	    : code(other.code)
	    , message(other.message)
	    , message_storage_(std::move(other.message_storage_))
	{
		if (!message_storage_.empty()) {
			message = message_storage_.c_str();
		}
	}

	Status& operator=(const Status& other)
	{
		if (this != &other) {
			code             = other.code;
			message_storage_ = other.message_storage_;
			if (!message_storage_.empty()) {
				message = message_storage_.c_str();
			} else {
				message = other.message;
			}
		}
		return *this;
	}

	Status& operator=(Status&& other) noexcept
	{
		if (this != &other) {
			code             = other.code;
			message_storage_ = std::move(other.message_storage_);
			if (!message_storage_.empty()) {
				message = message_storage_.c_str();
			} else {
				message = other.message;
			}
		}
		return *this;
	}

	static Status Ok() noexcept { return Status(OK, ""); }

	bool ok() const noexcept { return code == OK; }

private:
	std::string message_storage_;
};

// statusMessage 将错误码映射为静态描述字符串。
//
// 入参:
// - code: 错误码。
//
// 出参/返回:
// - 对应的静态错误描述字符串。
constexpr const char* statusMessage(Status::Code code) noexcept
{
	switch (code) {
	case Status::OK:                   return "ok";
	case Status::UNKNOWN:              return "unknown";
	case Status::INVALID_ARGUMENT:     return "invalid argument";
	case Status::NOT_SUPPORTED:        return "not supported";
	case Status::OUT_OF_RANGE:         return "out of range";
	case Status::IO_ERROR:             return "io error";
	case Status::INTERNAL_ERROR:       return "internal error";
	case Status::ADDRESS_CONFLICT:     return "address conflict";
	case Status::INVALID_ADDRESS:      return "invalid address";
	case Status::DEVICE_NOT_FOUND:     return "device not found";
	case Status::DEVICE_NOT_READY:     return "device not ready";
	case Status::HYPERVISOR_ERROR:     return "hypervisor error";
	case Status::VCPU_SHUTDOWN:        return "vcpu shutdown";
	case Status::VCPU_INTERRUPTED:     return "vcpu interrupted";
	case Status::ACCESSIBILITY_DENIED: return "accessibility permission denied";
	case Status::ELEMENT_NOT_FOUND:    return "element not found";
	case Status::WINDOW_NOT_FOUND:     return "window not found";
	case Status::OPERATION_DENIED:     return "operation denied by security policy";
	case Status::CONFIRM_TIMEOUT:      return "user confirmation timed out";
	case Status::CONFIRM_DENIED:       return "user denied the operation";
	case Status::CONFIRM_REQUIRED:     return "operation requires user confirmation";
	case Status::CONFIG_PARSE_ERROR:   return "configuration file parse error";
	case Status::NOT_INITIALIZED:      return "not initialized";
	case Status::CAPABILITY_NOT_FOUND: return "capability not found";
	case Status::MODULE_LOAD_ERROR:    return "module load error";
	case Status::ALREADY_EXISTS:       return "already exists";
	case Status::NOT_FOUND:            return "not found";
	}
	return "unknown";
}

} // namespace clawspan
