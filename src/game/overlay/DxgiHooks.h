/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <cstdint>

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <common/Result.h>
#include <dxgi.h>
#include <nlohmann/json.hpp>

namespace spdlog
{
class logger;
}

namespace ts4_mod::overlay
{

/// Gets the calls of the swap chain that the hooks wrap. The functions run on the thread that presents (the render
/// thread of the game). They must not throw, and they must do almost no work while the menu bar does not show.
class SwapChainListener
{
public:
	/// The swap chain presents a frame. The function runs before the original `Present`.
	virtual void BeforePresent(IDXGISwapChain* swapChain, UINT flags) noexcept = 0;
	/// The result of the original `Present`.
	virtual void AfterPresent(IDXGISwapChain* swapChain, HRESULT result) noexcept = 0;
	/// The game changes the size of the buffers. The function runs before the original `ResizeBuffers`, and must
	/// release each reference to the buffers.
	virtual void BeforeResizeBuffers(IDXGISwapChain* swapChain) noexcept = 0;

protected:
	~SwapChainListener() = default;
};

/// One hook on the code of a function (MinHook). The first bytes of the function become a jump to the hook, and the
/// hook calls the original code through a trampoline.
class CodeHook
{
public:
	/// Make the hook and its trampoline for `target`. The code does not change before `Enable`.
	[[nodiscard]] static ts4::Result<std::unique_ptr<CodeHook>> Install(void* target, void* detour);
	/// Put the jump to the hook into the code of the target.
	[[nodiscard]] ts4::Result<void> Enable();

	~CodeHook() { Restore(); }
	CodeHook(const CodeHook&) = delete;
	CodeHook& operator=(const CodeHook&) = delete;

	/// Put the original code back. If another program changed the first bytes after the install, keep its bytes and
	/// return false: its hook calls a copy of our jump, and that jump must stay correct.
	bool Restore();

	[[nodiscard]] void* Target() const { return _target; }
	/// The trampoline that runs the original code.
	[[nodiscard]] void* Original() const { return _original; }
	[[nodiscard]] bool Installed() const { return _installed; }

	/// The number of bytes of the jump that MinHook writes.
	static constexpr size_t k_JumpSize = 5;

private:
	CodeHook(void* target, void* original)
	    : _target(target)
	    , _original(original)
	{
	}

	void* _target;
	void* _original;
	std::array<uint8_t, k_JumpSize> _jump {};
	bool _installed = false;
};

/// The hooks on `IDXGISwapChain::Present` and `IDXGISwapChain::ResizeBuffers`, and on `IDXGISwapChain1::Present1` and
/// `IDXGISwapChain3::ResizeBuffers1`. The Sims 4 presents its frames with `Present1`.
///
/// The extender makes a small Direct3D 11 device and swap chain of its own on a hidden window, and reads the
/// functions from the vtables of that swap chain. All DXGI swap chains of the process call the same functions. The
/// extender then puts a hook on the code of each function, and does not change the vtable.
///
/// Other programs also hook these functions in the game: the EA overlay (`IGO64.dll`) and the Discord overlay
/// (`DiscordHook64.dll`). The rules that keep the chain of hooks correct:
/// - The vtable stays as DXGI made it. An overlay that reads the vtable then finds the code of DXGI, not our hook.
/// - A hook that is already on the code stays. MinHook copies its jump into our trampoline, so our hook calls it.
/// - The removal puts the original code back only if the first bytes are still our jump.
/// - A hook that our hook calls again on the same thread calls the original function only (`HookDepth`).
class DxgiHooks
{
public:
	/// Find the functions and put the hooks on them. Only one `DxgiHooks` can exist. `log` must stay valid for the
	/// life of the hooks.
	[[nodiscard]] static ts4::Result<std::unique_ptr<DxgiHooks>> Install(SwapChainListener& listener, spdlog::logger& log);

	~DxgiHooks();
	DxgiHooks(const DxgiHooks&) = delete;
	DxgiHooks& operator=(const DxgiHooks&) = delete;

	/// Put the original code back.
	void Restore();

	[[nodiscard]] nlohmann::json Describe() const;

	/// Compare the first bytes of each hooked function and each hook with the bytes after the install. Log each change
	/// one time: a change tells that another program put its own hook on that code. Call it on the main thread.
	void CheckCode();

	[[nodiscard]] SwapChainListener& Listener() const { return _listener; }

	/// Make one more probe swap chain while the hooks are on the code, and release it. An overlay can examine each new
	/// swap chain, so a check of the code after this call tells if the hooks stay safe when the game makes a new swap
	/// chain. Call it on the main thread.
	[[nodiscard]] ts4::Result<void> ProbeAgain();

	/// A hook found a loop: the hooks call each other on one thread. Log it one time, at once.
	void ReportLoop(const char* hook) noexcept;

	/// The slot numbers of the COM interface. The COM ABI fixes the order of the methods of each interface.
	enum Slot : size_t
	{
		k_Present = 8,
		k_ResizeBuffers = 13,
		k_Present1 = 22,
		k_ResizeBuffers1 = 39,
	};

	/// The vtables of the interfaces of the probe swap chain. A vtable of an interface that the swap chain does not have
	/// is null.
	struct Vtables
	{
		void** swapChain = nullptr;
		void** swapChain1 = nullptr;
		void** swapChain3 = nullptr;
	};

private:
	DxgiHooks(SwapChainListener& listener, spdlog::logger& log)
	    : _listener(listener)
	    , _log(log)
	{
	}

	/// The first bytes of a function after the install, and the bytes that the log told last.
	struct CodeWatch
	{
		std::string name;
		const uint8_t* code = nullptr;
		std::array<uint8_t, 16> bytes {};
		std::array<uint8_t, 16> logged {};
		bool warned = false; ///< The log has a warning for a change of this code.
	};

	void HookFunctions(const Vtables& vtables);
	void Watch(std::string name, const void* code);

	SwapChainListener& _listener;
	spdlog::logger& _log;
	std::unique_ptr<CodeHook> _present;
	std::unique_ptr<CodeHook> _resizeBuffers;
	std::unique_ptr<CodeHook> _present1;
	std::unique_ptr<CodeHook> _resizeBuffers1;
	std::string _module;
	std::vector<CodeWatch> _watches;
	std::vector<std::string> _notes;
	std::atomic<bool> _loopReported = false;
};

} // namespace ts4_mod::overlay
