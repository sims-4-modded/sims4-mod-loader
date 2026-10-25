/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include "game/overlay/Overlay.h"

#include <array>
#include <chrono>
#include <exception>
#include <format>
#include <stdexcept>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <mod_game/Guard.h>
#include <spdlog/spdlog.h>

#include "game/overlay/OverlayRenderer.h"
#include "game/overlay/OverlayUi.h"

// The Win32 backend of ImGui declares its window procedure handler only for code that asks for it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ts4_mod::overlay
{

namespace
{
/// The menu bar of the process. The window procedure is a plain function, so it finds the menu bar here.
std::atomic<Overlay*> s_overlay = nullptr;
/// The window procedure of the game, which the menu bar calls for each message.
std::atomic<WNDPROC> s_gameWindowProc = nullptr;

/// The longest time that a thread waits for the ImGui context. A longer wait skips the work, so that no thread of the
/// game stops.
constexpr auto k_LockTimeout = std::chrono::milliseconds(100);
/// The ticks with no `Present` after which the log tells that the hooks see no frame.
constexpr uint64_t k_NoPresentTicks = 3600;
/// The ticks between two checks of the code of the hooks.
constexpr uint64_t k_CodeCheckTicks = 30;

LRESULT CALLBACK OverlayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (auto* overlay = s_overlay.load(); overlay != nullptr)
	{
		if (overlay->OnWindowMessage(window, message, wParam, lParam).swallow)
		{
			// Raw input needs the default procedure, which releases the data of the message.
			return message == WM_INPUT ? DefWindowProcW(window, message, wParam, lParam) : 0;
		}
	}
	const auto game = s_gameWindowProc.load();
	return game != nullptr ? CallWindowProcW(game, window, message, wParam, lParam)
	                       : DefWindowProcW(window, message, wParam, lParam);
}

/// Return true if the window is a top-level window of this process.
[[nodiscard]] bool IsProcessTopWindow(HWND window)
{
	DWORD process = 0;
	GetWindowThreadProcessId(window, &process);
	return window != nullptr && process == GetCurrentProcessId() && GetAncestor(window, GA_ROOT) == window;
}

/// The title of a window.
[[nodiscard]] std::wstring WindowTitle(HWND window)
{
	std::array<wchar_t, 256> title {};
	const auto length = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
	return std::wstring(title.data(), length > 0 ? static_cast<size_t>(length) : 0);
}

[[nodiscard]] spdlog::logger& LoggerOf(const OverlayHost& host)
{
	return host.log ? *host.log : *spdlog::default_logger_raw();
}
} // namespace

HWND FindProcessWindow(std::wstring_view title)
{
	struct Search
	{
		std::wstring_view title;
		HWND found = nullptr;
	} search {title};
	EnumWindows(
	    [](HWND window, LPARAM context) -> BOOL {
		    auto& s = *reinterpret_cast<Search*>(context);
		    if (IsProcessTopWindow(window) && IsWindowVisible(window) && WindowTitleMatches(WindowTitle(window), s.title))
		    {
			    s.found = window;
			    return FALSE;
		    }
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&search));
	return search.found;
}

ts4::Result<std::unique_ptr<Overlay>> Overlay::Install(OverlayHost host, OverlayOptions options)
{
	if (s_overlay.load() != nullptr)
	{
		return ts4::MakeError("the menu bar is already installed");
	}
	auto overlay = std::unique_ptr<Overlay>(new Overlay(std::move(host), std::move(options)));
	auto& log = overlay->_log;
	const auto loaded = [](const wchar_t* name) { return GetModuleHandleW(name) != nullptr; };
	log.info("menu bar: the process loaded d3d11.dll {}, dxgi.dll {}, IGO64.dll {}, DiscordHook64.dll {}", loaded(L"d3d11.dll"),
	         loaded(L"dxgi.dll"), loaded(L"IGO64.dll"), loaded(L"DiscordHook64.dll"));
	log.info("menu bar: the game window {}", FindProcessWindow(overlay->_options.windowTitle) != nullptr
	                                             ? "is open"
	                                             : "is not open yet; the bar binds to it at its first frame");
	auto hooks = DxgiHooks::Install(*overlay, log);
	if (!hooks)
	{
		return hooks.GetError();
	}
	overlay->_hooks = std::move(*hooks);
	s_overlay = overlay.get();
	return overlay;
}

Overlay::Overlay(OverlayHost host, OverlayOptions options)
    : _host(std::move(host))
    , _options(std::move(options))
    , _log(LoggerOf(_host))
{
}

Overlay::~Overlay()
{
	*_alive = false;
	Shutdown();
	// The hooks wait for the calls that are in progress, so that no call uses the members after this point.
	_hooks.reset();
	std::unique_lock lock(_mutex, k_LockTimeout);
	if (!lock.owns_lock() || _context == nullptr)
	{
		// A draw is still in progress. The context and the renderer stay in memory; the bar does not use them again.
		(void)_renderer.release();
		return;
	}
	ImGui::SetCurrentContext(_context);
	_renderer.reset();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext(_context);
	_context = nullptr;
}

void Overlay::Shutdown()
{
	_failed = true;
	_visible = false;
	RestoreWindow();
	if (_hooks)
	{
		_hooks->Restore();
	}
	Overlay* self = this;
	s_overlay.compare_exchange_strong(self, nullptr);
}

void Overlay::Fail(const std::string& reason) noexcept
{
	if (_failed.exchange(true))
	{
		return;
	}
	_visible = false;
	try
	{
		_log.error("menu bar: {}; the menu bar stops, the game continues", reason);
		SetErrorText(reason);
	}
	catch (...)
	{
		// The log failed. The menu bar stops all the same.
	}
}

void Overlay::SetErrorText(const std::string& error)
{
	const std::scoped_lock lock(_statusMutex);
	_lastError = error;
}

VisibilityChange Overlay::Tick(bool menuOpen, bool togglePin)
{
	_visibility.SetEnabled(!_failed.load());
	const auto change = _visibility.Update(menuOpen, togglePin);
	_pinned = _visibility.Pinned();
	_visible = change.visible;
	if (change.shown || change.hidden)
	{
		_log.info("menu bar: {} (game menu {}, pinned {})", change.shown ? "shows" : "hides", menuOpen, _visibility.Pinned());
		// The keys and the buttons that ImGui holds go up, so that no control stays active while the bar hides.
		if (std::unique_lock lock(_mutex, k_LockTimeout); lock.owns_lock() && _context != nullptr && !_failed.load())
		{
			ImGui::SetCurrentContext(_context);
			ImGui::GetIO().AddFocusEvent(change.shown && GetForegroundWindow() == _window);
		}
	}
	const auto ticks = ++_mainFrames;
	if (_hooks && ticks % k_CodeCheckTicks == 0)
	{
		_hooks->CheckCode();
	}
	if (ticks >= k_NoPresentTicks && _presentCalls.load() == 0 && !_noFramesLogged)
	{
		_noFramesLogged = true;
		_log.warn("menu bar: no Present call came through the hooks in {} ticks. The game uses another swap chain or "
		          "another graphics API; the menu bar cannot show",
		          ticks);
		SetErrorText("no Present call came through the hooks");
	}
	return change;
}

void Overlay::Publish(OverlaySnapshot snapshot)
{
	const std::scoped_lock lock(_snapshotMutex);
	_snapshot = std::move(snapshot);
	++_snapshotRevision;
}

nlohmann::json Overlay::Describe() const
{
	const std::scoped_lock lock(_statusMutex);
	return {
	    {"api", _swapChain.load() != nullptr ? "Direct3D 11" : "not known yet"},
	    {"visible", _visible.load()},
	    {"pinned", _pinned.load()},
	    {"failed", _failed.load()},
	    {"presentCalls", _presentCalls.load()},
	    {"framesDrawn", _framesDrawn.load()},
	    {"lastError", _lastError},
	    {"hooks", _hooks ? _hooks->Describe() : nlohmann::json()},
	};
}

InputDecision Overlay::OnWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
	if (_failed.load() || !_visible.load())
	{
		return {};
	}
	struct Call
	{
		Overlay* self;
		HWND window;
		UINT message;
		WPARAM wParam;
		LPARAM lParam;
		InputDecision decision;
	} call {this, window, message, wParam, lParam, {}};
	const auto work = [](void* context) {
		auto& c = *static_cast<Call*>(context);
		std::unique_lock lock(c.self->_mutex, k_LockTimeout);
		if (!lock.owns_lock() || c.self->_context == nullptr || c.window != c.self->_window)
		{
			return;
		}
		ImGui::SetCurrentContext(c.self->_context);
		const auto& io = ImGui::GetIO();
		c.decision = DecideInput(ClassifyMessage(c.message), c.wParam, true, io.WantCaptureMouse, io.WantCaptureKeyboard);
		if (c.decision.feedOverlay)
		{
			ImGui_ImplWin32_WndProcHandler(c.window, c.message, c.wParam, c.lParam);
		}
	};
	if (const auto guard = GuardedCall(work, &call, &CatchCallbackFault); !guard.completed)
	{
		Fail("the window procedure failed: " + guard.Describe());
		return {};
	}
	return call.decision;
}

void Overlay::BeforePresent(IDXGISwapChain* swapChain, UINT flags) noexcept
{
	// While the bar does not show, the hook only counts the call and reads two flags. The bar does nothing with the
	// device, the window or ImGui before it shows the first time.
	if (_presentCalls.fetch_add(1, std::memory_order_relaxed) == 0)
	{
		try
		{
			_log.info("menu bar: the first Present came through the hook on thread {}", GetCurrentThreadId());
		}
		catch (...)
		{
			// The log failed. The bar continues.
		}
	}
	if (!_visible.load(std::memory_order_relaxed) || _failed.load(std::memory_order_relaxed) ||
	    (flags & DXGI_PRESENT_TEST) != 0)
	{
		return;
	}
	struct Call
	{
		Overlay* self;
		IDXGISwapChain* swapChain;
	} call {this, swapChain};
	const auto work = [](void* context) {
		auto& c = *static_cast<Call*>(context);
		c.self->Present(c.swapChain);
	};
	if (const auto guard = GuardedCall(work, &call, &CatchCallbackFault); !guard.completed)
	{
		Fail("the code that draws failed: " + guard.Describe());
	}
}

void Overlay::AfterPresent(IDXGISwapChain* swapChain, HRESULT result) noexcept
{
	if (swapChain == _swapChain && (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET))
	{
		// The renderer starts again on the new device at the next frame.
		_rebuild = true;
	}
}

void Overlay::BeforeResizeBuffers(IDXGISwapChain* swapChain) noexcept
{
	if (_failed.load() || swapChain != _swapChain)
	{
		return;
	}
	const auto work = [](void* context) {
		auto* self = static_cast<Overlay*>(context);
		std::unique_lock lock(self->_mutex, k_LockTimeout);
		if (!lock.owns_lock())
		{
			// The renderer cannot release the buffers now, so it starts again at the next frame.
			self->_rebuild = true;
			return;
		}
		if (self->_renderer)
		{
			ImGui::SetCurrentContext(self->_context);
			self->_renderer->ReleaseBackBuffers();
		}
	};
	if (const auto guard = GuardedCall(work, this, &CatchCallbackFault); !guard.completed)
	{
		Fail("the code before ResizeBuffers failed: " + guard.Describe());
	}
}

bool Overlay::IsGameWindow(HWND window) const
{
	return IsProcessTopWindow(window) && WindowTitleMatches(WindowTitle(window), _options.windowTitle);
}

bool Overlay::Bind(IDXGISwapChain* swapChain)
{
	if (swapChain == _swapChain && !_rebuild.load())
	{
		return _renderer != nullptr;
	}
	DXGI_SWAP_CHAIN_DESC description {};
	if (FAILED(swapChain->GetDesc(&description)) || !IsGameWindow(description.OutputWindow))
	{
		return false;
	}
	// A swap chain of another window does not draw the bar.
	if (_window != nullptr && description.OutputWindow != _window && swapChain != _swapChain)
	{
		return false;
	}
	if (!IsD3D11SwapChain(swapChain))
	{
		throw std::runtime_error("the swap chain of the game window has no Direct3D 11 device");
	}
	std::unique_lock lock(_mutex, k_LockTimeout);
	if (!lock.owns_lock())
	{
		return false;
	}
	if (_context == nullptr)
	{
		_log.info("menu bar: bind step 1: the bar makes the ImGui context on thread {}", GetCurrentThreadId());
		_context = ImGui::CreateContext();
		ImGui::SetCurrentContext(_context);
		auto& io = ImGui::GetIO();
		// The menu bar writes no file: no imgui.ini and no log, in the game folder or anywhere else.
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		// The bar never changes or draws a cursor. The game shows its own cursor, and that cursor is the only cursor on
		// the screen.
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		io.MouseDrawCursor = false;
		ImGui::StyleColorsDark();
		_baseStyle = std::make_unique<ImGuiStyle>(ImGui::GetStyle());
		_scale = 1.0f;
		_log.info("menu bar: bind step 2: the Win32 backend of ImGui starts");
		if (!ImGui_ImplWin32_Init(description.OutputWindow))
		{
			throw std::runtime_error("the Win32 backend of ImGui did not start");
		}
		_window = description.OutputWindow;
		_log.info("menu bar: bind step 3: the bar puts its window procedure on the game window");
		SubclassWindow(_window);
	}
	ImGui::SetCurrentContext(_context);
	_renderer.reset();
	_log.info("menu bar: bind step 4: the bar makes its renderer on the device of the game");
	auto renderer = OverlayRenderer::Create(swapChain);
	if (!renderer)
	{
		throw std::runtime_error(renderer.GetError().message);
	}
	_renderer = std::move(*renderer);
	_swapChain = swapChain;
	_rebuild = false;
	_log.info("menu bar: draws with Direct3D 11 on the swap chain of the game window ({}x{})", description.BufferDesc.Width,
	          description.BufferDesc.Height);
	return true;
}

void Overlay::Present(IDXGISwapChain* swapChain)
{
	if (!Bind(swapChain))
	{
		return;
	}
	std::unique_lock lock(_mutex, k_LockTimeout);
	if (lock.owns_lock())
	{
		DrawFrame(swapChain);
	}
}

void Overlay::ApplyScale()
{
	const auto scale = StyleScale(_shown.requested.overlay.fontScale, ImGui_ImplWin32_GetDpiScaleForHwnd(_window));
	auto& style = ImGui::GetStyle();
	if (scale != _scale && _baseStyle)
	{
		// The scale of the sizes changes the values of the style, so each new scale starts from the style with no scale.
		style = *_baseStyle;
		style.ScaleAllSizes(scale);
		_scale = scale;
	}
	style.FontScaleMain = scale;
	style.FontScaleDpi = 1.0f;
}

void Overlay::DrawFrame(IDXGISwapChain* swapChain)
{
	ImGui::SetCurrentContext(_context);
	{
		const std::scoped_lock lock(_snapshotMutex);
		if (_snapshotRevision != _shownRevision)
		{
			_shown = _snapshot;
			_shownRevision = _snapshotRevision;
		}
	}
	_renderer->NewFrame();
	ImGui_ImplWin32_NewFrame();
	ApplyScale();
	ImGui::NewFrame();
	// The controls show the settings of the mods, except while the user changes a control or a change waits for the
	// main thread.
	if (!ImGui::IsAnyItemActive() && _pendingEdits.load() == 0)
	{
		_draft = FieldValues(_shown);
		_draftLoader = _shown.requested;
	}
	const auto status = Describe();
	const std::function<void(OverlayCommand)> post = [this](OverlayCommand command) { Post(std::move(command)); };
	DrawOverlayMenuBar(OverlayUiFrame {
	    .snapshot = _shown, .draft = _draft, .draftLoader = _draftLoader, .post = post, .overlayStatus = status});
	ImGui::Render();
	switch (_renderer->Render(swapChain, ImGui::GetDrawData()))
	{
	case RenderResult::Drawn:
		++_framesDrawn;
		break;
	case RenderResult::Failed:
		throw std::runtime_error("the Direct3D 11 renderer failed");
	}
}

void Overlay::Post(OverlayCommand command)
{
	++_pendingEdits;
	_host.runOnMainThread([this, alive = _alive, command = std::move(command)] {
		if (!*alive)
		{
			return;
		}
		try
		{
			_host.apply(command);
		}
		catch (const std::exception& error)
		{
			_log.error("menu bar: the change failed: {}", error.what());
			SetErrorText(error.what());
		}
		--_pendingEdits;
	});
}

void Overlay::SubclassWindow(HWND window)
{
	const auto previous = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
	if (previous == nullptr || previous == &OverlayWindowProc)
	{
		throw std::runtime_error("the menu bar cannot read the window procedure of the game");
	}
	s_gameWindowProc = previous;
	::SetLastError(0);
	if (SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&OverlayWindowProc)) == 0 && GetLastError() != 0)
	{
		throw std::runtime_error(std::format("SetWindowLongPtrW failed with error {}", GetLastError()));
	}
	_log.info("menu bar: the window procedure of the game window has the input handler of the bar");
}

void Overlay::RestoreWindow()
{
	const auto game = s_gameWindowProc.load();
	if (_window == nullptr || game == nullptr)
	{
		return;
	}
	// Put the procedure of the game back only if no other program changed it after the bar.
	if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(_window, GWLP_WNDPROC)) == &OverlayWindowProc)
	{
		SetWindowLongPtrW(_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(game));
	}
}

} // namespace ts4_mod::overlay
