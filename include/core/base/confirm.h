#pragma once

#include "core/base/security.h"

#include <string>

namespace clawspan {
namespace core {

// ConfirmHandlerInterface 是 NeedConfirm 确认通道的抽象接口。
//
// CapabilityService 通过 setConfirmHandler 注入此接口的实现。
// 当 SecurityChain::runPreHook 返回 SecurityAction::NeedConfirm 时，
// CapabilityService 调用 requestConfirm 阻塞当前调用线程，等待用户做出确认。
//
// 每次调用独占一个线程，并发的 NeedConfirm 请求会被串行化（一个接一个处理）。
// 其他不涉及 NeedConfirm 的调用线程不受影响，正常并发执行。
//
// 未注入 ConfirmHandler 时：CapabilityService 收到 NeedConfirm 返回
// CONFIRM_REQUIRED 错误，不执行确认流程（保持向后兼容）。
class ConfirmHandlerInterface
{
public:
	virtual ~ConfirmHandlerInterface() = default;

	// requestConfirm 向用户请求确认，阻塞调用方线程直到用户响应或超时。
	//
	// 入参:
	// - ctx:    本次调用的安全上下文（含 capability_name、operation、params）。
	// - reason: 由安全模块填写的原因描述，展示给用户供决策参考。
	//
	// 出参/返回:
	// - true：用户确认，操作继续执行。
	// - false：用户拒绝或超时，操作取消并返回 OPERATION_DENIED。
	virtual bool requestConfirm(const SecurityContext& ctx,
	                            const std::string& reason) = 0;
};

} // namespace core
} // namespace clawspan
