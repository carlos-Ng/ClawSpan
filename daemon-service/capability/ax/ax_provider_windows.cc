#include "ax_provider_windows.h"
#include "ax_element.h"

#include "common/log.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>
// WIN32_LEAN_AND_MEAN 会跳过 ole2.h（其中包含 objbase.h）。
// objbase.h 定义了 `interface` 宏，UIAutomationCore.h 中的 COM 前向声明依赖它，
// 因此必须在 uiautomation.h 之前显式包含。
#include <ole2.h>
#include <objbase.h>
#include <uiautomation.h>
#include <psapi.h>

namespace clawspan {
namespace capability {
namespace ax {

// ─── RAII 辅助 ───────────────────────────────────────────────────────────────

// WinHandle HANDLE 的 RAII 包装，对应 macOS CFRef<T>。
// 自动在析构时调用 CloseHandle，防止句柄泄漏。
struct WinHandle
{
	HANDLE h = INVALID_HANDLE_VALUE;

	explicit WinHandle(HANDLE h_ = INVALID_HANDLE_VALUE) : h(h_) {}
	~WinHandle() { if (h != INVALID_HANDLE_VALUE && h != nullptr) ::CloseHandle(h); }

	WinHandle(const WinHandle&)            = delete;
	WinHandle& operator=(const WinHandle&) = delete;
	WinHandle(WinHandle&& o) noexcept : h(o.h) { o.h = INVALID_HANDLE_VALUE; }

	HANDLE get() const { return h; }
	explicit operator bool() const { return h != INVALID_HANDLE_VALUE && h != nullptr; }
};

// ComRef<T> COM 接口的 RAII 包装，对应 macOS CFRef<T>。
// 自动在析构时调用 Release，防止 COM 接口引用计数泄漏。
template <typename T>
struct ComRef
{
	T* ptr = nullptr;

	explicit ComRef(T* p = nullptr) : ptr(p) {}
	~ComRef() { if (ptr) ptr->Release(); }

	ComRef(const ComRef&)            = delete;
	ComRef& operator=(const ComRef&) = delete;

	ComRef(ComRef&& o) noexcept : ptr(o.ptr) { o.ptr = nullptr; }
	ComRef& operator=(ComRef&& o) noexcept
	{
		if (this != &o) {
			if (ptr) ptr->Release();
			ptr   = o.ptr;
			o.ptr = nullptr;
		}
		return *this;
	}

	T*  operator->() const { return ptr; }
	T*  get()        const { return ptr; }
	T** put()
	{
		if (ptr) { ptr->Release(); ptr = nullptr; }
		return &ptr;
	}
	explicit operator bool()              const { return ptr != nullptr; }
	bool operator==(std::nullptr_t)       const { return ptr == nullptr; }
	bool operator!=(std::nullptr_t)       const { return ptr != nullptr; }
};

// ComStr BSTR 的 RAII 包装，防止 BSTR 内存泄漏。
struct ComStr
{
	BSTR bstr = nullptr;

	explicit ComStr(BSTR b = nullptr) : bstr(b) {}
	~ComStr() { if (bstr) ::SysFreeString(bstr); }

	ComStr(const ComStr&)            = delete;
	ComStr& operator=(const ComStr&) = delete;

	// toUtf8 将 BSTR（宽字符串）转换为 UTF-8 std::string。
	std::string toUtf8() const
	{
		if (!bstr || ::SysStringLen(bstr) == 0) {
			return {};
		}
		int size = ::WideCharToMultiByte(CP_UTF8, 0, bstr, -1, nullptr, 0, nullptr, nullptr);
		if (size <= 0) {
			return {};
		}
		std::string result(static_cast<size_t>(size - 1), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, bstr, -1, result.data(), size, nullptr, nullptr);
		return result;
	}

	BSTR get() const { return bstr; }
};

// ComScopeInit 封装 COM 线程初始化/反初始化，以 RAII 保证配对调用。
struct ComScopeInit
{
	HRESULT hr;
	explicit ComScopeInit() : hr(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
	~ComScopeInit() { if (SUCCEEDED(hr) || hr == S_FALSE) ::CoUninitialize(); }
	bool ok() const { return SUCCEEDED(hr) || hr == S_FALSE; }
};

// ─── 常量 ─────────────────────────────────────────────────────────────────────

// 激活窗口后等待其获得前台焦点的时间（毫秒）。
// SendInput 需要目标窗口已获得焦点才能将事件定向投递，
// 50ms 在常规硬件上足够；若遇极端高负载系统可通过配置适当增大。
static constexpr DWORD kActivateWindowDelayMs = 50;

// ─── 内部工具函数 ─────────────────────────────────────────────────────────────

// wstrToUtf8 将宽字符串转换为 UTF-8 字符串。
static std::string wstrToUtf8(const std::wstring& wstr)
{
	if (wstr.empty()) {
		return {};
	}
	int size = ::WideCharToMultiByte(CP_UTF8, 0,
	                                  wstr.data(), static_cast<int>(wstr.size()),
	                                  nullptr, 0, nullptr, nullptr);
	if (size <= 0) {
		return {};
	}
	std::string result(static_cast<size_t>(size), '\0');
	::WideCharToMultiByte(CP_UTF8, 0,
	                       wstr.data(), static_cast<int>(wstr.size()),
	                       result.data(), size, nullptr, nullptr);
	return result;
}

// utf8ToWstr 将 UTF-8 字符串转换为宽字符串（供 SetValue 等 UIA 接口使用）。
static std::wstring utf8ToWstr(std::string_view utf8)
{
	if (utf8.empty()) {
		return {};
	}
	int size = ::MultiByteToWideChar(CP_UTF8, 0,
	                                  utf8.data(), static_cast<int>(utf8.size()),
	                                  nullptr, 0);
	if (size <= 0) {
		return {};
	}
	std::wstring result(static_cast<size_t>(size), L'\0');
	::MultiByteToWideChar(CP_UTF8, 0,
	                       utf8.data(), static_cast<int>(utf8.size()),
	                       result.data(), size);
	return result;
}

// hwndToWindowId 将 HWND 编码为 window_id 字符串，格式："w<pid>_<hwnd_hex>"。
// 与 macOS 版本 "w<pid>_<index>" 保持相同前缀，便于上层代码兼容两平台。
static std::string hwndToWindowId(HWND hwnd)
{
	DWORD pid = 0;
	::GetWindowThreadProcessId(hwnd, &pid);
	char buf[64];
	snprintf(buf, sizeof(buf), "w%lu_%llX",
	         static_cast<unsigned long>(pid),
	         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
	return buf;
}

// parseWindowId 解析 "w<pid>_<hwnd_hex>" 格式，失败返回 std::nullopt。
static std::optional<HWND> parseWindowId(std::string_view s)
{
	if (s.empty() || s[0] != 'w') {
		return std::nullopt;
	}
	s.remove_prefix(1);
	auto sep = s.find('_');
	if (sep == std::string_view::npos) {
		return std::nullopt;
	}
	auto hwnd_part = s.substr(sep + 1);
	uintptr_t hwnd_val = 0;
	auto [end, ec] = std::from_chars(hwnd_part.data(),
	                                  hwnd_part.data() + hwnd_part.size(),
	                                  hwnd_val, 16);
	if (ec != std::errc{}) {
		return std::nullopt;
	}
	return reinterpret_cast<HWND>(hwnd_val);
}

// splitPath 将 "/w1234_1A2B/role[id]/..." 拆分为各路径分量。
static std::vector<std::string> splitPath(std::string_view path)
{
	std::vector<std::string> parts;
	if (path.empty()) {
		return parts;
	}
	if (path[0] == '/') {
		path.remove_prefix(1);
	}
	while (!path.empty()) {
		auto pos = path.find('/');
		if (pos == std::string_view::npos) {
			parts.emplace_back(path);
			break;
		}
		if (pos > 0) {
			parts.emplace_back(path.substr(0, pos));
		}
		path.remove_prefix(pos + 1);
	}
	return parts;
}

// parseComponent 将 "Button[Back]" 拆分为 ("Button", "Back")，无方括号时第二元素为空。
static std::pair<std::string, std::string> parseComponent(std::string_view comp)
{
	auto open  = comp.find('[');
	auto close = comp.rfind(']');
	if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
		return {std::string(comp), {}};
	}
	return {
		std::string(comp.substr(0, open)),
		std::string(comp.substr(open + 1, close - open - 1)),
	};
}

// controlTypeToRole 将 UIA ControlType 枚举转换为可读角色字符串。
// 格式尽量对应 macOS AX 角色名（前缀 AX），使序列化输出一致。
static std::string controlTypeToRole(CONTROLTYPEID ctrl_type)
{
	static const std::unordered_map<CONTROLTYPEID, std::string> kMap = {
		{UIA_ButtonControlTypeId,       "AXButton"},
		{UIA_CalendarControlTypeId,     "AXCalendar"},
		{UIA_CheckBoxControlTypeId,     "AXCheckBox"},
		{UIA_ComboBoxControlTypeId,     "AXComboBox"},
		{UIA_EditControlTypeId,         "AXTextField"},
		{UIA_HyperlinkControlTypeId,    "AXLink"},
		{UIA_ImageControlTypeId,        "AXImage"},
		{UIA_ListItemControlTypeId,     "AXCell"},
		{UIA_ListControlTypeId,         "AXList"},
		{UIA_MenuControlTypeId,         "AXMenu"},
		{UIA_MenuBarControlTypeId,      "AXMenuBar"},
		{UIA_MenuItemControlTypeId,     "AXMenuItem"},
		{UIA_ProgressBarControlTypeId,  "AXProgressIndicator"},
		{UIA_RadioButtonControlTypeId,  "AXRadioButton"},
		{UIA_ScrollBarControlTypeId,    "AXScrollBar"},
		{UIA_SliderControlTypeId,       "AXSlider"},
		{UIA_SpinnerControlTypeId,      "AXIncrementor"},
		{UIA_StatusBarControlTypeId,    "AXStatusBar"},
		{UIA_TabControlTypeId,          "AXTabGroup"},
		{UIA_TabItemControlTypeId,      "AXRadioButton"},
		{UIA_TextControlTypeId,         "AXStaticText"},
		{UIA_ToolBarControlTypeId,      "AXToolbar"},
		{UIA_ToolTipControlTypeId,      "AXHelpTag"},
		{UIA_TreeControlTypeId,         "AXOutline"},
		{UIA_TreeItemControlTypeId,     "AXRow"},
		{UIA_CustomControlTypeId,       "AXGroup"},
		{UIA_GroupControlTypeId,        "AXGroup"},
		{UIA_ThumbControlTypeId,        "AXValueIndicator"},
		{UIA_DataGridControlTypeId,     "AXTable"},
		{UIA_DataItemControlTypeId,     "AXRow"},
		{UIA_DocumentControlTypeId,     "AXTextArea"},
		{UIA_SplitButtonControlTypeId,  "AXButton"},
		{UIA_WindowControlTypeId,       "AXWindow"},
		{UIA_PaneControlTypeId,         "AXScrollArea"},
		{UIA_HeaderControlTypeId,       "AXGroup"},
		{UIA_HeaderItemControlTypeId,   "AXCell"},
		{UIA_TableControlTypeId,        "AXTable"},
		{UIA_TitleBarControlTypeId,     "AXStaticText"},
		{UIA_SeparatorControlTypeId,    "AXSplitter"},
		{UIA_SemanticZoomControlTypeId, "AXGroup"},
		{UIA_AppBarControlTypeId,       "AXGroup"},
	};
	auto it = kMap.find(ctrl_type);
	if (it != kMap.end()) {
		return it->second;
	}
	return "AXGroup";
}

// uiaGetBstr 从 UIA 元素的属性中读取 BSTR 值，失败返回空字符串。
static std::string uiaGetBstr(IUIAutomationElement* elem, PROPERTYID prop_id)
{
	if (!elem) {
		return {};
	}
	VARIANT var{};
	::VariantInit(&var);
	if (FAILED(elem->GetCurrentPropertyValue(prop_id, &var))) {
		return {};
	}
	std::string result;
	if (var.vt == VT_BSTR && var.bstrVal) {
		ComStr bs(var.bstrVal);
		var.bstrVal = nullptr; // 转移所有权到 ComStr，避免双重释放
		result = bs.toUtf8();
	}
	::VariantClear(&var);
	return result;
}

// uiaGetBool 从 UIA 元素的属性中读取 BOOL 值，失败返回 default_val。
static bool uiaGetBool(IUIAutomationElement* elem, PROPERTYID prop_id, bool default_val)
{
	if (!elem) {
		return default_val;
	}
	VARIANT var{};
	::VariantInit(&var);
	if (FAILED(elem->GetCurrentPropertyValue(prop_id, &var))) {
		return default_val;
	}
	bool result = default_val;
	if (var.vt == VT_BOOL) {
		result = (var.boolVal != VARIANT_FALSE);
	}
	::VariantClear(&var);
	return result;
}

// uiaGetBounds 从 UIA 元素读取屏幕坐标矩形。
static AXBounds uiaGetBounds(IUIAutomationElement* elem)
{
	if (!elem) {
		return {};
	}
	RECT rect{};
	if (FAILED(elem->get_CurrentBoundingRectangle(&rect))) {
		return {};
	}
	return {
		static_cast<double>(rect.left),
		static_cast<double>(rect.top),
		static_cast<double>(rect.right - rect.left),
		static_cast<double>(rect.bottom - rect.top),
	};
}

// uiaGetCapabilities 根据 UIA 模式支持情况推断元素能力。
static std::vector<AXElementCap> uiaGetCapabilities(IUIAutomationElement* elem,
                                                     IUIAutomation*         uia)
{
	if (!elem || !uia) {
		return {};
	}
	std::vector<AXElementCap> caps;

	// 支持 InvokePattern → INVOCABLE（可点击）
	ComRef<IUIAutomationInvokePattern> invoke;
	if (SUCCEEDED(elem->GetCurrentPattern(UIA_InvokePatternId,
	                                       reinterpret_cast<IUnknown**>(invoke.put()))) && invoke) {
		caps.push_back(AXElementCap::INVOCABLE);
	}

	// 支持 ValuePattern 且 IsReadOnly=false → EDITABLE
	ComRef<IUIAutomationValuePattern> value_pat;
	if (SUCCEEDED(elem->GetCurrentPattern(UIA_ValuePatternId,
	                                       reinterpret_cast<IUnknown**>(value_pat.put()))) &&
	    value_pat) {
		BOOL readonly = TRUE;
		value_pat->get_CurrentIsReadOnly(&readonly);
		if (!readonly) {
			caps.push_back(AXElementCap::EDITABLE);
		}
	}

	// 支持 ScrollPattern → SCROLLABLE
	ComRef<IUIAutomationScrollPattern> scroll_pat;
	if (SUCCEEDED(elem->GetCurrentPattern(UIA_ScrollPatternId,
	                                       reinterpret_cast<IUnknown**>(scroll_pat.put()))) &&
	    scroll_pat) {
		caps.push_back(AXElementCap::SCROLLABLE);
	}

	// 支持 TogglePattern → TOGGLEABLE
	ComRef<IUIAutomationTogglePattern> toggle_pat;
	if (SUCCEEDED(elem->GetCurrentPattern(UIA_TogglePatternId,
	                                       reinterpret_cast<IUnknown**>(toggle_pat.put()))) &&
	    toggle_pat) {
		caps.push_back(AXElementCap::TOGGLEABLE);
	}

	return caps;
}

// ─── 元素树递归构建 ───────────────────────────────────────────────────────────

// buildIdentifier 根据 AutomationId > Name > 同类型兄弟节点序号，确定元素路径标识符。
// 优先级与 macOS AXIdentifier > Title > Index 一致。
static std::string buildIdentifier(IUIAutomationElement* elem, int same_type_index)
{
	// 优先 AutomationId（对应 macOS AXIdentifier）
	std::string aid = uiaGetBstr(elem, UIA_AutomationIdPropertyId);
	if (!aid.empty()) {
		return aid;
	}
	// 其次 Name（对应 macOS AXTitle）
	std::string name = uiaGetBstr(elem, UIA_NamePropertyId);
	if (!name.empty()) {
		return name;
	}
	// 最后 index（不稳定，UI 变更后可能失效）
	return std::to_string(same_type_index);
}

// buildElement 递归构建以 elem 为根的 AXElement 子树。
static AXElement buildElement(IUIAutomationElement*    elem,
                               IUIAutomation*           uia,
                               IUIAutomationTreeWalker* walker,
                               std::string_view         parent_path,
                               int                      depth,
                               int                      max_depth)
{
	AXElement result;
	result.element_path = std::string(parent_path);

	CONTROLTYPEID ctrl_type = UIA_GroupControlTypeId;
	elem->get_CurrentControlType(&ctrl_type);
	result.role = controlTypeToRole(ctrl_type);

	result.name    = uiaGetBstr(elem, UIA_NamePropertyId);
	result.value   = uiaGetBstr(elem, UIA_ValueValuePropertyId);
	result.bounds  = uiaGetBounds(elem);
	result.enabled = uiaGetBool(elem, UIA_IsEnabledPropertyId, true);
	result.focused = uiaGetBool(elem, UIA_HasKeyboardFocusPropertyId, false);
	result.capabilities = uiaGetCapabilities(elem, uia);

	if (depth >= max_depth) {
		// 检查是否存在未展开的子节点
		ComRef<IUIAutomationElement> first_child;
		if (SUCCEEDED(walker->GetFirstChildElement(elem, first_child.put())) && first_child) {
			result.truncated = true;
		}
		return result;
	}

	// 遍历子元素，按 role 类型分组跟踪序号
	std::unordered_map<CONTROLTYPEID, int> type_counts;

	ComRef<IUIAutomationElement> child;
	if (FAILED(walker->GetFirstChildElement(elem, child.put())) || !child) {
		return result;
	}

	while (child) {
		CONTROLTYPEID child_type = UIA_GroupControlTypeId;
		child->get_CurrentControlType(&child_type);
		int idx = type_counts[child_type]++;

		std::string child_role = controlTypeToRole(child_type);
		std::string child_id   = buildIdentifier(child.get(), idx);
		std::string child_path = std::string(parent_path) + "/" + child_role + "[" + child_id + "]";

		result.children.push_back(
		    buildElement(child.get(), uia, walker, child_path, depth + 1, max_depth));

		ComRef<IUIAutomationElement> next;
		if (FAILED(walker->GetNextSiblingElement(child.get(), next.put())) || !next) {
			break;
		}
		child = std::move(next);
	}

	return result;
}

// ─── 元素路径查找 ─────────────────────────────────────────────────────────────

// findElementByPath 在 UIA 树中按路径定位元素。
//
// 路径格式：/w<pid>_<hwnd_hex>/role[id]/role[id]/...
// 解析规则：第一个分量为 window_id，随后每个分量为 role[identifier]，
//           按 role + 名称/AutomationId/序号 定位子元素。
static ComRef<IUIAutomationElement> findElementByPath(
    std::string_view     element_path,
    IUIAutomation*       uia,
    IUIAutomationTreeWalker* walker)
{
	auto parts = splitPath(element_path);
	if (parts.size() < 2) {
		return ComRef<IUIAutomationElement>{};
	}

	// 第一个分量是 window_id，解析出 HWND
	auto hwnd_opt = parseWindowId(parts[0]);
	if (!hwnd_opt || !::IsWindow(*hwnd_opt)) {
		return ComRef<IUIAutomationElement>{};
	}
	HWND hwnd = *hwnd_opt;

	ComRef<IUIAutomationElement> current;
	if (FAILED(uia->ElementFromHandle(hwnd, current.put())) || !current) {
		return ComRef<IUIAutomationElement>{};
	}

	// 按路径分量逐层定位
	for (size_t i = 1; i < parts.size(); ++i) {
		auto [role, ident] = parseComponent(parts[i]);

		// 尝试将 ident 解析为整数（index）
		int target_index = -1;
		int parsed_idx   = 0;
		auto [end, ec] = std::from_chars(ident.data(), ident.data() + ident.size(), parsed_idx);
		if (ec == std::errc{} && end == ident.data() + ident.size()) {
			target_index = parsed_idx;
		}

		// 在 current 的子节点中查找匹配的元素
		ComRef<IUIAutomationElement> found;
		std::unordered_map<CONTROLTYPEID, int> type_counts;

		ComRef<IUIAutomationElement> child;
		if (FAILED(walker->GetFirstChildElement(current.get(), child.put())) || !child) {
			return ComRef<IUIAutomationElement>{};
		}

		while (child) {
			CONTROLTYPEID child_type = UIA_GroupControlTypeId;
			child->get_CurrentControlType(&child_type);
			std::string child_role = controlTypeToRole(child_type);
			int idx = type_counts[child_type]++;

			if (child_role == role) {
				if (target_index >= 0) {
					// 按 index 匹配
					if (idx == target_index) {
						found = std::move(child);
						break;
					}
				} else {
					// 按 AutomationId 或 Name 匹配
					std::string aid  = uiaGetBstr(child.get(), UIA_AutomationIdPropertyId);
					std::string name = uiaGetBstr(child.get(), UIA_NamePropertyId);
					if (aid == ident || name == ident) {
						found = std::move(child);
						break;
					}
				}
			}

			ComRef<IUIAutomationElement> next;
			if (FAILED(walker->GetNextSiblingElement(child.get(), next.put())) || !next) {
				break;
			}
			child = std::move(next);
		}

		if (!found) {
			return ComRef<IUIAutomationElement>{};
		}
		current = std::move(found);
	}
	return current;
}

// ─── 鼠标/键盘 InputEvent 辅助 ───────────────────────────────────────────────

// mouseBoundsCenter 根据元素的 AXBounds 计算鼠标点击的屏幕坐标。
static POINT mouseBoundsCenter(const AXBounds& bounds)
{
	return {
		static_cast<LONG>(bounds.x + bounds.width  / 2.0),
		static_cast<LONG>(bounds.y + bounds.height / 2.0),
	};
}

// sendMouseClick 在屏幕坐标 pt 处发送鼠标点击事件。
// button: 0=左键, 1=右键, 2=中键。
// clicks: 1=单击, 2=双击。
//
// 坐标归一化采用虚拟桌面（SM_CXVIRTUALSCREEN / SM_CYVIRTUALSCREEN）+
// MOUSEEVENTF_VIRTUALDESK，正确支持多显示器环境。
// 若虚拟桌面尺寸为 0（极少数驱动异常），回退到主屏尺寸。
static void sendMouseClick(POINT pt, int button, int clicks)
{
	DWORD down_flag, up_flag;
	switch (button) {
	case 1:  down_flag = MOUSEEVENTF_RIGHTDOWN; up_flag = MOUSEEVENTF_RIGHTUP;   break;
	case 2:  down_flag = MOUSEEVENTF_MIDDLEDOWN; up_flag = MOUSEEVENTF_MIDDLEUP; break;
	default: down_flag = MOUSEEVENTF_LEFTDOWN;  up_flag = MOUSEEVENTF_LEFTUP;   break;
	}

	// 使用虚拟桌面坐标系（涵盖所有显示器），配合 MOUSEEVENTF_VIRTUALDESK。
	int virt_x = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
	int virt_y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
	int virt_w = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
	int virt_h = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
	if (virt_w <= 0) { virt_w = ::GetSystemMetrics(SM_CXSCREEN); virt_x = 0; }
	if (virt_h <= 0) { virt_h = ::GetSystemMetrics(SM_CYSCREEN); virt_y = 0; }

	// 归一化：将虚拟桌面内的物理像素坐标映射到 [0, 65535]。
	// 使用 std::max(1, virt_w - 1) 保护单像素宽/高桌面（极端驱动上报）时的除零。
	LONG norm_x = static_cast<LONG>(((pt.x - virt_x) * 65535L) / std::max(1, virt_w - 1));
	LONG norm_y = static_cast<LONG>(((pt.y - virt_y) * 65535L) / std::max(1, virt_h - 1));

	// 最多需要 1 个 MOVE + clicks × (DOWN + UP)。
	// 双击时为 1 + 2×2 = 5，单击为 1 + 1×2 = 3；按最坏情况分配。
	INPUT inputs[5]{};
	int   n = 0;

	inputs[n]            = {};
	inputs[n].type       = INPUT_MOUSE;
	inputs[n].mi.dx      = norm_x;
	inputs[n].mi.dy      = norm_y;
	inputs[n].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
	n++;

	for (int c = 0; c < clicks; ++c) {
		inputs[n]            = {};
		inputs[n].type       = INPUT_MOUSE;
		inputs[n].mi.dwFlags = down_flag;
		n++;

		inputs[n]            = {};
		inputs[n].type       = INPUT_MOUSE;
		inputs[n].mi.dwFlags = up_flag;
		n++;
	}

	::SendInput(static_cast<UINT>(n), inputs, sizeof(INPUT));
}

// keyNameToVk 将按键名称字符串映射到 Windows 虚拟键码 VK_*。
// 命名与 macOS kVK_* / CGEventKeyboardSetUnicodeString 保持一致，
// 确保 MCP 工具侧的键名无需区分平台。
static WORD keyNameToVk(const std::string& key)
{
	static const std::unordered_map<std::string, int> kMap = {
		{"Return",   VK_RETURN},   {"Enter",    VK_RETURN},
		{"Escape",   VK_ESCAPE},   {"Esc",      VK_ESCAPE},
		{"Space",    VK_SPACE},
		{"Tab",      VK_TAB},
		{"Delete",   VK_DELETE},   {"Del",      VK_DELETE},
		{"BackSpace",VK_BACK},     {"Backspace",VK_BACK},
		{"Up",       VK_UP},
		{"Down",     VK_DOWN},
		{"Left",     VK_LEFT},
		{"Right",    VK_RIGHT},
		{"Home",     VK_HOME},
		{"End",      VK_END},
		{"PageUp",   VK_PRIOR},
		{"PageDown", VK_NEXT},
		{"Insert",   VK_INSERT},
		{"F1",       VK_F1},  {"F2",  VK_F2},  {"F3",  VK_F3},  {"F4",  VK_F4},
		{"F5",       VK_F5},  {"F6",  VK_F6},  {"F7",  VK_F7},  {"F8",  VK_F8},
		{"F9",       VK_F9},  {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
		// 修饰键
		{"Ctrl",     VK_CONTROL}, {"Control", VK_CONTROL},
		{"Alt",      VK_MENU},
		{"Shift",    VK_SHIFT},
		{"Win",      VK_LWIN},
		// macOS 特有修饰键别名（兼容 MCP 工具侧命名）
		{"Cmd",      VK_LWIN},    {"Command", VK_LWIN},
		{"Option",   VK_MENU},
	};
	auto it = kMap.find(key);
	if (it != kMap.end()) {
		return static_cast<WORD>(it->second);
	}
	// 单字符：将 UTF-8 解码为宽字符后使用 VkKeyScanW。
	// VkKeyScanW 对 ASCII 字符（≤0x7F）与 VkKeyScanA 等价；
	// 对 BMP 内的非 ASCII 字符（如拉丁扩展、希腊字母）也能正确查找虚拟键。
	// 注意：VkKeyScanW 仍依赖当前活跃键盘布局，非 ASCII 字符在无对应按键的
	// 键盘布局下会返回 -1。此类字符应通过 setValue（KEYEVENTF_UNICODE）输入。
	std::wstring wkey = utf8ToWstr(key);
	if (wkey.size() == 1) {
		short vk = ::VkKeyScanW(wkey[0]);
		if (vk != -1) {
			return static_cast<WORD>(vk & 0xFF);
		}
	}
	return 0;
}

// isModifierKey 判断按键名称是否为修饰键。
static bool isModifierKey(const std::string& key)
{
	static const std::unordered_map<std::string, bool> kMods = {
		{"Ctrl", true}, {"Control", true}, {"Alt", true}, {"Shift", true},
		{"Win", true}, {"Cmd", true}, {"Command", true}, {"Option", true},
	};
	return kMods.count(key) > 0;
}

// sendKeyEvent 发送单键按下和抬起事件。
// use_unicode: true 时使用 KEYEVENTF_UNICODE（用于非 ASCII 字符输入）。
static void sendKeyEvent(WORD vk, bool key_down, bool use_unicode = false)
{
	INPUT input{};
	input.type       = INPUT_KEYBOARD;
	input.ki.wVk     = use_unicode ? 0 : vk;
	input.ki.wScan   = use_unicode ? vk : static_cast<WORD>(::MapVirtualKeyA(vk, MAPVK_VK_TO_VSC));
	input.ki.dwFlags = (use_unicode ? KEYEVENTF_UNICODE : 0) |
	                   (key_down   ? 0                 : KEYEVENTF_KEYUP);
	::SendInput(1, &input, sizeof(INPUT));
}

// ─── Implement ───────────────────────────────────────────────────────────────

struct AXProviderWindows::Implement
{
	ComScopeInit             com_init_;
	ComRef<IUIAutomation>    uia_;
	ComRef<IUIAutomationTreeWalker> control_walker_;

	// init 初始化 UIA COM 接口。
	bool init()
	{
		if (!com_init_.ok()) {
			LOG_WARN("ax_provider_windows: CoInitializeEx failed (hr={})", com_init_.hr);
			// hr == S_FALSE 表示本线程已初始化，仍可继续使用
		}
		HRESULT hr = ::CoCreateInstance(
		    __uuidof(CUIAutomation8),
		    nullptr,
		    CLSCTX_INPROC_SERVER,
		    __uuidof(IUIAutomation),
		    reinterpret_cast<void**>(uia_.put()));
		if (FAILED(hr)) {
			LOG_ERROR("ax_provider_windows: failed to create IUIAutomation (hr={})", hr);
			return false;
		}
		hr = uia_->get_ControlViewWalker(control_walker_.put());
		if (FAILED(hr)) {
			LOG_ERROR("ax_provider_windows: failed to get ControlViewWalker (hr={})", hr);
			return false;
		}
		return true;
	}
};

// ─── 公开接口实现 ─────────────────────────────────────────────────────────────

AXProviderWindows::AXProviderWindows()
	: implement_(std::make_unique<Implement>())
{
	if (!implement_->init()) {
		// init() 失败时 uia_ 为 nullptr，后续所有操作均会返回 ACCESSIBILITY_DENIED。
		// 此处记录明确的错误日志，便于排查（常见原因：COM 初始化失败、UIAutomation 不可用）。
		LOG_ERROR("ax_provider_windows: UIA initialization failed; "
		          "all operations will return ACCESSIBILITY_DENIED");
	}
}

AXProviderWindows::~AXProviderWindows() = default;

// isPermissionGranted Windows 下 UIA 不需要用户手动授权，总是返回 true。
//
// 注意：对管理员权限（UAC elevated）进程的窗口执行 UIA 操作，需本进程也以管理员身份运行。
// 在这种情况下 UIA 不会报错，但会返回空元素树（HWND 仍可找到，但无法枚举其 UI 子元素）。
// 该限制无法在 isPermissionGranted() 层面静态检测，只能在运行时通过空树结果判断。
bool AXProviderWindows::isPermissionGranted() const
{
	return implement_->uia_ != nullptr;
}

// ─── listWindows ─────────────────────────────────────────────────────────────

namespace {
struct EnumData
{
	IUIAutomation*              uia;
	std::vector<AXWindowInfo>*  windows;
	int                         focused_pid;
};
}

// enumWindowsProc EnumWindows 回调，筛选可见、有标题、非工具窗口的顶层窗口。
static BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lparam)
{
	auto* data = reinterpret_cast<EnumData*>(lparam);

	if (!::IsWindowVisible(hwnd)) {
		return TRUE;
	}

	// 过滤无标题窗口
	wchar_t title[256] = {};
	if (::GetWindowTextW(hwnd, title, 256) == 0) {
		return TRUE;
	}

	// 过滤工具窗口（任务栏上不显示的辅助窗口）
	LONG ex_style = ::GetWindowLongW(hwnd, GWL_EXSTYLE);
	if (ex_style & WS_EX_TOOLWINDOW) {
		return TRUE;
	}

	AXWindowInfo info;
	info.window_id = hwndToWindowId(hwnd);
	info.title     = wstrToUtf8(title);

	// 获取进程 PID
	DWORD pid = 0;
	::GetWindowThreadProcessId(hwnd, &pid);
	info.pid = static_cast<int>(pid);

	// 获取进程可执行路径（对应 macOS bundleIdentifier/app_name）
	WinHandle proc(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
	if (proc) {
		wchar_t exe[MAX_PATH] = {};
		DWORD len = MAX_PATH;
		if (::QueryFullProcessImageNameW(proc.get(), 0, exe, &len)) {
			std::wstring exe_ws(exe, len);
			// 取文件名部分（不含目录和扩展名）作为 app_name
			auto slash = exe_ws.rfind(L'\\');
			if (slash != std::wstring::npos) {
				exe_ws = exe_ws.substr(slash + 1);
			}
			auto dot = exe_ws.rfind(L'.');
			if (dot != std::wstring::npos) {
				exe_ws = exe_ws.substr(0, dot);
			}
			info.app_name  = wstrToUtf8(exe_ws);
			info.bundle_id = wstrToUtf8(exe_ws); // Windows 下用 exe 名称对应 bundleIdentifier
		}
	}

	// 获取窗口位置
	RECT rect{};
	if (::GetWindowRect(hwnd, &rect)) {
		info.bounds = {
			static_cast<double>(rect.left),
			static_cast<double>(rect.top),
			static_cast<double>(rect.right - rect.left),
			static_cast<double>(rect.bottom - rect.top),
		};
	}

	// 判断是否为前台窗口
	HWND fg = ::GetForegroundWindow();
	info.focused = (hwnd == fg);

	data->windows->push_back(std::move(info));
	return TRUE;
}

// listWindows 枚举所有可访问的顶层窗口。
Result<std::vector<AXWindowInfo>> AXProviderWindows::listWindows()
{
	if (!implement_->uia_) {
		return Result<std::vector<AXWindowInfo>>::Error(Status::ACCESSIBILITY_DENIED);
	}

	std::vector<AXWindowInfo> windows;
	EnumData data{ implement_->uia_.get(), &windows, 0 };
	::EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&data));

	return Result<std::vector<AXWindowInfo>>::Ok(std::move(windows));
}

// ─── getUITree ───────────────────────────────────────────────────────────────

// getUITree 获取指定窗口的 UI 元素树。
Result<AXElement> AXProviderWindows::getUITree(std::string_view window_id, int max_depth)
{
	if (!implement_->uia_) {
		return Result<AXElement>::Error(Status::ACCESSIBILITY_DENIED);
	}

	auto hwnd_opt = parseWindowId(window_id);
	if (!hwnd_opt || !::IsWindow(*hwnd_opt)) {
		return Result<AXElement>::Error(Status::WINDOW_NOT_FOUND);
	}
	HWND hwnd = *hwnd_opt;

	ComRef<IUIAutomationElement> root;
	if (FAILED(implement_->uia_->ElementFromHandle(hwnd, root.put())) || !root) {
		return Result<AXElement>::Error(Status::WINDOW_NOT_FOUND);
	}

	// 构建根路径（第一层就是 window_id）
	std::string root_path = std::string(window_id);
	AXElement tree = buildElement(root.get(),
	                               implement_->uia_.get(),
	                               implement_->control_walker_.get(),
	                               root_path, 0, max_depth);

	return Result<AXElement>::Ok(std::move(tree));
}

// ─── click / doubleClick / rightClick ───────────────────────────────────────

// click 对指定元素执行单击。
// L1: IUIAutomationInvokePattern::Invoke（语义级）
// L2: SendInput 鼠标左键单击（坐标级）
Result<void> AXProviderWindows::click(std::string_view element_path)
{
	if (!implement_->uia_) {
		return Result<void>::Error(Status::ACCESSIBILITY_DENIED);
	}

	auto elem = findElementByPath(element_path, implement_->uia_.get(),
	                               implement_->control_walker_.get());
	if (!elem) {
		return Result<void>::Error(Status::ELEMENT_NOT_FOUND);
	}

	// L1: InvokePattern
	ComRef<IUIAutomationInvokePattern> invoke;
	if (SUCCEEDED(elem->GetCurrentPattern(UIA_InvokePatternId,
	                                       reinterpret_cast<IUnknown**>(invoke.put()))) && invoke) {
		if (SUCCEEDED(invoke->Invoke())) {
			return Result<void>::Ok();
		}
	}

	// L2: 回退到 SendInput 鼠标点击
	// 先将所属窗口激活到前台：SendInput 发送鼠标事件时，事件会命中光标下的窗口，
	// 若目标窗口被遮挡，点击将落入错误的前台窗口。
	{
		auto parts = splitPath(element_path);
		if (!parts.empty()) {
			auto hwnd_opt = parseWindowId(parts[0]);
			if (hwnd_opt && ::IsWindow(*hwnd_opt)) {
				if (::IsIconic(*hwnd_opt)) {
					::ShowWindow(*hwnd_opt, SW_RESTORE);
				}
				::SetForegroundWindow(*hwnd_opt);
				::Sleep(kActivateWindowDelayMs);
			}
		}
	}
	AXBounds bounds = uiaGetBounds(elem.get());
	if (bounds.width <= 0 || bounds.height <= 0) {
		return Result<void>::Error(Status::ELEMENT_NOT_FOUND);
	}
	sendMouseClick(mouseBoundsCenter(bounds), 0, 1);
	return Result<void>::Ok();
}

// doubleClick 对指定元素执行双击（直接走 SendInput，无语义级 API）。
Result<void> AXProviderWindows::doubleClick(std::string_view element_path)
{
	if (!implement_->uia_) {
		return Result<void>::Error(Status::ACCESSIBILITY_DENIED);
	}

	auto elem = findElementByPath(element_path, implement_->uia_.get(),
	                               implement_->control_walker_.get());
	if (!elem) {
		return Result<void>::Error(Status::ELEMENT_NOT_FOUND);
	}

	// 激活所属窗口到前台，确保 SendInput 事件不被前台遮挡窗口截获
	{
		auto parts = splitPath(element_path);
		if (!parts.empty()) {
			auto hwnd_opt = parseWindowId(parts[0]);
			if (hwnd_opt && ::IsWindow(*hwnd_opt)) {
				if (::IsIconic(*hwnd_opt)) {
					::ShowWindow(*hwnd_opt, SW_RESTORE);
				}
				::SetForegroundWindow(*hwnd_opt);
				::Sleep(kActivateWindowDelayMs);
			}
		}
	}
	AXBounds bounds = uiaGetBounds(elem.get());
	if (bounds.width <= 0 || bounds.height <= 0) {
		return Result<void>::Error(Status::ELEMENT_NOT_FOUND);
	}
	sendMouseClick(mouseBoundsCenter(bounds), 0, 2);
	return Result<void>::Ok();
}

// rightClick 对指定元素执行右键单击。
Result<void> AXProviderWindows::rightClick(std::string_view element_path)
{
	if (!implement_->uia_) {
		return Result<void>::Error(Status::ACCESSIBILITY_DENIED);
	}

	auto elem = findElementByPath(element_path, implement_->uia_.get(),
	                               implement_->control_walker_.get());
	if (!elem) {
		return Result<void>::Error(Status::ELEMENT_NOT_FOUND);
	}

	// 激活所属窗口到前台，确保 SendInput 事件不被前台遮挡窗口截获
	{
		auto parts = splitPath(element_path);
		if (!parts.empty()) {
			auto hwnd_opt = parseWindowId(parts[0]);
			if (hwnd_opt && ::IsWindow(*hwnd_opt)) {
				if (::IsIconic(*hwnd_opt)) {
					::ShowWindow(*hwnd_opt, SW_RESTORE);
				}
				::SetForegroundWindow(*hwnd_opt);
				::Sleep(kActivateWindowDelayMs);
			}
		}
	}
	AXBounds bounds = uiaGetBounds(elem.get());
	if (bounds.width <= 0 || bounds.height <= 0) {
		return Result<void>::Error(Status::ELEMENT_NOT_FOUND);
	}
	sendMouseClick(mouseBoundsCenter(bounds), 1, 1);
	return Result<void>::Ok();
}

// ─── setValue ────────────────────────────────────────────────────────────────

// setValue 向目标元素写入文字值。
// L1: IUIAutomationValuePattern::SetValue（语义级）
// L2: 聚焦后使用 SendInput KEYEVENTF_UNICODE 逐字输入（坐标/键盘级）
Result<void> AXProviderWindows::setValue(std::string_view element_path,
                                         std::string_view value)
{
	if (!implement_->uia_) {
		return Result<void>::Error(Status::ACCESSIBILITY_DENIED);
	}

	auto elem = findElementByPath(element_path, implement_->uia_.get(),
	                               implement_->control_walker_.get());
	if (!elem) {
		return Result<void>::Error(Status::ELEMENT_NOT_FOUND);
	}

	// L1: ValuePattern
	ComRef<IUIAutomationValuePattern> value_pat;
	if (SUCCEEDED(elem->GetCurrentPattern(UIA_ValuePatternId,
	                                       reinterpret_cast<IUnknown**>(value_pat.put()))) &&
	    value_pat) {
		BOOL readonly = TRUE;
		value_pat->get_CurrentIsReadOnly(&readonly);
		if (!readonly) {
			std::wstring wval = utf8ToWstr(value);
			ComStr bstr_val(::SysAllocString(wval.c_str()));
			if (SUCCEEDED(value_pat->SetValue(bstr_val.get()))) {
				return Result<void>::Ok();
			}
		}
	}

	// L2: 先聚焦，然后全选清空，再逐字输入
	elem->SetFocus();
	// Ctrl+A 全选
	sendKeyEvent(VK_CONTROL, true);
	sendKeyEvent('A',        true);
	sendKeyEvent('A',        false);
	sendKeyEvent(VK_CONTROL, false);
	// Delete 删除
	sendKeyEvent(VK_DELETE, true);
	sendKeyEvent(VK_DELETE, false);

	// 逐字符输入（Unicode）
	std::wstring wval = utf8ToWstr(value);
	for (wchar_t ch : wval) {
		sendKeyEvent(static_cast<WORD>(ch), true,  true);
		sendKeyEvent(static_cast<WORD>(ch), false, true);
	}
	return Result<void>::Ok();
}

// ─── focus ───────────────────────────────────────────────────────────────────

// focus 将键盘焦点移动到目标元素。
Result<void> AXProviderWindows::focus(std::string_view element_path)
{
	if (!implement_->uia_) {
		return Result<void>::Error(Status::ACCESSIBILITY_DENIED);
	}

	auto elem = findElementByPath(element_path, implement_->uia_.get(),
	                               implement_->control_walker_.get());
	if (!elem) {
		return Result<void>::Error(Status::ELEMENT_NOT_FOUND);
	}

	HRESULT hr = elem->SetFocus();
	if (FAILED(hr)) {
		return Result<void>::Error(Status::INTERNAL_ERROR, "SetFocus failed");
	}
	return Result<void>::Ok();
}

// ─── scroll ──────────────────────────────────────────────────────────────────

// scroll 对目标元素执行滚动操作。
// 优先使用 IUIAutomationScrollPattern，否则回退到 SendInput MOUSEEVENTF_WHEEL。
//
// amount 含义（已澄清）：滚动档数（notches），每档对应一次鼠标滚轮格，
// 默认值 3 表示三档（与多数系统默认滚动速度一致）。
// L1 路径（ScrollPattern）中 amount 用于控制循环调用次数；
// L2 路径（MOUSEEVENTF_WHEEL）中每档发送 WHEEL_DELTA（120）个单位。
Result<void> AXProviderWindows::scroll(std::string_view element_path,
                                       std::string_view direction,
                                       int              amount)
{
	// 参数验证置于函数最前，与 click/setValue 等接口保持一致
	if (!implement_->uia_) {
		return Result<void>::Error(Status::ACCESSIBILITY_DENIED);
	}

	if (amount <= 0) {
		return Result<void>::Error(Status::INVALID_ARGUMENT,
		                           "amount must be > 0 (unit: notches)");
	}

	bool vertical   = (direction == "up" || direction == "down");
	bool is_forward = (direction == "down" || direction == "right");

	if (!vertical && direction != "left" && direction != "right") {
		return Result<void>::Error(Status::INVALID_ARGUMENT,
		                           "direction must be up/down/left/right");
	}

	auto elem = findElementByPath(element_path, implement_->uia_.get(),
	                               implement_->control_walker_.get());
	if (!elem) {
		return Result<void>::Error(Status::ELEMENT_NOT_FOUND);
	}

	// L1: ScrollPattern
	ComRef<IUIAutomationScrollPattern> scroll_pat;
	if (SUCCEEDED(elem->GetCurrentPattern(UIA_ScrollPatternId,
	                                       reinterpret_cast<IUnknown**>(scroll_pat.put()))) &&
	    scroll_pat) {
		ScrollAmount scroll_amt = is_forward
		                          ? ScrollAmount_LargeIncrement
		                          : ScrollAmount_LargeDecrement;
		if (vertical) {
			scroll_pat->Scroll(ScrollAmount_NoAmount, scroll_amt);
		} else {
			scroll_pat->Scroll(scroll_amt, ScrollAmount_NoAmount);
		}
		return Result<void>::Ok();
	}

	// L2: SendInput MOUSEEVENTF_WHEEL / MOUSEEVENTF_HWHEEL
	// 将坐标内联到同一个 INPUT 结构，避免 SetCursorPos + SendInput 两步非原子操作。
	// 使用虚拟桌面坐标系（同 sendMouseClick），正确支持多显示器。
	AXBounds bounds = uiaGetBounds(elem.get());

	// amount 即档数，每档向 Windows 发送 WHEEL_DELTA（120）个滚动单位。
	int notches = amount;

	INPUT input{};
	input.type         = INPUT_MOUSE;
	input.mi.dwFlags   = (vertical ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL);
	// 向上/左为正值，向下/右为负值
	input.mi.mouseData = static_cast<DWORD>((is_forward ? -1 : 1) * notches * WHEEL_DELTA);

	if (bounds.width > 0 && bounds.height > 0) {
		POINT center = mouseBoundsCenter(bounds);
		int virt_x = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
		int virt_y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
		int virt_w = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
		int virt_h = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
		if (virt_w <= 0) { virt_w = ::GetSystemMetrics(SM_CXSCREEN); virt_x = 0; }
		if (virt_h <= 0) { virt_h = ::GetSystemMetrics(SM_CYSCREEN); virt_y = 0; }

		input.mi.dx      = static_cast<LONG>(((center.x - virt_x) * 65535L) / std::max(1, virt_w - 1));
		input.mi.dy      = static_cast<LONG>(((center.y - virt_y) * 65535L) / std::max(1, virt_h - 1));
		input.mi.dwFlags |= MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | MOUSEEVENTF_VIRTUALDESK;
	}

	::SendInput(1, &input, sizeof(INPUT));

	return Result<void>::Ok();
}

// ─── keyPress ────────────────────────────────────────────────────────────────

// keyPress 模拟按下并释放单个按键。
// window_id 传入时尝试先激活目标窗口（SendInput 需要前台焦点）。
Result<void> AXProviderWindows::keyPress(std::string_view key,
                                          std::string_view window_id)
{
	std::string key_str(key);
	WORD vk = keyNameToVk(key_str);
	if (vk == 0) {
		return Result<void>::Error(Status::NOT_SUPPORTED);
	}

	// 若指定了 window_id，先将对应窗口激活到前台
	if (!window_id.empty()) {
		auto hwnd_opt = parseWindowId(window_id);
		if (hwnd_opt && ::IsWindow(*hwnd_opt)) {
			::SetForegroundWindow(*hwnd_opt);
			::Sleep(kActivateWindowDelayMs);
		}
	}

	sendKeyEvent(vk, true);
	sendKeyEvent(vk, false);
	return Result<void>::Ok();
}

// ─── keyCombo ────────────────────────────────────────────────────────────────

// keyCombo 模拟按下组合键。
// keys 最后一个元素为主键，其余为修饰键（按顺序按下，逆序抬起）。
Result<void> AXProviderWindows::keyCombo(const std::vector<std::string>& keys,
                                          std::string_view                window_id)
{
	if (keys.empty()) {
		return Result<void>::Error(Status::INVALID_ARGUMENT, "keys must not be empty");
	}

	// 验证所有键名有效
	std::vector<WORD> vks;
	vks.reserve(keys.size());
	for (const auto& k : keys) {
		WORD vk = keyNameToVk(k);
		if (vk == 0) {
			return Result<void>::Error(Status::NOT_SUPPORTED);
		}
		vks.push_back(vk);
	}

	// 确保最后一个键不是修饰键（否则是无效组合键）
	if (isModifierKey(keys.back())) {
		return Result<void>::Error(Status::INVALID_ARGUMENT,
		                           "last key in combo must not be a modifier");
	}

	// 若指定了 window_id，先激活目标窗口
	if (!window_id.empty()) {
		auto hwnd_opt = parseWindowId(window_id);
		if (hwnd_opt && ::IsWindow(*hwnd_opt)) {
			::SetForegroundWindow(*hwnd_opt);
			::Sleep(kActivateWindowDelayMs);
		}
	}

	// 按下所有键（顺序）
	for (WORD vk : vks) {
		sendKeyEvent(vk, true);
	}
	// 抬起所有键（逆序）
	for (auto it = vks.rbegin(); it != vks.rend(); ++it) {
		sendKeyEvent(*it, false);
	}

	return Result<void>::Ok();
}

// ─── activate ────────────────────────────────────────────────────────────────

// activate 将目标窗口激活到屏幕前台。
Result<void> AXProviderWindows::activate(std::string_view window_id)
{
	auto hwnd_opt = parseWindowId(window_id);
	if (!hwnd_opt || !::IsWindow(*hwnd_opt)) {
		return Result<void>::Error(Status::WINDOW_NOT_FOUND);
	}
	HWND hwnd = *hwnd_opt;

	// 若窗口已最小化则恢复
	if (::IsIconic(hwnd)) {
		::ShowWindow(hwnd, SW_RESTORE);
	}

	::SetForegroundWindow(hwnd);
	::BringWindowToTop(hwnd);
	return Result<void>::Ok();
}

} // namespace ax
} // namespace capability
} // namespace clawspan
