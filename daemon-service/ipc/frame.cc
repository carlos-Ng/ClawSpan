#include "frame.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  error "Platform not supported: add POSIX read/write/arpa/inet.h implementation here"
#endif

namespace clawspan {
namespace ipc {

// ─── 字节序辅助 ───────────────────────────────────────────────────────────────

// hton32 / ntoh32 在没有 arpa/inet.h 的 Windows 上手动实现大端字节序转换。
static uint32_t hton32(uint32_t host)
{
	return ((host & 0x000000FFu) << 24) |
	       ((host & 0x0000FF00u) <<  8) |
	       ((host & 0x00FF0000u) >>  8) |
	       ((host & 0xFF000000u) >> 24);
}

static uint32_t ntoh32(uint32_t net)
{
	return hton32(net); // 对称操作
}

// ─── FrameCodec Windows 实现 ─────────────────────────────────────────────────

#ifdef _WIN32

// readExact 从 Named Pipe HANDLE 精确读取 n 字节，循环处理 ReadFile 的短读情况。
bool FrameCodec::readExact(HANDLE handle, void* buf, size_t n)
{
	auto*    p         = static_cast<uint8_t*>(buf);
	size_t   remaining = n;

	while (remaining > 0) {
		DWORD to_read = static_cast<DWORD>(
		    remaining > 65536 ? 65536 : remaining);
		DWORD bytes_read = 0;
		BOOL  ok         = ::ReadFile(handle, p, to_read, &bytes_read, nullptr);
		if (!ok || bytes_read == 0) {
			return false;
		}
		p         += bytes_read;
		remaining -= bytes_read;
	}
	return true;
}

// writeExact 向 Named Pipe HANDLE 精确写入 n 字节，循环处理 WriteFile 的短写情况。
bool FrameCodec::writeExact(HANDLE handle, const void* buf, size_t n)
{
	const auto* p         = static_cast<const uint8_t*>(buf);
	size_t      remaining = n;

	while (remaining > 0) {
		DWORD to_write    = static_cast<DWORD>(
		    remaining > 65536 ? 65536 : remaining);
		DWORD bytes_written = 0;
		BOOL  ok            = ::WriteFile(handle, p, to_write, &bytes_written, nullptr);
		if (!ok || bytes_written == 0) {
			return false;
		}
		p         += bytes_written;
		remaining -= bytes_written;
	}
	return true;
}

// readFrame 从 Named Pipe HANDLE 中阻塞读取一个完整帧。
Result<std::string> FrameCodec::readFrame(HANDLE handle)
{
	uint32_t net_len = 0;
	if (!readExact(handle, &net_len, sizeof(net_len))) {
		return Result<std::string>::Error(Status::IO_ERROR, "failed to read frame length");
	}
	uint32_t len = ntoh32(net_len);
	if (len == 0) {
		return Result<std::string>::Ok(std::string{});
	}
	if (len > MAX_FRAME_BODY_SIZE) {
		return Result<std::string>::Error(Status::INVALID_ARGUMENT, "frame body exceeds max size");
	}
	std::string data(len, '\0');
	if (!readExact(handle, data.data(), len)) {
		return Result<std::string>::Error(Status::IO_ERROR, "failed to read frame body");
	}
	return Result<std::string>::Ok(std::move(data));
}

// writeFrame 向 Named Pipe HANDLE 写入一个完整帧（含大端字节序的长度前缀）。
Status FrameCodec::writeFrame(HANDLE handle, const std::string& data)
{
	if (data.size() > MAX_FRAME_BODY_SIZE) {
		return Status(Status::INVALID_ARGUMENT, "frame body exceeds max size");
	}
	uint32_t net_len = hton32(static_cast<uint32_t>(data.size()));
	if (!writeExact(handle, &net_len, sizeof(net_len))) {
		return Status(Status::IO_ERROR, "failed to write frame length");
	}
	if (!data.empty() && !writeExact(handle, data.data(), data.size())) {
		return Status(Status::IO_ERROR, "failed to write frame body");
	}
	return Status::Ok();
}

// ─── OVERLAPPED（异步）变体 ────────────────────────────────────────────────
//
// 适用于以 FILE_FLAG_OVERLAPPED 创建的 Named Pipe 句柄。
//
// 核心差异：ReadFile / WriteFile 传入 OVERLAPPED* 而非 nullptr，
// 使得 Windows I/O 子系统为每个操作维护独立的上下文。
// 只要 read 和 write 使用各自的 OVERLAPPED（含各自的 event），
// 两个操作可安全并发在同一 HANDLE 上，不会互相阻塞。

// readExactAsync 使用 OVERLAPPED 模式从 HANDLE 精确读取 n 字节。
bool FrameCodec::readExactAsync(HANDLE handle, void* buf, size_t n, HANDLE event)
{
	auto*  p         = static_cast<uint8_t*>(buf);
	size_t remaining = n;

	while (remaining > 0) {
		DWORD to_read = static_cast<DWORD>(remaining > 65536 ? 65536 : remaining);

		OVERLAPPED ov = {};
		ov.hEvent     = event;
		::ResetEvent(event);

		DWORD bytes_read = 0;
		BOOL  ok = ::ReadFile(handle, p, to_read, &bytes_read, &ov);

		if (!ok) {
			DWORD err = ::GetLastError();
			if (err == ERROR_IO_PENDING) {
				if (!::GetOverlappedResult(handle, &ov, &bytes_read, TRUE)) {
					return false;
				}
			} else {
				return false;
			}
		}

		if (bytes_read == 0) {
			return false;
		}
		p         += bytes_read;
		remaining -= bytes_read;
	}
	return true;
}

// writeExactAsync 使用 OVERLAPPED 模式向 HANDLE 精确写入 n 字节。
bool FrameCodec::writeExactAsync(HANDLE handle, const void* buf, size_t n, HANDLE event)
{
	const auto* p         = static_cast<const uint8_t*>(buf);
	size_t      remaining = n;

	while (remaining > 0) {
		DWORD to_write = static_cast<DWORD>(remaining > 65536 ? 65536 : remaining);

		OVERLAPPED ov = {};
		ov.hEvent     = event;
		::ResetEvent(event);

		DWORD bytes_written = 0;
		BOOL  ok = ::WriteFile(handle, p, to_write, &bytes_written, &ov);

		if (!ok) {
			DWORD err = ::GetLastError();
			if (err == ERROR_IO_PENDING) {
				if (!::GetOverlappedResult(handle, &ov, &bytes_written, TRUE)) {
					return false;
				}
			} else {
				return false;
			}
		}

		if (bytes_written == 0) {
			return false;
		}
		p         += bytes_written;
		remaining -= bytes_written;
	}
	return true;
}

// readFrameAsync 从 OVERLAPPED 模式的 HANDLE 中阻塞读取一个完整帧。
Result<std::string> FrameCodec::readFrameAsync(HANDLE handle, HANDLE event)
{
	uint32_t net_len = 0;
	if (!readExactAsync(handle, &net_len, sizeof(net_len), event)) {
		return Result<std::string>::Error(Status::IO_ERROR, "failed to read frame length");
	}
	uint32_t len = ntoh32(net_len);
	if (len == 0) {
		return Result<std::string>::Ok(std::string{});
	}
	if (len > MAX_FRAME_BODY_SIZE) {
		return Result<std::string>::Error(Status::INVALID_ARGUMENT, "frame body exceeds max size");
	}
	std::string data(len, '\0');
	if (!readExactAsync(handle, data.data(), len, event)) {
		return Result<std::string>::Error(Status::IO_ERROR, "failed to read frame body");
	}
	return Result<std::string>::Ok(std::move(data));
}

// writeFrameAsync 向 OVERLAPPED 模式的 HANDLE 写入一个完整帧。
Status FrameCodec::writeFrameAsync(HANDLE handle, const std::string& data, HANDLE event)
{
	if (data.size() > MAX_FRAME_BODY_SIZE) {
		return Status(Status::INVALID_ARGUMENT, "frame body exceeds max size");
	}
	uint32_t net_len = hton32(static_cast<uint32_t>(data.size()));
	if (!writeExactAsync(handle, &net_len, sizeof(net_len), event)) {
		return Status(Status::IO_ERROR, "failed to write frame length");
	}
	if (!data.empty() && !writeExactAsync(handle, data.data(), data.size(), event)) {
		return Status(Status::IO_ERROR, "failed to write frame body");
	}
	return Status::Ok();
}

#else
#  error "Platform not supported: add POSIX read/write implementation here"
#endif

} // namespace ipc
} // namespace clawspan
