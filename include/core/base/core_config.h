#pragma once

#include "core/base/module_config.h"

#include <string>
#include <vector>

namespace clawspan {
namespace core {

// ModuleSpec 描述一个模块的加载规格，由 daemon 解析 TOML 后填充，
// 传入 CapabilityService::init 驱动 ModuleManager 完成动态加载。
//
// name 字段同时用于：
//   - dylib 路径推导：module_dir + "/lib" + name + ".dylib"
//   - IPC 路由前缀：UnixIpcServer::registerModule(name, handler)
//   - CapabilityService::callCapability(name, operation, params) 的 capability_name
//
// 因此，模块的 ModuleInterface::name() 返回值须与此字段保持一致。
struct ModuleSpec
{
	// 模块标识，例如 "capability_ax"、"security_audit_logger"。
	std::string name;

	// 安全模块在调用链中的执行优先级，数值越小越先执行。
	// 能力模块忽略此字段；安全模块默认 100，与 SecurityModuleInterface::priority() 一致。
	int priority = 100;

	// 模块自定义初始化参数，来自 TOML [[modules]] 中各内联键值对。
	ModuleConfig params;
};

// CoreConfig 是 CapabilityService 初始化所需的完整配置，
// 由 daemon 解析 TOML 后构造并传入。
struct CoreConfig
{
	// 模块 dylib 所在目录（相对于可执行文件，或绝对路径）。
	std::string module_dir;

	// 模块规格列表，按 TOML [[modules]] 声明顺序排列。
	std::vector<ModuleSpec> modules;
};

} // namespace core
} // namespace clawspan
