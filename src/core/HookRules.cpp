/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <array>
#include <format>

#include <core/HookRules.h>
#include <mod_api/Ts4ModApi.h>
#include <mod_core/Signature.h>

namespace ts4_mod::loader
{

namespace
{
constexpr std::array<std::string_view, 2> k_GameModules {TS4_MODULE_GAME, TS4_MODULE_SIMULATION};

[[nodiscard]] char Lower(char c)
{
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool SameIgnoringCase(std::string_view a, std::string_view b)
{
	return std::ranges::equal(a, b, [](char x, char y) { return Lower(x) == Lower(y); });
}
} // namespace

std::string FunctionKey::Text() const
{
	return std::format("{} [{}]", module, signature);
}

std::string_view GameModuleName(std::string_view module)
{
	const auto found =
	    std::ranges::find_if(k_GameModules, [module](std::string_view name) { return SameIgnoringCase(name, module); });
	return found == k_GameModules.end() ? std::string_view() : *found;
}

ts4::Result<FunctionTarget> MakeFunctionTarget(std::string_view module, std::string_view name, std::string_view signature,
                                               std::uint32_t parameterCount)
{
	if (name.empty() || name.size() > k_MaxFunctionName)
	{
		return ts4::MakeError("a native function needs a name of 1 to {} characters", k_MaxFunctionName);
	}
	const auto moduleName = GameModuleName(module);
	if (moduleName.empty())
	{
		return ts4::MakeError("the function '{}' is in the module '{}'; a callback can name only {} or {}", name, module,
		                      TS4_MODULE_GAME, TS4_MODULE_SIMULATION);
	}
	auto parsed = Signature::Parse(signature);
	if (!parsed)
	{
		return ts4::MakeError("the function '{}': {}", name, parsed.GetError().message);
	}
	if (parameterCount > TS4_CALL_MAX_ARGUMENTS)
	{
		return ts4::MakeError("the function '{}' has {} parameters; a callback gets at most {}", name, parameterCount,
		                      TS4_CALL_MAX_ARGUMENTS);
	}
	return FunctionTarget {.key = {.module = std::string(moduleName), .signature = parsed->ToString()},
	                       .name = std::string(name),
	                       .parameterCount = parameterCount};
}

ts4::Result<void> CheckSameFunction(const FunctionTarget& existing, const FunctionTarget& next, std::string_view owner)
{
	if (existing.key != next.key || existing.parameterCount == next.parameterCount)
	{
		return {};
	}
	return ts4::MakeError("{} has a hook of {} with {} parameters as '{}'; the callback asks for {} parameters",
	                      next.key.Text(), owner, existing.parameterCount, existing.name, next.parameterCount);
}

} // namespace ts4_mod::loader
