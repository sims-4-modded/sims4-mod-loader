/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <common/Result.h>

namespace ts4_mod::loader
{

/// The longest name of a function for the log.
inline constexpr std::size_t k_MaxFunctionName = 128;

/// A native function that a hook goes on: a module of the game and the signature of the first bytes of the function.
/// The extender installs one hook for each key. The signature is in the normal form of `Signature::ToString`, so two
/// texts of the same signature give the same key.
struct FunctionKey
{
	std::string module;
	std::string signature;

	/// The text for the log, for example "Simulation_x64.dll [48 89 5C 24 ?? 57]".
	[[nodiscard]] std::string Text() const;
	friend auto operator<=>(const FunctionKey&, const FunctionKey&) = default;
};

/// A native function that a mod asks for: its key, its name for the log and the number of parameters that its
/// callbacks get.
struct FunctionTarget
{
	FunctionKey key;
	std::string name;
	std::uint32_t parameterCount = 0;

	friend bool operator==(const FunctionTarget&, const FunctionTarget&) = default;
};

/// The name of the module of the game in the form of `Ts4ModApi.h` (`TS4_MODULE_GAME` or `TS4_MODULE_SIMULATION`).
/// The test ignores the case of letters, as Windows does. Return an empty text for a module that a callback cannot name.
[[nodiscard]] std::string_view GameModuleName(std::string_view module);

/// Check the description of a native function and make its target. Return an error if the module is not a module of
/// the game, if the name is empty or too long, if the signature text is not correct, or if the function has more
/// parameters than a callback can get.
[[nodiscard]] ts4::Result<FunctionTarget> MakeFunctionTarget(std::string_view module, std::string_view name,
                                                             std::string_view signature, std::uint32_t parameterCount);

/// Check that a new target agrees with a target that already has callbacks on the same function. One function has one
/// hook, so all its callbacks must get the same number of parameters. `owner` is the name of the owner of
/// `existing`, for the error.
[[nodiscard]] ts4::Result<void> CheckSameFunction(const FunctionTarget& existing, const FunctionTarget& next,
                                                  std::string_view owner);

/// A native function after the scan of its module: the target and the address that its signature found. An address of
/// 0 tells that the scan found no single match.
struct ResolvedFunction
{
	FunctionTarget target;
	std::uintptr_t address = 0;
};

/// One hook on one address, with the targets whose signatures found that address. Two signatures can find the same
/// function, so the extender joins their callbacks into one hook.
struct JoinedHook
{
	std::uintptr_t address = 0;
	std::uint32_t parameterCount = 0;
	/// The targets in the order of their first callback.
	std::vector<FunctionTarget> targets;
	/// Why the extender puts no hook on this address, or empty. Then no callback of the targets runs.
	std::string error;
};

/// Join the targets that resolve to one address into one hook, in the order of the first target of each address. The
/// join fails if the targets of one address ask for different numbers of parameters. A target with no address gives
/// no hook.
[[nodiscard]] std::vector<JoinedHook> JoinResolvedFunctions(std::span<const ResolvedFunction> functions);

} // namespace ts4_mod::loader

namespace ts4_mod::overlay
{

/// Why the menu bar does not put a hook on a function of the swap chain.
enum class HookTargetError
{
	None,          ///< The function is correct for a hook.
	NoFunction,    ///< The slot of the vtable is empty.
	OwnHook,       ///< The slot points to a hook of the extender. The original function is then not known.
	OutsideModule, ///< The slot points out of the module of the vtable: another program changed the slot.
};

/// Check the function that a slot of a swap chain vtable points to, before the extender puts its hook on the code of
/// that function. The extender reads the function one time only. It never keeps a hook of its own as the original
/// function, because that hook would call itself with no end. `ownHooks` are the hook functions of the extender.
[[nodiscard]] constexpr HookTargetError CheckHookTarget(uintptr_t function, std::span<const uintptr_t> ownHooks,
                                                        bool inVtableModule)
{
	if (function == 0)
	{
		return HookTargetError::NoFunction;
	}
	if (std::ranges::find(ownHooks, function) != ownHooks.end())
	{
		return HookTargetError::OwnHook;
	}
	if (!inVtableModule)
	{
		return HookTargetError::OutsideModule;
	}
	return HookTargetError::None;
}

/// The depth of the hook calls at which the extender tells of a loop. A correct chain of hooks calls a hook of the
/// extender one time, or two times if one function of DXGI calls another.
inline constexpr int k_HookLoopDepth = 8;

/// The depth of the hook calls on one thread. Each hook enters it at the start and leaves it at the end. Only the
/// outer call gives the frame to the menu bar. An inner call calls the original function immediately and does no
/// other work.
class HookDepth
{
public:
	/// What a hook does in this call.
	struct Entry
	{
		bool outer = false;     ///< No other hook of the extender is in progress on this thread.
		bool loopFound = false; ///< The depth is at `k_HookLoopDepth` for the first time on this thread.
	};

	[[nodiscard]] constexpr Entry Enter()
	{
		++_depth;
		const bool loop = _depth >= k_HookLoopDepth && !_loopReported;
		_loopReported = _loopReported || loop;
		return {.outer = _depth == 1, .loopFound = loop};
	}

	constexpr void Leave()
	{
		if (_depth > 0)
		{
			--_depth;
		}
	}

	[[nodiscard]] constexpr int Depth() const { return _depth; }

private:
	int _depth = 0;
	bool _loopReported = false;
};

/// Enter a `HookDepth` for the time of one hook call.
class HookScope
{
public:
	explicit HookScope(HookDepth& depth)
	    : _depth(depth)
	    , _entry(depth.Enter())
	{
	}
	~HookScope() { _depth.Leave(); }
	HookScope(const HookScope&) = delete;
	HookScope& operator=(const HookScope&) = delete;

	[[nodiscard]] bool Outer() const { return _entry.outer; }
	[[nodiscard]] bool LoopFound() const { return _entry.loopFound; }

private:
	HookDepth& _depth;
	HookDepth::Entry _entry;
};

} // namespace ts4_mod::overlay
