#include "core/base/module_manager.h"
#include "core/base/capability.h"
#include "core/base/security.h"

#include "common/log.h"

#include <cassert>

#ifdef _WIN32
#  include <windows.h>
#else
#  error "Platform not supported: add POSIX dlfcn.h implementation here"
#endif

namespace clawspan {
namespace core {

// ─── 平台辅助：动态库加载/卸载 ───────────────────────────────────────────────

#ifdef _WIN32

// dlError 将 GetLastError() 格式化为可读字符串，用于日志输出。
static std::string dlError()
{
	DWORD  code = ::GetLastError();
	LPSTR  buf  = nullptr;
	::FormatMessageA(
	    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
	        FORMAT_MESSAGE_IGNORE_INSERTS,
	    nullptr, code,
	    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
	    reinterpret_cast<LPSTR>(&buf), 0, nullptr);
	std::string msg = buf ? buf : "unknown error";
	if (buf) {
		::LocalFree(buf);
	}
	// 去除末尾换行
	while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
		msg.pop_back();
	}
	return msg;
}

// dllPath 根据 module_dir 和模块名拼接 Windows DLL 路径。
// Windows 惯例：无 lib 前缀，使用 .dll 扩展名。
static std::string dllPath(const std::string& module_dir, const std::string& name)
{
	return module_dir + "\\" + name + ".dll";
}

// openLib 加载 DLL，失败返回 nullptr 并通过 out_err 输出错误信息。
static void* openLib(const std::string& path, std::string& out_err)
{
	HMODULE h = ::LoadLibraryA(path.c_str());
	if (!h) {
		out_err = dlError();
		return nullptr;
	}
	return reinterpret_cast<void*>(h);
}

// getSymbol 从已加载库中查找符号，失败返回 nullptr。
static void* getSymbol(void* handle, const char* symbol)
{
	return reinterpret_cast<void*>(
	    ::GetProcAddress(reinterpret_cast<HMODULE>(handle), symbol));
}

// closeLib 卸载动态库。
static void closeLib(void* handle)
{
	if (handle) {
		::FreeLibrary(reinterpret_cast<HMODULE>(handle));
	}
}

#else
#  error "Platform not supported: add POSIX dlfcn.h implementation here"
#endif

// ─── ModuleManager 方法实现 ──────────────────────────────────────────────────

// registerModule 注册一个模块实例，转移所有权，并按类型路由到对应列表。
void ModuleManager::registerModule(std::unique_ptr<ModuleInterface> module)
{
	if (!module) {
		return;
	}
	auto* raw = module.get();
	all_modules_.push_back(std::move(module));
	routeModule(raw);
}

// routeModule 根据 moduleType() 将裸指针分发到能力路由表或安全模块列表。
//
// 类型安全策略：
//   Debug 构建：dynamic_cast 验证，失败则 assert 中止，早期发现插件声明错误。
//   Release 构建：仍使用 dynamic_cast 做非致命验证，失败时记录 ERROR 并跳过该模块，
//     避免 static_cast 在插件 moduleType() 声明与实际继承不一致时引发未定义行为。
//     dynamic_cast 开销仅在加载阶段（启动时）产生，运行时热路径不受影响。
void ModuleManager::routeModule(ModuleInterface* raw)
{
	if (raw->moduleType() == ModuleType::Capability) {
		auto* cap = dynamic_cast<CapabilityInterface*>(raw);
		if (!cap) {
			LOG_ERROR("module '{}': moduleType() reports Capability but does not "
			          "inherit CapabilityInterface — skipping (plugin bug)",
			          raw->name());
			assert(false && "moduleType() reports Capability but class does not inherit CapabilityInterface");
			return;
		}
		capabilities_[std::string(cap->name())] = cap;
	} else if (raw->moduleType() == ModuleType::Security) {
		auto* sec = dynamic_cast<SecurityModuleInterface*>(raw);
		if (!sec) {
			LOG_ERROR("module '{}': moduleType() reports Security but does not "
			          "inherit SecurityModuleInterface — skipping (plugin bug)",
			          raw->name());
			assert(false && "moduleType() reports Security but class does not inherit SecurityModuleInterface");
			return;
		}
		security_modules_.push_back(sec);
	}
}

// getCapability 按名称查找已加载的能力插件。
CapabilityInterface* ModuleManager::getCapability(std::string_view name)
{
	auto it = capabilities_.find(std::string(name));
	if (it == capabilities_.end()) {
		return nullptr;
	}
	return it->second;
}

// capabilityNames 返回所有已加载能力插件的名称列表。
std::vector<std::string> ModuleManager::capabilityNames() const
{
	std::vector<std::string> names;
	names.reserve(capabilities_.size());
	for (const auto& [name, _] : capabilities_) {
		names.push_back(name);
	}
	return names;
}

// loadOneModule 加载单个模块：LoadLibrary → GetModuleInstance → init → 注册。
Result<void> ModuleManager::loadOneModule(const ModuleSpec& spec,
                                          const std::string& module_dir,
                                          SecurityChain& chain)
{
	std::string lib_path = dllPath(module_dir, spec.name);
	LOG_INFO("loading module: {} from {}", spec.name, lib_path);

	std::string err_msg;
	void* handle = openLib(lib_path, err_msg);
	if (!handle) {
		LOG_ERROR("LoadLibrary failed for '{}': {}", lib_path, err_msg);
		return Result<void>::Error(Status::MODULE_LOAD_ERROR);
	}
	// 注意：handle 暂不加入 dl_handles_，仅在整个 loadOneModule 全部成功后才入队，
	// 防止中途失败时 dl_handles_ 与 all_modules_ 不对应

	auto* factory = reinterpret_cast<ModuleFactory>(
	    getSymbol(handle, "GetModuleInstance"));
	if (!factory) {
		LOG_ERROR("module '{}' missing GetModuleInstance symbol", spec.name);
		closeLib(handle);
		return Result<void>::Error(Status::MODULE_LOAD_ERROR);
	}

	auto module = std::unique_ptr<ModuleInterface>(factory());
	if (!module) {
		LOG_ERROR("GetModuleInstance returned nullptr for module '{}'", spec.name);
		closeLib(handle);
		return Result<void>::Error(Status::INTERNAL_ERROR);
	}

	ModuleType type = module->moduleType();

	auto result = module->init(spec.params);
	if (result.failure()) {
		LOG_ERROR("module '{}' init failed: {}", spec.name, result.error().message);
		closeLib(handle);
		return Result<void>::Error(result.error());
	}

	// 所有步骤均成功，才将 handle 加入管理列表
	dl_handles_.push_back(handle);
	registerModule(std::move(module));

	if (type == ModuleType::Security) {
		// 从 all_modules_.back() 重新取指针，避免 move 后继续使用原裸指针引发困惑
		auto* sec = static_cast<SecurityModuleInterface*>(all_modules_.back().get());
		chain.registerModule(sec, spec.priority);
		LOG_INFO("security module registered: {} (priority={})", spec.name, spec.priority);
	} else {
		LOG_INFO("capability module registered: {}", spec.name);
	}

	return Result<void>::Ok();
}

// init 根据 CoreConfig 动态加载并初始化所有模块。
Result<void> ModuleManager::init(const CoreConfig& config, SecurityChain& chain)
{
	// module_dir 为空但存在需要加载的模块时，路径退化为 "\xxx.dll"，
	// 通常不存在，提前报错以给出更明确的错误信息
	if (config.module_dir.empty() && !config.modules.empty()) {
		LOG_ERROR("module_dir is not set but {} module(s) are configured; "
		          "set [daemon].module_dir in TOML or use --module-dir",
		          config.modules.size());
		return Result<void>::Error(Status::INVALID_ARGUMENT, "module_dir must not be empty");
	}
	for (const auto& spec : config.modules) {
		auto result = loadOneModule(spec, config.module_dir, chain);
		if (result.failure()) {
			return result;
		}
	}
	return Result<void>::Ok();
}

// release 按注册逆序调用所有模块的 release，再 delete 模块对象，最后卸载库。
//
// 顺序：module->release() → all_modules_.clear()（delete）→ FreeLibrary
// 必须先完成所有 unique_ptr 析构再调用 FreeLibrary，
// 否则析构函数执行时所属 DLL 已被卸载会导致崩溃。
void ModuleManager::release()
{
	for (auto it = all_modules_.rbegin(); it != all_modules_.rend(); ++it) {
		(*it)->release();
	}
	capabilities_.clear();
	security_modules_.clear();
	all_modules_.clear();

	// FreeLibrary 按注册逆序执行，与 release/析构顺序对称，
	// 防止后注册模块的析构中调用已被先卸载的 DLL 符号导致崩溃
	for (auto it = dl_handles_.rbegin(); it != dl_handles_.rend(); ++it) {
		closeLib(*it);
	}
	dl_handles_.clear();
}

} // namespace core
} // namespace clawspan
