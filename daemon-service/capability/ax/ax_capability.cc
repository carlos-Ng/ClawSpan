#include "capability/ax/ax_capability.h"

#ifdef _WIN32
#  include "ax_provider_windows.h"
#else
#  error "Platform not supported: add macOS ax_provider_macos.h include here"
#endif

#include "ax_text_serializer.h"

#include <algorithm>
#include <functional>
#include <memory>

namespace clawspan {
namespace capability {
namespace ax {

// ─────────────────────────────────────────────────────────────────────────────
// AXCapability::Implement
// ─────────────────────────────────────────────────────────────────────────────

struct AXCapability::Implement
{
	std::unique_ptr<AXProviderInterface> provider;
	int default_max_depth = 10;
};

// ─────────────────────────────────────────────────────────────────────────────
// 生命周期
// ─────────────────────────────────────────────────────────────────────────────

AXCapability::AXCapability()                        = default;
AXCapability::~AXCapability()                       = default;
AXCapability::AXCapability(AXCapability&&) noexcept = default;
AXCapability& AXCapability::operator=(AXCapability&&) noexcept = default;

const char* AXCapability::name()    const { return "capability_ax"; }
const char* AXCapability::version() const { return "1.0.0"; }

// init 初始化 AX 能力模块。
//
// 入参:
// - config: 模块配置（可选键 "default_max_depth"）。
//
// 出参/返回:
// - Result::Ok()：成功。
// - Result::Error(ACCESSIBILITY_DENIED)：辅助功能权限未授予。
Result<void> AXCapability::init(const core::ModuleConfig& config)
{
	implement_ = std::make_unique<Implement>();

	if (auto depth = config.getInt("default_max_depth")) {
		implement_->default_max_depth =
		    static_cast<int>(std::max(int64_t(1), std::min(*depth, int64_t(20))));
	}

#ifdef _WIN32
	implement_->provider = std::make_unique<AXProviderWindows>();
#else
#  error "Platform not supported: add macOS AXProviderMacOS instantiation here"
#endif

	if (!implement_->provider->isPermissionGranted()) {
		return Result<void>::Error(Status::ACCESSIBILITY_DENIED);
	}

	return Result<void>::Ok();
}

// release 释放 AX 能力模块持有的所有资源。
void AXCapability::release()
{
	implement_.reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// 各操作的处理函数（文件内部使用）
// ─────────────────────────────────────────────────────────────────────────────

// handleListWindows 枚举可访问窗口并以 AXT 文本格式返回。
//
// 输出格式（每个窗口占一行）：
//   <window_id>  <app_name>  "<title>"  [focused]
//
// 出参/返回:
// - Result::Ok(<AXT 文本字符串>)
static Result<nlohmann::json> handleListWindows(AXProviderInterface& provider)
{
	auto result = provider.listWindows();
	if (result.failure()) {
		return Result<nlohmann::json>::Error(result);
	}

	return Result<nlohmann::json>::Ok(
	    nlohmann::json(AXTextSerializer{}.serializeWindowList(result.value())));
}

// handleGetUITree 获取指定窗口的 UI 元素树，以 AXT 紧凑文本格式返回。
//
// 入参（params）:
// - window_id      (string, 必选)
// - max_depth      (int,    可选，缺省使用模块配置值；有效范围 [1, 20]，由 provider 内部裁剪)
// - include_bounds (bool,   可选，默认 false；为 true 时每行末尾附带屏幕坐标)
//
// 出参/返回:
// - Result::Ok(<AXT 文本字符串>)
//   AXT 格式说明见 docs/design-draft.md § 4.5
static Result<nlohmann::json> handleGetUITree(AXProviderInterface&  provider,
                                               const nlohmann::json& params,
                                               int                   defaultDepth)
{
	if (!params.contains("window_id") || !params["window_id"].is_string()) {
		return Result<nlohmann::json>::Error(Status::INVALID_ARGUMENT);
	}

	std::string window_id = params["window_id"].get<std::string>();

	int max_depth = defaultDepth;
	if (params.contains("max_depth") && params["max_depth"].is_number_integer()) {
		// 限制到 [1, 20]：0 或负数会让根节点立即被截断，大于 20 可能导致栈深度过大
		max_depth = std::clamp(params["max_depth"].get<int>(), 1, 20);
	}

	bool include_bounds = false;
	if (params.contains("include_bounds") && params["include_bounds"].is_boolean()) {
		include_bounds = params["include_bounds"].get<bool>();
	}

	auto result = provider.getUITree(window_id, max_depth);
	if (result.failure()) {
		return Result<nlohmann::json>::Error(result);
	}

	AXTextSerializer::Options opts;
	opts.include_bounds = include_bounds;
	return Result<nlohmann::json>::Ok(
	    nlohmann::json(AXTextSerializer{}.serialize(result.value(), opts)));
}

// handleElementOp 为只需 element_path 参数的操作提供通用处理模板。
//
// 入参（params）:
// - element_path (string, 必选)
//
// 出参/返回:
// - Result::Ok({"success": true})。
template <typename Fn>
static Result<nlohmann::json> handleElementOp(const nlohmann::json& params, Fn&& fn)
{
	if (!params.contains("element_path") || !params["element_path"].is_string()) {
		return Result<nlohmann::json>::Error(Status::INVALID_ARGUMENT);
	}

	auto result = fn(params["element_path"].get<std::string>());
	if (result.failure()) {
		return Result<nlohmann::json>::Error(result);
	}

	return Result<nlohmann::json>::Ok({{"success", true}});
}

// handleSetValue 向指定元素写入文字值。
//
// 入参（params）:
// - element_path (string, 必选)
// - value        (string, 必选)
//
// 出参/返回:
// - Result::Ok({"success": true})。
static Result<nlohmann::json> handleSetValue(AXProviderInterface&  provider,
                                              const nlohmann::json& params)
{
	if (!params.contains("element_path") || !params["element_path"].is_string() ||
	    !params.contains("value")        || !params["value"].is_string()) {
		return Result<nlohmann::json>::Error(Status::INVALID_ARGUMENT);
	}

	auto result = provider.setValue(params["element_path"].get<std::string>(),
	                                 params["value"].get<std::string>());
	if (result.failure()) {
		return Result<nlohmann::json>::Error(result);
	}

	return Result<nlohmann::json>::Ok({{"success", true}});
}

// handleScroll 对指定元素执行滚动。
//
// 入参（params）:
// - element_path (string, 必选)
// - direction    (string, 必选): up / down / left / right
// - amount       (int, 可选, 默认 3)：滚动档数（notches），每档 = 一次鼠标滚轮格
//
// 出参/返回:
// - Result::Ok({"success": true})。
static Result<nlohmann::json> handleScroll(AXProviderInterface&  provider,
                                            const nlohmann::json& params)
{
	if (!params.contains("element_path") || !params["element_path"].is_string() ||
	    !params.contains("direction")    || !params["direction"].is_string()) {
		return Result<nlohmann::json>::Error(Status::INVALID_ARGUMENT);
	}

	int amount = 3; // 默认 3 档，与多数系统默认滚动速度一致
	if (params.contains("amount") && params["amount"].is_number_integer()) {
		amount = params["amount"].get<int>();
	}

	auto result = provider.scroll(params["element_path"].get<std::string>(),
	                               params["direction"].get<std::string>(),
	                               amount);
	if (result.failure()) {
		return Result<nlohmann::json>::Error(result);
	}

	return Result<nlohmann::json>::Ok({{"success", true}});
}

// handleKeyPress 模拟按下单个按键。
//
// 入参（params）:
// - key       (string, 必选): 按键名称，如 "Return"、"Escape"、"a"
// - window_id (string, 可选): 目标窗口 ID；传入时事件定向投递到该进程，不抢夺前台焦点
//
// 出参/返回:
// - Result::Ok({"success": true})。
static Result<nlohmann::json> handleKeyPress(AXProviderInterface&  provider,
                                              const nlohmann::json& params)
{
	if (!params.contains("key") || !params["key"].is_string()) {
		return Result<nlohmann::json>::Error(Status::INVALID_ARGUMENT);
	}

	std::string window_id;
	if (params.contains("window_id") && params["window_id"].is_string()) {
		window_id = params["window_id"].get<std::string>();
	}

	auto result = provider.keyPress(params["key"].get<std::string>(),
	                                 window_id.empty() ? std::string_view{} : window_id);
	if (result.failure()) {
		return Result<nlohmann::json>::Error(result);
	}

	return Result<nlohmann::json>::Ok({{"success", true}});
}

// handleKeyCombo 模拟按下组合键。
//
// 入参（params）:
// - keys      (array of string, 必选): 如 ["Cmd", "C"]，最后一个为主键
// - window_id (string, 可选): 目标窗口 ID；传入时事件定向投递到该进程，不抢夺前台焦点
//
// 出参/返回:
// - Result::Ok({"success": true})。
static Result<nlohmann::json> handleKeyCombo(AXProviderInterface&  provider,
                                              const nlohmann::json& params)
{
	if (!params.contains("keys") || !params["keys"].is_array()) {
		return Result<nlohmann::json>::Error(Status::INVALID_ARGUMENT);
	}

	std::vector<std::string> keys;
	for (const auto& k : params["keys"]) {
		if (!k.is_string()) {
			return Result<nlohmann::json>::Error(Status::INVALID_ARGUMENT);
		}
		keys.push_back(k.get<std::string>());
	}

	std::string window_id;
	if (params.contains("window_id") && params["window_id"].is_string()) {
		window_id = params["window_id"].get<std::string>();
	}

	auto result = provider.keyCombo(keys,
	                                 window_id.empty() ? std::string_view{} : window_id);
	if (result.failure()) {
		return Result<nlohmann::json>::Error(result);
	}

	return Result<nlohmann::json>::Ok({{"success", true}});
}

// handleActivate 将目标窗口激活到前台。
//
// 入参（params）:
// - window_id (string, 必选)
//
// 出参/返回:
// - Result::Ok({"success": true})。
static Result<nlohmann::json> handleActivate(AXProviderInterface&  provider,
                                              const nlohmann::json& params)
{
	if (!params.contains("window_id") || !params["window_id"].is_string()) {
		return Result<nlohmann::json>::Error(Status::INVALID_ARGUMENT);
	}

	auto result = provider.activate(params["window_id"].get<std::string>());
	if (result.failure()) {
		return Result<nlohmann::json>::Error(result);
	}

	return Result<nlohmann::json>::Ok({{"success", true}});
}

// ─────────────────────────────────────────────────────────────────────────────
// execute：operation 路由
// ─────────────────────────────────────────────────────────────────────────────

// execute 将请求按 operation 名称分发到对应处理函数。
//
// 入参:
// - operation: 操作名（区分大小写）。
// - params:    操作参数 JSON。
//
// 出参/返回:
// - Result::Ok(json)：成功，json 内容由各操作定义。
// - Result::Error(NOT_SUPPORTED)：未知 operation。
// - 其他错误码：见各处理函数。
Result<nlohmann::json> AXCapability::execute(std::string_view      operation,
                                              const nlohmann::json& params)
{
	if (!implement_) {
		return Result<nlohmann::json>::Error(Status::INTERNAL_ERROR);
	}

	AXProviderInterface& provider = *implement_->provider;

	if (operation == "list_windows") {
		return handleListWindows(provider);
	}

	if (operation == "get_ui_tree") {
		return handleGetUITree(provider, params, implement_->default_max_depth);
	}

	if (operation == "click") {
		return handleElementOp(params, [&](std::string_view p) { return provider.click(p); });
	}

	if (operation == "double_click") {
		return handleElementOp(params, [&](std::string_view p) { return provider.doubleClick(p); });
	}

	if (operation == "right_click") {
		return handleElementOp(params, [&](std::string_view p) { return provider.rightClick(p); });
	}

	if (operation == "set_value") {
		return handleSetValue(provider, params);
	}

	if (operation == "focus") {
		return handleElementOp(params, [&](std::string_view p) { return provider.focus(p); });
	}

	if (operation == "scroll") {
		return handleScroll(provider, params);
	}

	if (operation == "key_press") {
		return handleKeyPress(provider, params);
	}

	if (operation == "key_combo") {
		return handleKeyCombo(provider, params);
	}

	if (operation == "activate") {
		return handleActivate(provider, params);
	}

	return Result<nlohmann::json>::Error(Status::NOT_SUPPORTED);
}

} // namespace ax
} // namespace capability
} // namespace clawspan

// 导出工厂函数，供 ModuleManager 动态加载（Phase 3）或直接调用（Phase 1 静态链接）。
CLAWSPAN_MODULE_EXPORT(clawspan::capability::ax::AXCapability)
