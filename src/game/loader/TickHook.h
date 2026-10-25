/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <cstdint>

#include <functional>
#include <memory>
#include <string_view>

#include <common/Result.h>

namespace ts4_mod::loader
{

/// The update of all client services of the game. The main loop of the game calls it one time in each frame, on the
/// main thread, also at the main menu and on the load screens. One of the services runs the simulation and the game
/// Python. The function gets four integer parameters: the service manager, the game clock and two flags.
inline constexpr std::string_view k_ServiceUpdateSignature =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 4C 8B 51 58";
/// The number of parameters of the update of the client services.
inline constexpr std::uint32_t k_ServiceUpdateParameters = 4;

/// The first step of the shutdown of the game application. The game calls it one time, on the main thread, after the
/// main loop stops and before the services stop. It gets one parameter: the application.
inline constexpr std::string_view k_AppShutdownSignature = "48 83 EC 28 48 89 74 24 38 48 8B F1 48 89 7C 24 20";

/// The hook that starts the extender on the main thread. The extender does not know the main thread before the first
/// frame, and the hook manager must start on that thread. Thus this hook goes on the tick function first, with its own
/// MinHook hook. In each call, after the original function, it calls `tick` on the thread of the game. When `tick`
/// returns true, the hook removes itself and then calls `removed`, still on the main thread. The hook manager can
/// then put its own hook on the same function.
///
/// Only one bootstrap hook can exist, because MinHook calls a fixed function that cannot carry data.
class BootstrapHook
{
public:
	/// Return true to remove the hook after this call.
	using Tick = std::function<bool()>;
	using Removed = std::function<void()>;

	/// Install the hook on `target`, a function with at most four integer parameters and an integer result.
	[[nodiscard]] static ts4::Result<std::unique_ptr<BootstrapHook>> Install(void* target, Tick tick, Removed removed);

	/// The hook stays in the game until `tick` removes it, because the game can call it at any time. Thus the object
	/// must live as long as the hook: the extender keeps it until the process stops.
	~BootstrapHook() = default;
	BootstrapHook(const BootstrapHook&) = delete;
	BootstrapHook& operator=(const BootstrapHook&) = delete;

	[[nodiscard]] bool Installed() const { return _installed; }
	[[nodiscard]] std::uint64_t Calls() const { return _calls; }

private:
	BootstrapHook(void* target, Tick tick, Removed removed);

	static std::uintptr_t Detour(std::uintptr_t first, std::uintptr_t second, std::uintptr_t third, std::uintptr_t fourth);

	void* _target = nullptr;
	Tick _tick;
	Removed _removed;
	void* _original = nullptr;
	bool _installed = false;
	/// True while `tick` runs. A call of the function from `tick` runs only the original function.
	bool _inTick = false;
	std::uint64_t _calls = 0;
};

} // namespace ts4_mod::loader
