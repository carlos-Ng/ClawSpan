#pragma once

#include "core/base/module_config.h"
#include "common/error.h"

namespace clawspan {
namespace core {

// ModuleType 标识模块的类别，供 ModuleManager 在注册时进行类型路由。
enum class ModuleType
{
	Capability, // 能力模块，继承 CapabilityInterface
	Security,   // 安全模块，继承 SecurityModuleInterface
};

// ModuleInterface 是所有模块（能力模块与安全模块）的公共抽象基类，
// 定义统一的身份标识、类型识别与生命周期接口。
//
// ModuleManager 通过此接口统一管理所有模块的 init 与 release，
// 并通过 moduleType() 判断模块类别，完成类型路由。
//
// 继承层次：
//   ModuleInterface
//     ├── CapabilityInterface    （能力模块，新增 execute 接口）
//     └── SecurityModuleInterface（安全模块，新增 preHook/postHook 接口）
class ModuleInterface
{
public:
	virtual ~ModuleInterface() = default;

	// name 返回模块的唯一标识，供 ModuleManager 路由与日志使用。
	//
	// 出参/返回:
	// - 静态字符串，生命周期与模块实例相同。
	virtual const char* name() const = 0;

	// version 返回模块的版本字符串。
	//
	// 出参/返回:
	// - 静态字符串，例如 "1.0.0"。
	virtual const char* version() const = 0;

	// moduleType 返回模块的类别，供 ModuleManager 注册时进行类型路由。
	//
	// 出参/返回:
	// - ModuleType::Capability：能力模块。
	// - ModuleType::Security：安全模块。
	virtual ModuleType moduleType() const = 0;

	// init 使用配置参数完成模块自身的初始化。
	//
	// 由 ModuleManager 在启动阶段调用，传入从外部配置文件中提取的
	// 当前模块配置节，模块无需感知外部配置格式（toml/json 等）。
	//
	// 入参:
	// - config: 当前模块的配置项，以 flat key-value 形式提供。
	//
	// 出参/返回:
	// - Result::Ok()：初始化成功。
	// - Result::Error(status)：初始化失败，status 描述原因。
	virtual Result<void> init(const ModuleConfig& config) = 0;

	// release 释放模块持有的所有资源。
	//
	// 由 ModuleManager 在关闭阶段按注册逆序调用。
	// 调用后模块不可再使用。
	virtual void release() = 0;
};

} // namespace core
} // namespace clawspan

// ─────────────────────────────────────────────────────────────────────────────
// 模块导出接口（供模块实现文件使用）
// ─────────────────────────────────────────────────────────────────────────────

// ModuleFactory 是模块工厂函数的指针类型。
//
// ModuleManager 在动态加载（Phase 3）时通过 dlsym("GetModuleInstance") 获取此函数指针，
// 调用后得到模块实例的所有权（由 unique_ptr 接管）。
using ModuleFactory = clawspan::core::ModuleInterface* (*)();

// CLAWSPAN_MODULE_EXPORT 宏用于在模块实现文件中导出工厂函数。
//
// 每个模块的 .cc 文件末尾调用一次此宏即可完成导出，无需手动编写 extern "C" 样板代码。
//
// 用法：
//   CLAWSPAN_MODULE_EXPORT(AXCapability)
//
// 等价于：
//   extern "C" clawspan::core::ModuleInterface* GetModuleInstance() {
//       return new AXCapability();
//   }
//
// 注：模块资源清理通过 ModuleInterface::release() 完成，
//     ModuleManager 在 delete 前保证已调用 release()。
//
// Windows DLL 必须显式导出符号，否则 GetProcAddress 无法找到 GetModuleInstance。
#ifdef _WIN32
#  define CLAWSPAN_MODULE_EXPORT(ClassName)                                    \
      extern "C" __declspec(dllexport) clawspan::core::ModuleInterface* GetModuleInstance() \
      {                                                                         \
          return new ClassName();                                               \
      }
#else
#  define CLAWSPAN_MODULE_EXPORT(ClassName)                              \
      extern "C" clawspan::core::ModuleInterface* GetModuleInstance()    \
      {                                                                   \
          return new ClassName();                                         \
      }
#endif

