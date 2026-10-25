/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <cstdint>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <common/Result.h>
#include <core/LoaderSettings.h>
#include <core/OverlayMenus.h>
#include <core/OverlayRules.h>
#include <mod_api/Ts4ModApi.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include "game/overlay/DxgiHooks.h"

struct ImGuiContext;
struct ImGuiStyle;

namespace spdlog
{
class logger;
}

namespace ts4_mod::overlay
{

class OverlayRenderer;

/// The title of the game window. The class of the window changes at each start of the game, so the menu bar finds the
/// window by its process and this title.
inline constexpr std::wstring_view k_GameWindowTitle = L"The Sims™ 4";

/// What the extender gives to the menu bar.
struct OverlayHost
{
	/// Run work on the main thread of the game, between two ticks.
	std::function<void(std::function<void()>)> runOnMainThread;
	/// Do a command. The menu bar calls it through `runOnMainThread`.
	std::function<void(const OverlayCommand&)> apply;
	/// The log of the menu bar. If it is null, the menu bar uses the default logger of spdlog.
	std::shared_ptr<spdlog::logger> log;
};

/// The options of the menu bar.
struct OverlayOptions
{
	/// The title of the window that gets the menu bar. The window must also be a top-level window of this process. An
	/// empty title accepts each top-level window of this process.
	std::wstring windowTitle {k_GameWindowTitle};
};

/// Find a visible top-level window of this process with the title, or null. An empty title finds the first visible
/// top-level window of this process.
[[nodiscard]] HWND FindProcessWindow(std::wstring_view title);

/// The menu bar of the mods: a Dear ImGui main menu bar at the top of the game window.
///
/// Threads:
/// - The main thread calls `Install`, `Tick`, `Publish` and `Shutdown`, and runs the commands of the bar.
/// - The thread that presents (the render thread of the game) draws the bar in the hook on `Present`.
/// - The thread of the game window gives the input to the bar in the window procedure.
/// One lock keeps the ImGui context. Any error stops only the menu bar; the game continues.
class Overlay final: public SwapChainListener
{
public:
	/// Install the hooks on the swap chain. Only one menu bar can exist. Do not call it from `DllMain`, because the
	/// probe makes a Direct3D 11 device.
	[[nodiscard]] static ts4::Result<std::unique_ptr<Overlay>> Install(OverlayHost host, OverlayOptions options = {});

	/// Remove the hooks and the window procedure. The destructor waits for a short time for a draw that is in progress.
	~Overlay();
	Overlay(const Overlay&) = delete;
	Overlay& operator=(const Overlay&) = delete;

	/// Calculate the visible state of this frame. `menuOpen` tells if the menu of the game is open. `togglePin` is true
	/// in the frame where the user pressed the hotkey of the menu bar. Call it on the main thread, one time in each tick.
	VisibilityChange Tick(bool menuOpen, bool togglePin);
	/// Give the menu bar the newest state of the mods. Call it on the main thread.
	void Publish(OverlaySnapshot snapshot);
	/// Remove the window procedure and the hooks. The menu bar then never shows again.
	void Shutdown();

	/// Give a command to the main thread, as a control of the bar does. Any thread can call it.
	void Post(OverlayCommand command);

	[[nodiscard]] bool Visible() const { return _visible.load(); }
	[[nodiscard]] nlohmann::json Describe() const;
	/// The hooks on the swap chain, for a check of the code (`DxgiHooks::ProbeAgain`).
	[[nodiscard]] DxgiHooks* Hooks() const { return _hooks.get(); }

	/// The window procedure calls it for each message of the game window.
	[[nodiscard]] InputDecision OnWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;

	void BeforePresent(IDXGISwapChain* swapChain, UINT flags) noexcept override;
	void AfterPresent(IDXGISwapChain* swapChain, HRESULT result) noexcept override;
	void BeforeResizeBuffers(IDXGISwapChain* swapChain) noexcept override;

private:
	Overlay(OverlayHost host, OverlayOptions options);

	/// Stop the menu bar after an error. The hooks and the window procedure stay, but they do nothing.
	void Fail(const std::string& reason) noexcept;
	void SetErrorText(const std::string& error);
	/// Bind the menu bar to the swap chain of the game window. Return false if this swap chain does not draw the bar.
	[[nodiscard]] bool Bind(IDXGISwapChain* swapChain);
	/// Return true if the window is the game window: a top-level window of this process with the title of the options.
	[[nodiscard]] bool IsGameWindow(HWND window) const;
	void Present(IDXGISwapChain* swapChain);
	void DrawFrame(IDXGISwapChain* swapChain);
	/// Set the scale of the text and of the sizes from `overlay.fontScale` and the DPI of the window.
	void ApplyScale();
	void SubclassWindow(HWND window);
	void RestoreWindow();

	OverlayHost _host;
	OverlayOptions _options;
	spdlog::logger& _log;
	std::unique_ptr<DxgiHooks> _hooks;

	/// Keeps the ImGui context, the renderer and the window. A thread that waits longer than a short time does nothing.
	std::timed_mutex _mutex;
	ImGuiContext* _context = nullptr;
	std::unique_ptr<OverlayRenderer> _renderer;
	std::atomic<IDXGISwapChain*> _swapChain = nullptr;
	HWND _window = nullptr;
	std::atomic<bool> _rebuild = false;
	/// The scale that the style has now, and the style before any scale.
	float _scale = 0.0f;
	std::unique_ptr<ImGuiStyle> _baseStyle;

	std::atomic<bool> _visible = false;
	std::atomic<bool> _pinned = false;
	std::atomic<bool> _failed = false;
	std::atomic<uint64_t> _presentCalls = 0;
	std::atomic<uint64_t> _framesDrawn = 0;
	std::atomic<uint64_t> _mainFrames = 0;
	bool _noFramesLogged = false;

	/// Main thread only.
	OverlayVisibility _visibility;

	std::mutex _snapshotMutex;
	OverlaySnapshot _snapshot;
	uint64_t _snapshotRevision = 0;

	/// Render thread only: the copy of the snapshot that the bar draws, and the settings that the controls change.
	OverlaySnapshot _shown;
	uint64_t _shownRevision = 0;
	/// The values that the controls show, by the id of the field.
	std::map<size_t, Ts4FieldValue> _draft;
	loader::LoaderSettings _draftLoader;
	std::atomic<int> _pendingEdits = 0;
	/// False after the destructor starts. A command that the main thread runs later then does nothing. The main thread
	/// runs the commands and the destructor, so the flag needs no lock.
	std::shared_ptr<bool> _alive = std::make_shared<bool>(true);

	mutable std::mutex _statusMutex;
	std::string _lastError;
};

} // namespace ts4_mod::overlay
