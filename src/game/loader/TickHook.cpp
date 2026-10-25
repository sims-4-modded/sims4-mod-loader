/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include "game/loader/TickHook.h"

#include <atomic>
#include <utility>

#include <MinHook.h>

namespace ts4_mod::loader
{

namespace
{
using NativeFunction = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);

/// The one bootstrap hook. MinHook calls the fixed function `Detour`, which cannot carry data, so the hook finds its
/// object here. `Install` sets it before the hook works, and nothing clears it, because the object lives until the
/// process stops.
std::atomic<BootstrapHook*> s_hook = nullptr;

[[nodiscard]] MH_STATUS StartMinHook()
{
	const auto status = MH_Initialize();
	return status == MH_ERROR_ALREADY_INITIALIZED ? MH_OK : status;
}
} // namespace

BootstrapHook::BootstrapHook(void* target, Tick tick, Removed removed)
    : _target(target)
    , _tick(std::move(tick))
    , _removed(std::move(removed))
{
}

ts4::Result<std::unique_ptr<BootstrapHook>> BootstrapHook::Install(void* target, Tick tick, Removed removed)
{
	if (target == nullptr || !tick)
	{
		return ts4::MakeError("the bootstrap hook has no target or no tick function");
	}
	if (s_hook.load() != nullptr)
	{
		return ts4::MakeError("a bootstrap hook exists already");
	}
	if (const auto status = StartMinHook(); status != MH_OK)
	{
		return ts4::MakeError("MinHook did not start: {}", MH_StatusToString(status));
	}
	std::unique_ptr<BootstrapHook> hook(new BootstrapHook(target, std::move(tick), std::move(removed)));
	if (const auto status = MH_CreateHook(target, reinterpret_cast<void*>(&Detour), &hook->_original); status != MH_OK)
	{
		return ts4::MakeError("MinHook cannot put the bootstrap hook on the tick function: {}", MH_StatusToString(status));
	}
	s_hook = hook.get();
	if (const auto status = MH_EnableHook(target); status != MH_OK)
	{
		s_hook = nullptr;
		(void)MH_RemoveHook(target);
		return ts4::MakeError("MinHook cannot start the bootstrap hook: {}", MH_StatusToString(status));
	}
	hook->_installed = true;
	return hook;
}

std::uintptr_t BootstrapHook::Detour(std::uintptr_t first, std::uintptr_t second, std::uintptr_t third, std::uintptr_t fourth)
{
	auto* hook = s_hook.load(std::memory_order_acquire);
	const auto original = reinterpret_cast<NativeFunction>(hook->_original);
	const auto result = original(first, second, third, fourth);
	if (!hook->_installed || hook->_inTick)
	{
		return result;
	}
	++hook->_calls;
	hook->_inTick = true;
	bool remove = false;
	// No C++ exception may leave the extender into the game.
	try
	{
		remove = hook->_tick();
	}
	catch (...)
	{
		remove = false;
	}
	hook->_inTick = false;
	if (!remove)
	{
		return result;
	}
	// The original function returned, so no code of this call uses the trampoline after this point. The game calls the
	// function only on the main thread, which is this thread.
	if (MH_DisableHook(hook->_target) != MH_OK || MH_RemoveHook(hook->_target) != MH_OK)
	{
		return result;
	}
	hook->_installed = false;
	if (hook->_removed)
	{
		try
		{
			hook->_removed();
		}
		catch (...)
		{
			// The tick function has no hook of the extender now. The log of `removed` tells the error.
		}
	}
	return result;
}

} // namespace ts4_mod::loader
