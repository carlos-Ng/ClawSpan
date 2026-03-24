#pragma once

#include "core/base/capability.h"
#include "core/base/core_config.h"
#include "core/base/module.h"
#include "core/base/security.h"
#include "common/error.h"

#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace clawspan {
namespace core {

// ModuleManager 是 core 的内部模块管理器，负责所有插件的加载、初始化与生命周期管理。
//
// 职责：
// - 统一持有所有模块（CapabilityInterface、SecurityModuleInterface）的所有权。
// - 根据配置文件依次调用各模块的 init，按注册逆序调用 shutdown。
// - 将安全模块按 priority 注册进 SecurityChain。
// - 按名称路由：为 CapabilityService 提供 getCapability 查找。
//
// 所有权模型：
// - all_modules_：unique_ptr 列表，拥有所有模块实例，决定 shutdown 顺序。
// - capabilities_：非拥有指针，指向 all_modules_ 中的 CapabilityInterface，用于路由。
// - security_modules_：非拥有指针，指向 all_modules_ 中的 SecurityModuleInterface，
//   用于注册进 SecurityChain。
//
// ModuleManager 是 CapabilityService 的内部实现细节，不对 daemon 暴露。
class ModuleManager
{
public:
	ModuleManager()  = default;
	~ModuleManager() = default;

	ModuleManager(const ModuleManager&)            = delete;
	ModuleManager& operator=(const ModuleManager&) = delete;

	// init 根据 CoreConfig 动态加载并初始化所有模块，将安全模块注册进 SecurityChain。
	//
	// 内部流程：
	//   1. 遍历 config.modules，推导 dylib 路径：module_dir + "/lib" + name + ".dylib"。
	//   2. dlopen 加载 dylib，dlsym 获取 GetModuleInstance 工厂函数。
	//   3. 调用工厂函数获取模块实例，调用 module->init(spec.params) 完成初始化。
	//   4. 能力模块加入路由表；安全模块按 spec.priority 注册进 chain。
	//
	// 入参:
	// - config: 包含 module_dir 与模块规格列表的核心配置。
	// - chain:  SecurityChain 引用，init 负责将安全模块注册其中。
	//
	// 出参/返回:
	// - Result::Ok()：所有模块加载并初始化成功。
	// - Result::Error(status)：加载或初始化失败，status 描述原因。
	Result<void> init(const CoreConfig& config, SecurityChain& chain);

	// release 按注册逆序依次调用所有模块的 release。
	//
	// 调用后所有模块进入不可用状态，getCapability 的返回值不可再使用。
	void release();

	// getCapability 按名称查找已加载的能力插件。
	//
	// 入参:
	// - name: 能力标识，与 CapabilityInterface::name() 返回值匹配。
	//
	// 出参/返回:
	// - 非空指针：找到对应插件。
	// - nullptr：  未找到，name 无对应插件或尚未加载。
	CapabilityInterface* getCapability(std::string_view name);

	// capabilityNames 返回所有已加载能力插件的名称列表。
	//
	// 供 CapabilityService 在 init 后查询，由 daemon 用于向 IpcServer 注册 handler。
	//
	// 出参/返回:
	// - 能力名称列表，顺序与 init 时注册顺序一致。
	std::vector<std::string> capabilityNames() const;

	// registerModule 注册一个模块实例（Phase 1 静态注册方式）。
	//
	// 须在 init 之前调用。ModuleManager 获得模块的所有权，并通过
	// ModuleInterface::moduleType() 自动完成类型路由：
	// - ModuleType::Capability → 加入能力路由表（capabilities_）
	// - ModuleType::Security   → 加入安全模块列表（security_modules_）
	//
	// init 时将按注册顺序读取配置并调用各模块的 init(config)；
	// 安全模块还会被注册进 SecurityChain。
	//
	// 入参:
	// - module: 模块的 unique_ptr，转移所有权。
	void registerModule(std::unique_ptr<ModuleInterface> module);

private:
	// routeModule 根据 moduleType() 将裸指针分发到对应的路由表中。
	// 由 registerModule 和 init（动态加载路径）共用。
	void routeModule(ModuleInterface* raw);

	// loadOneModule 加载单个模块：dlopen → GetModuleInstance → init → 注册。
	//
	// 入参:
	// - spec:       模块规格，含名称、优先级和初始化参数。
	// - module_dir: dylib 所在目录路径。
	// - chain:      安全模块注册目标。
	//
	// 出参/返回:
	// - Result::Ok()：加载并初始化成功。
	// - Result::Error(status)：加载或初始化失败，status 描述原因。
	Result<void> loadOneModule(const ModuleSpec& spec,
	                           const std::string& module_dir,
	                           SecurityChain& chain);

	// 拥有所有模块实例，按注册顺序排列，release 时逆序遍历。
	std::vector<std::unique_ptr<ModuleInterface>> all_modules_;

	// 能力插件路由表，非拥有指针，指向 all_modules_ 中对应元素。
	std::unordered_map<std::string, CapabilityInterface*> capabilities_;

	// 安全模块列表，非拥有指针，指向 all_modules_ 中对应元素，用于注册进 SecurityChain。
	std::vector<SecurityModuleInterface*> security_modules_;

	// 动态库句柄列表，在 release 时统一 dlclose。
	std::vector<void*> dl_handles_;
};

} // namespace core
} // namespace clawspan
