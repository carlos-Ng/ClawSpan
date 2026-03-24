#pragma once

#include "core/base/capability.h"

#include <memory>

namespace clawspan {
namespace capability {
namespace ax {

// AXCapability 是基于 macOS Accessibility API 的 GUI 能力插件。
//
// 它实现 core::CapabilityInterface，通过内部 IAXProvider 适配层与平台 AX API
// 交互，使用 PIMPL 隐藏所有平台相关头文件，使公开头文件可在非 macOS 环境下
// 编译（未来 Windows UIA 适配时无需修改此头文件）。
//
// 支持的操作（通过 execute 调用，operation 字符串区分大小写）：
//
//  查询类（只读）：
//   - list_windows     列出所有可访问的屏幕前台窗口
//   - get_ui_tree      获取指定窗口的完整 UI 元素树
//
//  操作类（写入）：
//   - click            对目标元素执行单击
//   - double_click     对目标元素执行双击
//   - right_click      对目标元素执行右键点击
//   - set_value        向目标元素写入文字值（输入文字）
//   - focus            将键盘焦点设置到目标元素
//   - scroll           对目标元素执行滚动
//   - key_press        模拟按下单个按键
//   - key_combo        模拟按下组合键
//   - activate         将目标窗口激活到屏幕前台
//
// 各操作的 params / 返回 JSON 格式详见 docs/capability-ax-design.md。
class AXCapability : public core::CapabilityInterface
{
public:
	AXCapability();
	~AXCapability() override;

	AXCapability(AXCapability&&) noexcept;
	AXCapability& operator=(AXCapability&&) noexcept;

	// name 返回模块唯一标识 "capability_ax"。
	// 此值同时用于：IPC 路由前缀、capabilities_ 路由表键、TOML name 字段约定。
	//
	// 出参/返回:
	// - 静态字符串 "capability_ax"。
	const char* name() const override;

	// version 返回模块版本字符串。
	//
	// 出参/返回:
	// - 静态字符串，例如 "1.0.0"。
	const char* version() const override;

	// init 初始化 AX 能力模块。
	//
	// 检查 macOS 辅助功能权限，并根据配置初始化内部 IAXProvider 实例。
	//
	// 入参:
	// - config: 模块配置，支持以下可选键：
	//   - "default_max_depth" (int64): get_ui_tree 默认遍历深度，缺省值 10，上限 20。
	//
	// 出参/返回:
	// - Result::Ok()：初始化成功。
	// - Result::Error(ACCESSIBILITY_DENIED)：未获得 macOS 辅助功能权限。
	Result<void> init(const core::ModuleConfig& config) override;

	// release 释放 AX 能力模块持有的所有资源。
	//
	// 调用后本实例不可再使用。由 ModuleManager 在关闭阶段调用。
	void release() override;

	// execute 执行指定 GUI 操作，是外部调用此能力的唯一入口。
	//
	// 入参:
	// - operation: 操作名（见类注释中的支持列表）。
	// - params:    操作参数（JSON 对象），无参数时传空对象 {}。
	//              各操作的具体参数字段详见 docs/capability-ax-design.md。
	//
	// 出参/返回:
	// - Result::Ok(json)：操作成功，json 内容由各操作自行定义。
	// - Result::Error(NOT_SUPPORTED)：operation 不在支持列表中。
	// - Result::Error(ACCESSIBILITY_DENIED)：未获得辅助功能权限。
	// - Result::Error(WINDOW_NOT_FOUND)：window_id 对应窗口不存在或已关闭。
	// - Result::Error(ELEMENT_NOT_FOUND)：element_path 无法定位到对应元素。
	Result<nlohmann::json> execute(std::string_view operation,
	                               const nlohmann::json& params) override;

private:
	struct Implement;
	std::unique_ptr<Implement> implement_;
};

} // namespace ax
} // namespace capability
} // namespace clawspan
