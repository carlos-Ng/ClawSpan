#pragma once

#include "common/types.h"
#include <string>
#include <toml++/toml.h>

namespace clawspan {

static bool parseU64FromString(const std::string& s, u64& out)
{
	try {
		if (s.size() >= 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X")) {
			out = std::stoull(s, nullptr, 16);
		} else {
			out = std::stoull(s, nullptr, 10);
		}
		return true;
	} catch (...) {
		return false;
	}
}

static bool parseU32FromString(const std::string& s, u32& out)
{
	u64 tmp = 0;

	if (!parseU64FromString(s, tmp)) 
		return false;

	out = static_cast<u32>(tmp);

	return true;
}

static bool getString(const toml::table& tbl, const char* key, std::string& out)
{
	if (auto node = tbl.get(key)) {
		if (auto v = node->value<std::string>()) {
			out = *v;
			return true;
		}
	}
	return false;
}

static bool getU64(const toml::table& tbl, const char* key, u64& out)
{
	if (auto node = tbl.get(key)) {
		if (auto v = node->value<i64>()) {
			if (*v < 0) 
				return false;
			out = static_cast<u64>(*v);
			return true;
		}
		if (auto v = node->value<std::string>()) {
			return parseU64FromString(*v, out);
		}
	}
	return false;
}

static bool getU32(const toml::table& tbl, const char* key, u32& out)
{
	u64 tmp = 0;
	if (!getU64(tbl, key, tmp)) return false;
	out = static_cast<u32>(tmp);
	return true;
}

static const toml::table* getTable(const toml::table& root, const char* key)
{
	if (auto node = root.get(key)) {
		if (auto tbl = node->as_table()) {
			return tbl;
		}
	}
	return nullptr;
}


}
