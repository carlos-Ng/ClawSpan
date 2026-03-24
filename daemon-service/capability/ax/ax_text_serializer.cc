#include "ax_text_serializer.h"

#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>
#include <unordered_map>

namespace clawspan {
namespace capability {
namespace ax {

// ─── role name mapping ────────────────────────────────────────────────────────

// roleToText 将 macOS AX 角色名转换为 AXT 格式中使用的紧凑小写名称。
//
// 映射表覆盖 macOS AX API 中常见的角色；未收录的角色剥离 "AX" 前缀后全小写。
std::string AXTextSerializer::roleToText(const std::string& ax_role)
{
	static const std::unordered_map<std::string, const char*> kRoleMap = {
	    {"AXWindow",             "window"},
	    {"AXButton",             "button"},
	    {"AXTextField",          "textfield"},
	    {"AXTextArea",           "textarea"},
	    {"AXStaticText",         "text"},
	    {"AXLink",               "link"},
	    {"AXImage",              "image"},
	    {"AXCheckBox",           "checkbox"},
	    {"AXRadioButton",        "radio"},
	    {"AXComboBox",           "combobox"},
	    {"AXPopUpButton",        "popup"},
	    {"AXToolbar",            "toolbar"},
	    {"AXWebArea",            "webarea"},
	    {"AXGroup",              "group"},
	    {"AXScrollArea",         "scrollarea"},
	    {"AXScrollBar",          "scrollbar"},
	    {"AXList",               "list"},
	    {"AXTable",              "table"},
	    {"AXRow",                "row"},
	    {"AXCell",               "cell"},
	    {"AXColumn",             "column"},
	    {"AXHeading",            "heading"},
	    {"AXMenuBar",            "menubar"},
	    {"AXMenuBarItem",        "menubaritem"},
	    {"AXMenu",               "menu"},
	    {"AXMenuItem",           "menuitem"},
	    {"AXTabGroup",           "tabs"},
	    {"AXTab",                "tab"},
	    {"AXSplitGroup",         "splitgroup"},
	    {"AXSplitter",           "splitter"},
	    {"AXSlider",             "slider"},
	    {"AXProgressIndicator",  "progress"},
	    {"AXBusyIndicator",      "spinner"},
	    {"AXStepper",            "stepper"},
	    {"AXOutline",            "outline"},
	    {"AXDisclosureTriangle", "disclosure"},
	    {"AXBrowser",            "browser"},
	    {"AXSheet",              "sheet"},
	    {"AXDrawer",             "drawer"},
	    {"AXPopover",            "popover"},
	    {"AXSeparator",          "separator"},
	    {"AXColorWell",          "colorwell"},
	    {"AXLevelIndicator",     "level"},
	    {"AXLayoutArea",         "layout"},
	    {"AXValueIndicator",     "indicator"},
	    {"AXHandle",             "handle"},
	};

	auto it = kRoleMap.find(ax_role);
	if (it != kRoleMap.end()) {
		return it->second;
	}

	// 兜底：剥离 "AX" 前缀后全小写
	std::string result =
	    (ax_role.size() > 2 && ax_role[0] == 'A' && ax_role[1] == 'X')
	        ? ax_role.substr(2)
	        : ax_role;
	for (char& c : result) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return result;
}

// ─── tag formatting ───────────────────────────────────────────────────────────

// formatTags 将元素的能力与状态渲染为 AXT tag 字符串，附加在节点行末。
//
// INVOCABLE 不输出：button / link 等角色名已隐含"可点击"语义，对 LLM 无需重申。
// 其余 capabilities 及状态标记（disabled / focused / truncated）按需输出。
std::string AXTextSerializer::formatTags(const std::vector<AXElementCap>& caps,
                                          bool                              enabled,
                                          bool                              focused,
                                          bool                              truncated)
{
	std::string tags;
	for (auto cap : caps) {
		switch (cap) {
		case AXElementCap::EDITABLE:   { tags += " [editable]";   break; }
		case AXElementCap::SCROLLABLE: { tags += " [scrollable]"; break; }
		case AXElementCap::TOGGLEABLE: { tags += " [toggleable]"; break; }
		case AXElementCap::INVOCABLE:  { break; } // 角色名已隐含可点击语义
		}
	}
	if (!enabled)  { tags += " [disabled]"; }
	if (focused)   { tags += " [focused]";  }
	if (truncated) { tags += " [+]"; } // 因 max_depth 截断，子节点未展开
	return tags;
}

// ─── path helpers ─────────────────────────────────────────────────────────────

// relPath 从完整 element_path 中提取 window 内相对路径。
//
// 示例："/w1234_0/AXToolbar/AXButton[Back]" → "/AXToolbar/AXButton[Back]"
//        "/w1234_0"                         → ""（窗口根节点本身）
std::string AXTextSerializer::relPath(const std::string& element_path)
{
	if (element_path.empty() || element_path[0] != '/') {
		return element_path;
	}
	auto pos = element_path.find('/', 1); // 找第二个 '/'
	if (pos == std::string::npos) {
		return ""; // 这是窗口根节点
	}
	return element_path.substr(pos);
}

// ─── bounds formatting ────────────────────────────────────────────────────────

std::string AXTextSerializer::formatBounds(const AXBounds& b)
{
	char buf[64];
	std::snprintf(buf, sizeof(buf), " {%.0f,%.0f,%.0f\xc3\x97%.0f}",
	              b.x, b.y, b.width, b.height); // × = U+00D7
	return buf;
}

// ─── pruning helpers ──────────────────────────────────────────────────────────

// isLayoutOnly 返回 true 表示该节点是纯结构容器：无名称、无能力、无值。
// 此类节点在渲染时透明化——自身不输出行，子节点在同一缩进层级继续渲染。
bool AXTextSerializer::isLayoutOnly(const AXElement& elem)
{
	return elem.capabilities.empty() && elem.name.empty() && elem.value.empty();
}

// shouldPrune 返回 true 时，该节点（及其整棵子树）不输出。
//
// 剪枝规则：
//   1. AXSeparator 无名称：纯视觉分隔线，对 Agent 无意义
//   2. AXImage 无名称且无 capabilities：纯装饰图片
bool AXTextSerializer::shouldPrune(const AXElement& elem) const
{
	if (elem.role == "AXSeparator" && elem.name.empty()) {
		return true;
	}
	if (elem.role == "AXImage" && elem.name.empty() && elem.capabilities.empty()) {
		return true;
	}
	return false;
}

// ─── recursive renderer ───────────────────────────────────────────────────────

static constexpr const char* kIndent = "  "; // 每层两个空格

// renderNode 递归渲染单个节点及其子树到 out，返回是否写入了任何内容。
//
// 输出格式（非 layout 节点，单行）：
//   <indent><role> ["<name>"] [tags]  → <rel_path> [bounds]
//   <indent+1>"<value>"（仅在 value 非空时追加一行）
//
// 纯 layout 容器（isLayoutOnly == true）本身不输出行，但透明地向下递归，
// 子节点保持当前缩进深度（depth 不增加）。若子树无任何输出，则返回 false，
// 父节点的子循环可据此感知该子树被完全剪枝，无需额外遍历。
// 这使剪枝复杂度从 O(n²)（逐节点 hasInteractiveDescendant）降为 O(n)。
bool AXTextSerializer::renderNode(const AXElement&    elem,
                                   int                 depth,
                                   std::ostringstream& out,
                                   const Options&      opts) const
{
	if (shouldPrune(elem)) {
		return false;
	}

	// 纯 layout 容器透明化：不写自身行，子节点保持同一深度递归
	if (isLayoutOnly(elem)) {
		bool anyRendered = false;
		for (const auto& child : elem.children) {
			if (renderNode(child, depth, out, opts)) {
				anyRendered = true;
			}
		}
		return anyRendered;
	}

	// ── 缩进 ──
	for (int i = 0; i < depth; ++i) {
		out << kIndent;
	}

	// ── 角色名 ──
	out << roleToText(elem.role);

	// ── 显示名称 ──
	if (!elem.name.empty()) {
		out << " \"" << elem.name << '"';
	}

	// ── 能力与状态 tag ──
	out << formatTags(elem.capabilities, elem.enabled, elem.focused, elem.truncated);

	// ── 相对路径 ──
	const std::string rel = relPath(elem.element_path);
	if (!rel.empty()) {
		out << "  \xe2\x86\x92 " << rel; // → = U+2192
	}

	// ── 坐标（可选）──
	if (opts.include_bounds) {
		out << formatBounds(elem.bounds);
	}

	out << '\n';

	// ── 值（非空时另起一行，缩进多一级）──
	if (!elem.value.empty()) {
		for (int i = 0; i <= depth; ++i) {
			out << kIndent;
		}
		out << '"' << elem.value << "\"\n";
	}

	// ── 子节点 ──
	for (const auto& child : elem.children) {
		renderNode(child, depth + 1, out, opts);
	}

	return true;
}

// ─── public entry points ──────────────────────────────────────────────────────

// serializeWindowList 将窗口列表渲染为 AXT 文本，每个窗口占一行。
//
// 输出字段（按顺序，均在值为空时省略）：
//   window_id  app_name  "title"  [focused]
std::string AXTextSerializer::serializeWindowList(
    const std::vector<AXWindowInfo>& windows) const
{
	std::ostringstream out;
	for (const auto& w : windows) {
		out << w.window_id;
		if (!w.app_name.empty()) {
			out << "  " << w.app_name;
		}
		if (!w.title.empty()) {
			out << "  \"" << w.title << '"';
		}
		if (w.focused) {
			out << "  [focused]";
		}
		out << '\n';
	}
	return out.str();
}

// serialize 将 UI 树渲染为 AXT 紧凑文本。
//
// window_id 自动从 root.element_path 中提取（第一段，即 "/w1234_0" 的 "w1234_0"），
// 无需调用方单独传入，避免两处来源不一致的问题。
//
// 第一行为窗口头部：
//   window <window_id> "<title>"
//
// 后续各行为元素树，根元素（AXWindow）的直接子节点从深度 1 开始渲染。
std::string AXTextSerializer::serialize(const AXElement& root,
                                         const Options&   opts) const
{
	std::ostringstream out;

	// 从 root.element_path（格式 "/w1234_0"）中提取 window_id
	std::string_view ep = root.element_path;
	if (!ep.empty() && ep[0] == '/') {
		ep.remove_prefix(1);
	}
	auto slash = ep.find('/');
	std::string_view window_id = (slash == std::string_view::npos) ? ep : ep.substr(0, slash);

	// 窗口头部行
	out << "window " << window_id;
	if (!root.name.empty()) {
		out << " \"" << root.name << '"';
	}
	if (opts.include_bounds) {
		out << formatBounds(root.bounds);
	}
	out << '\n';

	// 渲染窗口根节点的直接子节点
	for (const auto& child : root.children) {
		renderNode(child, 1, out, opts);
	}

	return out.str();
}

} // namespace ax
} // namespace capability
} // namespace clawspan
