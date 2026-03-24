#pragma once

#include "ax_element.h"

#include <sstream>
#include <string>
#include <string_view>

namespace clawspan {
namespace capability {
namespace ax {

// AXTextSerializerOptions 控制 AXTextSerializer 的渲染行为。
struct AXTextSerializerOptions
{
	bool include_bounds = false; // 是否在每行末尾附带 bounds，默认不输出
};

// AXTextSerializer 将 AX 元素树序列化为 AXT 紧凑文本格式，供 LLM 消费。
//
// AXT 格式核心特点：
//   - 用缩进表示层级，每层两个空格
//   - 角色名去 AX 前缀并小写（AXButton → button）
//   - 默认不输出 bounds（LLM 决策不需要坐标）
//   - 智能剪枝：纯 layout 容器透明化（自身不输出行，子节点保持原深度），
//               纯装饰元素不输出
//   - token 消耗约为等价 JSON 的 1/5 ~ 1/4
//
// AXElement 只负责存储数据，AXTextSerializer 只负责文本渲染，职责分离。
//
// 使用示例：
//   AXTextSerializer ser;
//   std::string text = ser.serialize(root_element);
class AXTextSerializer
{
public:
	using Options = AXTextSerializerOptions;

	// serializeWindowList 将 list_windows 返回的窗口列表渲染为 AXT 文本。
	//
	// 输出格式（每个窗口占一行）：
	//   <window_id>  <app_name>  "<title>"  [focused]
	//
	// 示例：
	//   w1234_0  Safari         "Google - Safari"  [focused]
	//   w5678_1  Google Chrome  "GitHub"
	//   w9012_0  Finder         "Downloads"
	//
	// 入参:
	// - windows: listWindows() 返回的窗口信息列表。
	//
	// 出参/返回:
	// - AXT 格式的 UTF-8 字符串（若列表为空则返回空字符串）。
	std::string serializeWindowList(const std::vector<AXWindowInfo>& windows) const;

	// serialize 将 UI 树渲染为 AXT 文本。
	//
	// window_id 自动从 root.element_path 的第一段提取，无需调用方传入。
	//
	// 入参:
	// - root: get_ui_tree 返回的根元素（role = AXWindow）。
	// - opts: 渲染选项。
	//
	// 出参/返回:
	// - AXT 格式的 UTF-8 字符串。
	std::string serialize(const AXElement& root,
	                      const Options&   opts = {}) const;

private:
	// renderNode 递归渲染单个节点及其子树到 out。
	//
	// 返回值：true 表示本次调用向 out 写入了至少一行内容。
	// 纯 layout 容器（无名称、无能力、无值）本身不写行，但透明地向下递归，
	// 若子树有输出则返回 true，否则返回 false（整棵子树被静默剪枝）。
	bool renderNode(const AXElement&    elem,
	                int                 depth,
	                std::ostringstream& out,
	                const Options&      opts) const;

	// shouldPrune 返回 true 时，该节点及其整棵子树不输出。
	bool shouldPrune(const AXElement& elem) const;

	// isLayoutOnly 返回 true 表示该节点是纯结构容器（无名称、无能力、无值），
	// 渲染时透明化处理（自身不输出行，子节点在同一深度继续渲染）。
	static bool isLayoutOnly(const AXElement& elem);

	// roleToText 将 AX 角色名规范化为小写短名称。
	static std::string roleToText(const std::string& ax_role);

	// formatTags 将 capabilities、enabled、focused、truncated 格式化为 tag 字符串。
	static std::string formatTags(const std::vector<AXElementCap>& caps,
	                               bool                              enabled,
	                               bool                              focused,
	                               bool                              truncated);

	// relPath 从完整 element_path 中提取 window 内相对路径。
	static std::string relPath(const std::string& element_path);

	// formatBounds 将 AXBounds 格式化为紧凑字符串。
	static std::string formatBounds(const AXBounds& b);
};

} // namespace ax
} // namespace capability
} // namespace clawspan
