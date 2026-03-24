#pragma once

#include "common/error.h"

#include <string>
#include <vector>

namespace clawspan {
namespace capability {
namespace ax {

// AXBounds 描述 UI 元素在屏幕全局坐标系中的位置和大小（单位：点）。
// 使用 double 与 macOS 原生 CGFloat（64 位平台为 double）保持精度，
// 避免多显示器/HiDPI 大坐标值（>2000）时的亚像素偏差。
struct AXBounds
{
	double x      = 0.0;
	double y      = 0.0;
	double width  = 0.0;
	double height = 0.0;
};

// AXElementCap 描述 UI 元素支持的操作能力，供 Agent 判断可对其执行哪些操作。
enum class AXElementCap
{
	INVOCABLE,  // 可点击（支持 click / double_click 操作）
	EDITABLE,   // 可输入（支持 set_value 操作）
	SCROLLABLE, // 可滚动（支持 scroll 操作）
	TOGGLEABLE, // 可切换状态（复选框等，支持 click 操作）
};

// AXElement 描述 UI 树中的单个元素，是 get_ui_tree 返回结果的基本单元。
//
// element_path 是元素的全局寻址路径，格式：
//   /window_id/role[identifier]/role[identifier]/...
//
// identifier 的优先级：
//   1. kAXIdentifierAttribute（应用开发者设置的 accessibility id，最稳定）
//   2. name（元素的显示名称，较稳定）
//   3. index（兜底，格式为整数，UI 变更后可能失效）
//
// 示例：
//   /w1234_0/AXToolbar/AXButton[Back]      ← 名称作为 identifier
//   /w1234_0/AXWebArea/AXTextField[search] ← ax identifier 作为 identifier
//   /w1234_0/AXGroup[0]/AXButton[2]        ← 无名称，退回 index
struct AXElement
{
	std::string               element_path;
	std::string               role;
	std::string               name;
	std::string               value;
	AXBounds                  bounds;
	bool                      enabled     = true;
	bool                      focused     = false;
	std::vector<AXElementCap> capabilities;
	std::vector<AXElement>    children;
	bool                      truncated   = false; // 因 max_depth 截断，仍有未展开子节点
};

// AXWindowInfo 描述一个可访问的屏幕前台窗口，是 list_windows 返回结果的基本单元。
//
// window_id 格式：w<pid>_<index>，例如 "w1234_0"。
// 其中 index 是同一应用内的窗口序号（0-based，按 AX API 返回顺序）。
// window_id 在本次 daemon 会话内唯一，跨会话不保证稳定。
struct AXWindowInfo
{
	std::string window_id;
	std::string app_name;
	std::string bundle_id;
	std::string title;
	int         pid     = 0;
	AXBounds    bounds;
	bool        focused = false;
};

} // namespace ax
} // namespace capability
} // namespace clawspan
