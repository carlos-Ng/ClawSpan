#pragma once

#include "ax_element.h"
#include "common/error.h"

#include <string>
#include <string_view>
#include <vector>

namespace clawspan {
namespace capability {
namespace ax {

// AXProviderInterface 是平台无关的 Accessibility 操作适配接口，将 AXCapability 与
// 具体平台的 API 实现解耦。
//
// 当前实现:  AXProviderMacOS — 基于 macOS Accessibility API（ApplicationServices.framework）
// 未来扩展:  AXProviderUIA   — 基于 Windows UI Automation
//
// AXCapability::Implement 持有 AXProviderInterface 的 unique_ptr，在 init() 时根据平台
// 创建对应实现，其余代码只依赖此接口，对平台 API 无感知。
//
// AXProviderInterface 不缓存窗口/元素的长期状态；window_id 由 listWindows 分配并在
// 会话内有效，element_path 由 getUITree 生成并在同一窗口的生命周期内有效。
class AXProviderInterface
{
public:
	virtual ~AXProviderInterface() = default;

	// isPermissionGranted 检查当前进程是否已获得辅助功能权限。
	//
	// 出参/返回:
	// - true：已获得权限，可调用其他接口。
	// - false：未获得权限，需引导用户在"系统设置 → 隐私与安全性 → 辅助功能"中开启。
	virtual bool isPermissionGranted() const = 0;

	// listWindows 枚举当前屏幕上所有前台可访问窗口，并分配会话内唯一的 window_id。
	//
	// 每次调用都会重新枚举，window_id 可能因窗口打开/关闭而变化，
	// 调用方在每次需要操作前应重新获取。
	//
	// 出参/返回:
	// - Result::Ok(vector<AXWindowInfo>)：成功，列表可为空（无可访问窗口）。
	// - Result::Error(ACCESSIBILITY_DENIED)：未获得辅助功能权限。
	virtual Result<std::vector<AXWindowInfo>> listWindows() = 0;

	// getUITree 获取指定窗口的 UI 元素树。
	//
	// 遍历从窗口根元素（role = AXWindow）开始，递归向下直到 max_depth 或叶节点。
	// 超出 max_depth 的子树以 truncated=true 标记（体现在 AXT 输出的 [+] 标记中）。
	//
	// 入参:
	// - window_id: 由 listWindows 返回的窗口 ID。
	// - max_depth: 最大遍历深度，建议值 10，有效范围 [1, 20]。
	//
	// 出参/返回:
	// - Result::Ok(AXElement)：成功，返回窗口根元素（含子树）。
	// - Result::Error(WINDOW_NOT_FOUND)：window_id 不存在或对应窗口已关闭。
	// - Result::Error(ACCESSIBILITY_DENIED)：未获得辅助功能权限。
	virtual Result<AXElement> getUITree(std::string_view window_id,
	                                    int              max_depth) = 0;

	// click 对指定路径的元素执行单击。
	//
	// 执行策略（分层降级，L1 失败自动尝试 L2）：
	//   L1: AXUIElementPerformAction(kAXPressAction)     — 语义级，不移动鼠标
	//   L2: CGEvent 鼠标点击（从 bounds 计算中心坐标）  — 会移动真实鼠标，作为兜底
	//
	// 入参:
	// - element_path: 由 getUITree 返回的元素路径。
	//
	// 出参/返回:
	// - Result::Ok()：点击成功。
	// - Result::Error(ELEMENT_NOT_FOUND)：路径无法定位到元素。
	// - Result::Error(WINDOW_NOT_FOUND)：路径中的 window_id 对应窗口不存在。
	virtual Result<void> click(std::string_view element_path) = 0;

	// doubleClick 对指定路径的元素执行双击。
	//
	// 入参:
	// - element_path: 由 getUITree 返回的元素路径。
	//
	// 出参/返回:
	// - Result::Ok()：双击成功。
	// - Result::Error(ELEMENT_NOT_FOUND)：路径无法定位到元素。
	virtual Result<void> doubleClick(std::string_view element_path) = 0;

	// rightClick 对指定路径的元素执行右键点击。
	//
	// 入参:
	// - element_path: 由 getUITree 返回的元素路径。
	//
	// 出参/返回:
	// - Result::Ok()：右键点击成功。
	// - Result::Error(ELEMENT_NOT_FOUND)：路径无法定位到元素。
	virtual Result<void> rightClick(std::string_view element_path) = 0;

	// setValue 向目标元素写入文字值。
	//
	// 执行策略（分层降级）：
	//   L1: AXUIElementSetAttributeValue(kAXValueAttribute)
	//   L2: focus 后模拟键盘输入（CGEvent）作为兜底
	//
	// 入参:
	// - element_path: 目标元素路径，应为可编辑元素（capabilities 含 EDITABLE）。
	// - value:        要写入的文字内容（UTF-8）。
	//
	// 出参/返回:
	// - Result::Ok()：设置成功。
	// - Result::Error(ELEMENT_NOT_FOUND)：路径无法定位到元素。
	virtual Result<void> setValue(std::string_view element_path,
	                              std::string_view value) = 0;

	// focus 将键盘焦点移动到目标元素。
	//
	// 入参:
	// - element_path: 目标元素路径。
	//
	// 出参/返回:
	// - Result::Ok()：焦点设置成功。
	// - Result::Error(ELEMENT_NOT_FOUND)：路径无法定位到元素。
	virtual Result<void> focus(std::string_view element_path) = 0;

	// scroll 对目标元素执行滚动操作。
	//
	// 入参:
	// - element_path: 目标元素路径，应为可滚动元素（capabilities 含 SCROLLABLE）。
	// - direction:    滚动方向，取值：up / down / left / right。
	// - amount:       滚动量（单位：像素），建议值 100~300，必须 > 0。
	//
	// 出参/返回:
	// - Result::Ok()：滚动成功。
	// - Result::Error(ELEMENT_NOT_FOUND)：路径无法定位到元素。
	// - Result::Error(INVALID_ARGUMENT)：direction 不在支持列表或 amount <= 0。
	virtual Result<void> scroll(std::string_view element_path,
	                            std::string_view direction,
	                            int              amount) = 0;

	// keyPress 模拟按下并释放单个按键。
	//
	// 支持的 key 名称（区分大小写）：
	//   Return, Escape, Space, Tab, Delete, BackSpace,
	//   Up, Down, Left, Right,
	//   Home, End, PageUp, PageDown,
	//   F1 ~ F12,
	//   单个可打印字符（如 "a"、"1"、"."）
	//
	// 入参:
	// - key:       按键名称。
	// - window_id: 目标窗口 ID（可选）。传入时事件定向投递到该窗口所属进程，
	//              不抢夺前台焦点；留空则发往当前焦点窗口。
	//
	// 出参/返回:
	// - Result::Ok()：按键成功。
	// - Result::Error(NOT_SUPPORTED)：key 名称不在支持列表中。
	virtual Result<void> keyPress(std::string_view key,
	                              std::string_view window_id) = 0;

	// keyCombo 模拟按下组合键（修饰键 + 主键）。
	//
	// 支持的修饰键名称：Cmd（⌘）、Ctrl、Alt（⌥）、Shift。
	// 修饰键必须放在列表前面，主键放最后一个。
	//
	// 示例：
	//   {"Cmd", "C"}          → ⌘C（复制）
	//   {"Cmd", "Shift", "Z"} → ⌘⇧Z（重做）
	//
	// 入参:
	// - keys:      按键名称列表，至少包含 2 个元素，最后一个为主键。
	// - window_id: 目标窗口 ID（可选）。传入时事件定向投递到该窗口所属进程，
	//              不抢夺前台焦点；留空则发往当前焦点窗口。
	//
	// 出参/返回:
	// - Result::Ok()：组合键成功。
	// - Result::Error(NOT_SUPPORTED)：包含不支持的按键名称。
	// - Result::Error(INVALID_ARGUMENT)：keys 列表为空或仅含修饰键。
	virtual Result<void> keyCombo(const std::vector<std::string>& keys,
	                               std::string_view                window_id) = 0;

	// activate 将目标窗口激活到屏幕前台并获得键盘焦点。
	//
	// 入参:
	// - window_id: 由 listWindows 返回的窗口 ID。
	//
	// 出参/返回:
	// - Result::Ok()：激活成功。
	// - Result::Error(WINDOW_NOT_FOUND)：window_id 对应窗口不存在或已关闭。
	virtual Result<void> activate(std::string_view window_id) = 0;
};

} // namespace ax
} // namespace capability
} // namespace clawspan
