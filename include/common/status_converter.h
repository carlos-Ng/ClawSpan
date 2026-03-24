#pragma once

#include "common/status.h"

namespace clawspan {

// StatusConverterInterface 定义将 Status 转换为外部错误类型 T 的通用接口。
// 各集成层（IPC、gRPC 等）实现各自具体的转换逻辑，与 common 层完全解耦。
template <typename T>
class StatusConverterInterface
{
public:
	virtual ~StatusConverterInterface() = default;

	// convert 将 Status 转换为目标类型 T。
	//
	// 入参:
	// - status: 待转换的状态。
	//
	// 出参/返回:
	// - T: 转换后的目标类型实例。
	virtual T convert(const Status& status) const = 0;
};

} // namespace clawspan
