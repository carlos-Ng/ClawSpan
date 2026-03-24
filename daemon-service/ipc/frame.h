#pragma once

#include "common/error.h"

#include <cstdint>
#include <cstddef>
#include <string>

#ifdef _WIN32
#  include <windows.h>
#else
#  error "Platform not supported: add POSIX fd-based declarations here"
#endif

namespace clawspan {
namespace ipc {

// FrameCodec 提供基于 Length-prefix 的帧编解码功能（仅供 ipc 模块内部使用）。
//
// 帧格式：[uint32_t len（大端字节序，4 字节）][len 字节的 UTF-8 数据]
// 最大帧长：MAX_FRAME_BODY_SIZE 字节，超出视为非法，readFrame 返回错误。
constexpr size_t MAX_FRAME_BODY_SIZE = 4 * 1024 * 1024; // 4 MiB

class FrameCodec
{
public:
	// readFrame 从 Named Pipe 句柄中阻塞读取一个完整帧。
	//
	// 入参:
	// - handle: 已连接的 Named Pipe HANDLE。
	//
	// 出参/返回:
	// - Result::Ok(string)：成功，返回帧体数据（UTF-8 字符串）。
	// - Result::Error(IO_ERROR)：读取失败或对端已关闭连接。
	// - Result::Error(INVALID_ARGUMENT)：帧长超过 MAX_FRAME_BODY_SIZE。
#ifdef _WIN32
	static Result<std::string> readFrame(HANDLE handle);
#else
#  error "Platform not supported"
#endif

	// writeFrame 向 Named Pipe 句柄写入一个完整帧（含长度前缀）。
	//
	// 入参:
	// - handle: 已连接的 Named Pipe HANDLE。
	// - data:   待写入的帧体数据，长度不得超过 MAX_FRAME_BODY_SIZE。
	//
	// 出参/返回:
	// - Status::Ok()：写入成功。
	// - Status(IO_ERROR)：写入失败。
	// - Status(INVALID_ARGUMENT)：data.size() 超过 MAX_FRAME_BODY_SIZE。
#ifdef _WIN32
	static Status writeFrame(HANDLE handle, const std::string& data);
#else
#  error "Platform not supported"
#endif

	// ── OVERLAPPED（异步）变体 ───────────────────────────────────────────
	//
	// 适用于以 FILE_FLAG_OVERLAPPED 创建的 Named Pipe 句柄。
	//
	// 典型场景：一个线程阻塞在 readFrameAsync 等待对端数据，另一个线程
	// 通过 writeFrameAsync 向同一 HANDLE 发送数据——两者各自使用独立的
	// event，互不阻塞（Windows 同步管道无法做到这一点）。
	//
	// event 参数要求：
	// - 必须是有效的手动重置事件（CreateEvent(NULL, TRUE, FALSE, NULL)）。
	// - 同一 event 不得被多个并发 I/O 操作共享。读/写各用自己的 event。
	// - 如果多个线程同时调用 writeFrameAsync，调用方须自行加锁序列化并
	//   共享同一个 event（锁保证同一时刻只有一个写操作使用该 event）。

	// readFrameAsync 从 OVERLAPPED 模式的 HANDLE 阻塞读取一个完整帧。
	//
	// 入参:
	// - handle: 以 FILE_FLAG_OVERLAPPED 创建的 Named Pipe HANDLE。
	// - event:  调用方专用的手动重置事件（可跨帧复用，不可跨线程共享）。
	//
	// 出参/返回: 语义与 readFrame 相同。
#ifdef _WIN32
	static Result<std::string> readFrameAsync(HANDLE handle, HANDLE event);
#else
#  error "Platform not supported"
#endif

	// writeFrameAsync 向 OVERLAPPED 模式的 HANDLE 写入一个完整帧。
	//
	// 入参:
	// - handle: 以 FILE_FLAG_OVERLAPPED 创建的 Named Pipe HANDLE。
	// - data:   待写入的帧体数据，长度不得超过 MAX_FRAME_BODY_SIZE。
	// - event:  调用方专用的手动重置事件（多写者场景须外部加锁序列化）。
	//
	// 出参/返回: 语义与 writeFrame 相同。
#ifdef _WIN32
	static Status writeFrameAsync(HANDLE handle, const std::string& data, HANDLE event);
#else
#  error "Platform not supported"
#endif

private:
	// readExact 从 HANDLE 精确读取 n 字节，循环处理 ReadFile 的短读情况。
#ifdef _WIN32
	static bool readExact(HANDLE handle, void* buf, size_t n);

	// writeExact 向 HANDLE 精确写入 n 字节，循环处理 WriteFile 的短写情况。
	static bool writeExact(HANDLE handle, const void* buf, size_t n);

	// readExactAsync 使用 OVERLAPPED 模式从 HANDLE 精确读取 n 字节。
	static bool readExactAsync(HANDLE handle, void* buf, size_t n, HANDLE event);

	// writeExactAsync 使用 OVERLAPPED 模式向 HANDLE 精确写入 n 字节。
	static bool writeExactAsync(HANDLE handle, const void* buf, size_t n, HANDLE event);
#else
#  error "Platform not supported"
#endif
};

} // namespace ipc
} // namespace clawspan
