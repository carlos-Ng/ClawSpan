#pragma once

#include "core/base/module.h"
#include "common/error.h"

#include <nlohmann/json.hpp>
#include <string_view>

namespace clawspan {
namespace core {

// CapabilityInterface 是所有能力插件的基类，继承 ModuleInterface 获得统一的
// 生命周期管理，并新增 execute 作为能力执行的唯一入口。
//
// 所有具体能力（GUI 操作、命令执行等）均继承此接口。
// 生命周期（init/shutdown）由 ModuleManager 统一调用，外部（包括 CapabilityService）
// 不直接管理模块生命周期。
class CapabilityInterface : public ModuleInterface
{
public:
	// moduleType 返回 ModuleType::Capability，标识本接口为能力模块类别。
	//
	// 出参/返回:
	// - ModuleType::Capability。
	ModuleType moduleType() const override
	{
		return ModuleType::Capability;
	}

	// execute 执行指定操作，是 CapabilityService 调用能力的唯一入口。
	//
	// 入参:
	// - operation: 操作名，例如 "list_windows"、"perform_action"。
	// - params:    操作参数（JSON 对象），无参数时传空对象 {}。
	//
	// 出参/返回:
	// - Result::Ok(json)：操作成功，json 为返回数据。
	// - Result::Error(status)：操作失败，status 描述原因。
	virtual Result<nlohmann::json> execute(std::string_view operation,
	                                       const nlohmann::json& params) = 0;
};

} // namespace core
} // namespace clawspan
