#pragma once

#include "common/status.h"

#include <type_traits>
#include <utility>
#include <tl/expected.hpp>

namespace clawspan {

template <typename T>
class [[nodiscard]] Result
{
public:
	using ValueType    = T;
	using ErrorType    = Status;
	using ExpectedType = tl::expected<T, Status>;

	// Ok 用于构造成功结果（带值）。
	//
	// 入参:
	// - value: 成功值，支持移动/拷贝。
	//
	// 出参/返回:
	// - Result<T>::Ok(...)：成功。
	template <typename U = T,
	          typename = std::enable_if_t<std::is_constructible<T, U&&>::value>>
	static Result Ok(U&& value) noexcept(std::is_nothrow_constructible<T, U&&>::value)
	{
		return Result(ExpectedType(tl::in_place, std::forward<U>(value)));
	}

	// Error 用于构造失败结果（直接传入 Status）。
	//
	// 入参:
	// - status: 错误状态。
	//
	// 出参/返回:
	// - Result<T>::Error(status)：失败。
	static Result Error(Status status)
	{
		if (status.message == nullptr || status.message[0] == '\0') {
			status.message = statusMessage(status.code);
		}
		return Result(ExpectedType(tl::unexpect, std::move(status)));
	}

	// Error 用于构造失败结果（便捷入参：code + 静态 message）。
	//
	// 入参:
	// - code:    错误码。
	// - message: 静态错误信息（需保证生命周期）。
	//
	// 出参/返回:
	// - Result<T>::Error(code, message)：失败。
	static Result Error(Status::Code code, const char* message)
	{
		return Error(Status(code, message));
	}

	// Error 用于构造失败结果（便捷入参：code + 动态 std::string reason）。
	//
	// 动态 reason 被 Status 内部持有，无需调用方维护其生命周期。
	//
	// 入参:
	// - code:   错误码。
	// - reason: 运行时生成的原因字符串（右值移动，零拷贝）。
	//
	// 出参/返回:
	// - Result<T>::Error(code, reason)：失败。
	static Result Error(Status::Code code, std::string reason)
	{
		return Error(Status(code, std::move(reason)));
	}

	// Error 用于构造失败结果（便捷入参：仅 code）。
	//
	// 入参:
	// - code: 错误码。
	//
	// 出参/返回:
	// - Result<T>::Error(code)：失败。
	static Result Error(Status::Code code)
	{
		return Error(Status(code, ""));
	}

	// Error 用于从另一个 Result 传递失败结果。
	//
	// 入参:
	// - other: 任意类型的失败 Result。
	//
	// 出参/返回:
	// - Result<T>::Error 失败。
	template <typename U>
	static Result Error(const Result<U>& other)
	{
		return Error(other.error());
	}

	// success 用于判断是否成功。
	//
	// 出参/返回:
	// - true：成功；false：失败。
	constexpr bool success() const noexcept { return expected_.has_value(); }

	// failure 用于判断是否失败。
	//
	// 出参/返回:
	// - true：失败；false：成功。
	constexpr bool failure() const noexcept { return !expected_.has_value(); }

	// value 用于获取成功值（左值）。
	//
	// 出参/返回:
	// - 成功值引用；失败时行为由底层 expected 处理。
	T& value() & { return expected_.value(); }

	// value 用于获取成功值（常量左值）。
	//
	// 出参/返回:
	// - 成功值常量引用；失败时行为由底层 expected 处理。
	const T& value() const & { return expected_.value(); }

	// value 用于获取成功值（右值）。
	//
	// 出参/返回:
	// - 成功值右值引用；失败时行为由底层 expected 处理。
	T&& value() && { return std::move(expected_).value(); }

	// error 用于获取失败状态（左值）。
	//
	// 出参/返回:
	// - 失败状态引用；成功时行为由底层 expected 处理。
	Status& error() & { return expected_.error(); }

	// error 用于获取失败状态（常量左值）。
	//
	// 出参/返回:
	// - 失败状态常量引用；成功时行为由底层 expected 处理。
	const Status& error() const & { return expected_.error(); }

	// value_or 用于在失败时返回默认值（左值）。
	//
	// 入参:
	// - default_value: 失败兜底值。
	//
	// 出参/返回:
	// - 成功值或默认值（按值返回）。
	template <typename U,
	          typename = std::enable_if_t<std::is_constructible<T, U&&>::value>>
	T value_or(U&& default_value) const &
	{
		if (expected_.has_value()) {
			return expected_.value();
		}
		return T(std::forward<U>(default_value));
	}

	// value_or 用于在失败时返回默认值（右值）。
	//
	// 入参:
	// - default_value: 失败兜底值。
	//
	// 出参/返回:
	// - 成功值或默认值（按值返回，可移动）。
	template <typename U,
	          typename = std::enable_if_t<std::is_constructible<T, U&&>::value>>
	T value_or(U&& default_value) &&
	{
		if (expected_.has_value()) {
			return std::move(expected_.value());
		}
		return T(std::forward<U>(default_value));
	}

	// value_or_else 用于在失败时延迟计算默认值（左值）。
	//
	// 入参:
	// - func: 回调，返回 T。
	//
	// 出参/返回:
	// - 成功值或回调结果（按值返回）。
	template <typename F>
	T value_or_else(F&& func) const &
	{
		if (expected_.has_value()) {
			return expected_.value();
		}
		return T(std::forward<F>(func)());
	}

	// value_or_else 用于在失败时延迟计算默认值（右值）。
	//
	// 入参:
	// - func: 回调，返回 T。
	//
	// 出参/返回:
	// - 成功值或回调结果（按值返回，可移动）。
	template <typename F>
	T value_or_else(F&& func) &&
	{
		if (expected_.has_value()) {
			return std::move(expected_.value());
		}
		return T(std::forward<F>(func)());
	}

	// expected 用于访问底层 expected（左值）。
	//
	// 出参/返回:
	// - 底层 expected 引用。
	ExpectedType& expected() & { return expected_; }

	// expected 用于访问底层 expected（常量左值）。
	//
	// 出参/返回:
	// - 底层 expected 常量引用。
	const ExpectedType& expected() const & { return expected_; }

private:
	// Result 内部构造函数，包装底层 expected。
	//
	// 入参:
	// - expected: 底层 expected。
	explicit Result(ExpectedType expected) noexcept(std::is_nothrow_move_constructible<ExpectedType>::value)
	    : expected_(std::move(expected))
	{}

	ExpectedType expected_;
};

template <>
class [[nodiscard]] Result<void>
{
public:
	using ValueType    = void;
	using ErrorType    = Status;
	using ExpectedType = tl::expected<void, Status>;

	// Ok 用于构造成功结果（void）。
	//
	// 出参/返回:
	// - Result<void>::Ok()：成功。
	static Result Ok() noexcept
	{
		return Result(ExpectedType());
	}

	// Error 用于构造失败结果（直接传入 Status）。
	//
	// 入参:
	// - status: 错误状态。
	//
	// 出参/返回:
	// - Result<void>::Error(status)：失败。
	static Result Error(Status status)
	{
		if (status.message == nullptr || status.message[0] == '\0') {
			status.message = statusMessage(status.code);
		}
		return Result(ExpectedType(tl::unexpect, std::move(status)));
	}

	// Error 用于构造失败结果（便捷入参：code + 静态 message）。
	//
	// 入参:
	// - code:    错误码。
	// - message: 静态错误信息（需保证生命周期）。
	//
	// 出参/返回:
	// - Result<void>::Error(code, message)：失败。
	static Result Error(Status::Code code, const char* message)
	{
		return Error(Status(code, message));
	}

	// Error 用于构造失败结果（便捷入参：code + 动态 std::string reason）。
	//
	// 动态 reason 被 Status 内部持有，无需调用方维护其生命周期。
	static Result Error(Status::Code code, std::string reason)
	{
		return Error(Status(code, std::move(reason)));
	}

	// Error 用于构造失败结果（便捷入参：仅 code）。
	//
	// 入参:
	// - code: 错误码。
	//
	// 出参/返回:
	// - Result<void>::Error(code)：失败。
	static Result Error(Status::Code code)
	{
		return Error(Status(code, ""));
	}

	// Error 用于从另一个 Result 传递失败结果。
	//
	// 入参:
	// - other: 任意类型的失败 Result。
	//
	// 出参/返回:
	// - Result<void>::Error 失败。
	template <typename U>
	static Result Error(const Result<U>& other)
	{
		return Error(other.error());
	}

	// success 用于判断是否成功。
	//
	// 出参/返回:
	// - true：成功；false：失败。
	constexpr bool success() const noexcept { return expected_.has_value(); }

	// failure 用于判断是否失败。
	//
	// 出参/返回:
	// - true：失败；false：成功。
	constexpr bool failure() const noexcept { return !expected_.has_value(); }

	// error 用于获取失败状态（左值）。
	//
	// 出参/返回:
	// - 失败状态引用；成功时行为由底层 expected 处理。
	Status& error() & { return expected_.error(); }

	// error 用于获取失败状态（常量左值）。
	//
	// 出参/返回:
	// - 失败状态常量引用；成功时行为由底层 expected 处理。
	const Status& error() const & { return expected_.error(); }

	// expected 用于访问底层 expected（左值）。
	//
	// 出参/返回:
	// - 底层 expected 引用。
	ExpectedType& expected() & { return expected_; }

	// expected 用于访问底层 expected（常量左值）。
	//
	// 出参/返回:
	// - 底层 expected 常量引用。
	const ExpectedType& expected() const & { return expected_; }

private:
	// Result 内部构造函数，包装底层 expected。
	//
	// 入参:
	// - expected: 底层 expected。
	explicit Result(ExpectedType expected) noexcept(std::is_nothrow_move_constructible<ExpectedType>::value)
	    : expected_(std::move(expected))
	{}

	ExpectedType expected_;
};

} // namespace clawspan
