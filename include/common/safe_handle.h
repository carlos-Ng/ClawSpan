#pragma once

#ifdef _WIN32
#  include <windows.h>
#else
#  error "Platform not supported: add POSIX fd-based implementation using unistd.h here"
#endif

namespace clawspan {

// SafeHandle Windows HANDLE 的 RAII 包装，对应 POSIX SafeFd。
//
// 对 CloseHandle 进行自动配对调用，防止句柄泄漏。
// 不可拷贝，仅支持移动语义。
//
// 使用示例：
//   SafeHandle h(::CreateFile(...));
//   if (!h) { /* 创建失败 */ }
//   ::ReadFile(h.get(), ...);
class SafeHandle
{
public:
	SafeHandle() : handle_(INVALID_HANDLE_VALUE) {}
	explicit SafeHandle(HANDLE h) : handle_(h) {}

	~SafeHandle()
	{
		if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
			::CloseHandle(handle_);
		}
	}

	SafeHandle(const SafeHandle&)            = delete;
	SafeHandle& operator=(const SafeHandle&) = delete;

	SafeHandle(SafeHandle&& other) noexcept : handle_(other.handle_)
	{
		other.handle_ = INVALID_HANDLE_VALUE;
	}

	SafeHandle& operator=(SafeHandle&& other) noexcept
	{
		if (this != &other) {
			if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
				::CloseHandle(handle_);
			}
			handle_       = other.handle_;
			other.handle_ = INVALID_HANDLE_VALUE;
		}
		return *this;
	}

	HANDLE get()     const { return handle_; }
	bool   isValid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
	explicit operator bool() const { return isValid(); }

	// release 放弃所有权并返回裸 HANDLE（调用方负责后续关闭）。
	HANDLE release()
	{
		HANDLE h  = handle_;
		handle_   = INVALID_HANDLE_VALUE;
		return h;
	}

private:
	HANDLE handle_;
};

} // namespace clawspan

