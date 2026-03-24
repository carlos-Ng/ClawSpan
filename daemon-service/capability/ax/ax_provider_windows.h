#pragma once

#include "ax_provider.h"

#include <memory>

namespace clawspan {
namespace capability {
namespace ax {

// AXProviderWindows 是 AXProviderInterface 基于 Windows UI Automation COM 接口的具体实现。
//
// 所有方法为无状态实现：window_id 中直接编码了窗口 HWND 的十六进制字符串，
// 每次操作按需向 UIA 重新查询，无会话状态缓存。
//
// window_id 格式：w<pid>_<hwnd_hex>，例如 "w1234_1A2B0"。
// 其中 hwnd_hex 是 HWND 的十六进制无前缀表示，与 macOS 的 pid/index 格式对称。
//
// 注意：对管理员权限进程的窗口执行 UIA 操作，需本进程也以管理员身份运行。
// isPermissionGranted() 将始终返回 true（UIA 不需要用户手动授权），
// 但对提权窗口的操作可能返回空元素，实现中已有注释说明。
class AXProviderWindows : public AXProviderInterface
{
public:
	AXProviderWindows();
	~AXProviderWindows() override;

	// 不可拷贝
	AXProviderWindows(const AXProviderWindows&)            = delete;
	AXProviderWindows& operator=(const AXProviderWindows&) = delete;

	bool isPermissionGranted() const override;

	Result<std::vector<AXWindowInfo>> listWindows() override;

	Result<AXElement> getUITree(std::string_view window_id,
	                            int              max_depth) override;

	Result<void> click(std::string_view element_path) override;

	Result<void> doubleClick(std::string_view element_path) override;

	Result<void> rightClick(std::string_view element_path) override;

	Result<void> setValue(std::string_view element_path,
	                      std::string_view value) override;

	Result<void> focus(std::string_view element_path) override;

	Result<void> scroll(std::string_view element_path,
	                    std::string_view direction,
	                    int              amount) override;

	Result<void> keyPress(std::string_view key,
	                      std::string_view window_id) override;

	Result<void> keyCombo(const std::vector<std::string>& keys,
	                       std::string_view                window_id) override;

	Result<void> activate(std::string_view window_id) override;

private:
	struct Implement;
	std::unique_ptr<Implement> implement_;
};

} // namespace ax
} // namespace capability
} // namespace clawspan
