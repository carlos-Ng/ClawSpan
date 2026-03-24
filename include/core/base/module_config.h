#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace clawspan {
namespace core {

// ModuleConfig 是模块初始化参数的泛型载体，以 flat key-value 形式存储配置项。
//
// 设计目标：解耦模块与具体配置文件格式（toml/json 等），模块仅依赖此结构，
// 不引入任何外部配置库。ModuleManager 负责将外部配置格式转换为 ModuleConfig
// 后传入各模块的 init 方法。
//
// 支持的值类型：std::string、int64_t、double、bool。
// 若未来确实需要，可扩展支持数组类型。
class ModuleConfig
{
public:
	// Value 是所有支持的配置值类型的 variant 联合。
	using Value = std::variant<std::string, int64_t, double, bool>;

	// getString 获取指定键对应的字符串值。
	//
	// 入参:
	// - key: 配置键名。
	//
	// 出参/返回:
	// - 键存在且类型匹配时返回 std::string 值；否则返回 std::nullopt。
	std::optional<std::string> getString(std::string_view key) const;

	// getInt 获取指定键对应的整数值。
	//
	// 入参:
	// - key: 配置键名。
	//
	// 出参/返回:
	// - 键存在且类型匹配时返回 int64_t 值；否则返回 std::nullopt。
	std::optional<int64_t> getInt(std::string_view key) const;

	// getDouble 获取指定键对应的浮点数值。
	//
	// 入参:
	// - key: 配置键名。
	//
	// 出参/返回:
	// - 键存在且类型匹配时返回 double 值；否则返回 std::nullopt。
	std::optional<double> getDouble(std::string_view key) const;

	// getBool 获取指定键对应的布尔值。
	//
	// 入参:
	// - key: 配置键名。
	//
	// 出参/返回:
	// - 键存在且类型匹配时返回 bool 值；否则返回 std::nullopt。
	std::optional<bool> getBool(std::string_view key) const;

	// set 写入一个配置项，供 ModuleManager 在解析外部配置后填充使用。
	//
	// 入参:
	// - key:   配置键名。
	// - value: 配置值，类型须为 Value variant 的成员类型之一。
	void set(std::string key, Value value);

private:
	std::unordered_map<std::string, Value> entries_;
};

} // namespace core
} // namespace clawspan
