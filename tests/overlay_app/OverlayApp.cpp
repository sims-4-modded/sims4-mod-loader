/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

// A small Direct3D 11 window that draws the menu bar of the extender with a fake registry of two mods. It checks the
// parts that the tests of CTest cannot check: the probe, the hook on Present, the window procedure and the draw.
//
// Usage: ts4_overlay_app [--seconds <n>] [--font-scale <x>]. The app stops after the time (10 seconds by default),
// writes the state of the menu bar as JSON, and returns 0 if the menu bar drew at least one frame. The font scale is the
// start value of `overlay.fontScale`.

#include <cstdio>
#include <cstdlib>

#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/LoaderSettings.h>
#include <core/ModCatalogue.h>
#include <core/ModRegistry.h>
#include <core/OverlayMenus.h>
#include <d3d11.h>
#include <game/overlay/Overlay.h>
#include <spdlog/spdlog.h>
#include <windows.h>
#include <wrl/client.h>

namespace
{

using Microsoft::WRL::ComPtr;
using namespace ts4_mod;
using namespace ts4_mod::loader;
using namespace ts4_mod::overlay;

constexpr wchar_t k_WindowClass[] = L"Ts4OverlayTestApp";
constexpr wchar_t k_WindowTitle[] = L"TS4 overlay test";
constexpr int k_Width = 1280;
constexpr int k_Height = 720;
/// The ticks between two snapshots of the registry for the menu bar.
constexpr uint64_t k_SnapshotTicks = 15;

/// The values of the fake mods and of the extender. The C callbacks of the registry change them.
struct FakeState
{
	bool cameraEnabled = true;
	double cameraZoom = 2.0;
	uint32_t cameraMode = 1;
	bool needsFreeze = false;
	double needsDecay = 1.0;
	int resets = 0;
	double fontScale = 1.0;
};

/// A field of a number or a toggle in `FakeState`.
struct FieldBinding
{
	FakeState* state = nullptr;
	double* number = nullptr;
	bool* toggle = nullptr;
	uint32_t* choice = nullptr;
};

void GetField(void* user, Ts4FieldState* state)
{
	const auto& field = *static_cast<FieldBinding*>(user);
	if (field.number != nullptr)
	{
		state->value.number = *field.number;
	}
	if (field.toggle != nullptr)
	{
		state->value.toggle = *field.toggle ? 1u : 0u;
	}
	if (field.choice != nullptr)
	{
		state->value.choice = *field.choice;
	}
}

uint32_t SetField(void* user, const Ts4FieldValue* value, uint32_t /*save*/, Ts4Text* message)
{
	const auto& field = *static_cast<FieldBinding*>(user);
	if (field.number != nullptr)
	{
		*field.number = value->number;
	}
	if (field.toggle != nullptr)
	{
		*field.toggle = value->toggle != 0;
	}
	if (field.choice != nullptr)
	{
		*field.choice = value->choice;
	}
	constexpr std::string_view text = "the fake field changed";
	message->assign(message->context, text.data(), text.size());
	return 1;
}

uint32_t LoadSettings(void* /*user*/, const char* /*json*/, Ts4Text* /*error*/)
{
	return 1;
}

void ResetCamera(void* user)
{
	auto& state = *static_cast<FakeState*>(user);
	state.cameraZoom = 2.0;
	++state.resets;
}

/// The app pins the bar itself, so the action of the hotkey does nothing.
void PinBar(void* /*user*/) {}

uint32_t CameraEnabled(void* user)
{
	return static_cast<FakeState*>(user)->cameraEnabled ? 1u : 0u;
}

void ReadCamera(void* user, Ts4Text* text)
{
	const auto& state = *static_cast<FakeState*>(user);
	const auto line = std::format("Zoom {:.2f}, {} resets", state.cameraZoom, state.resets);
	text->assign(text->context, line.data(), line.size());
}

void ReadNeeds(void* user, Ts4Text* text)
{
	const auto& state = *static_cast<FakeState*>(user);
	const auto line = std::format("Decay x{:.2f}{}", state.needsDecay, state.needsFreeze ? ", frozen" : "");
	text->assign(text->context, line.data(), line.size());
}

[[nodiscard]] Ts4FieldDesc Field(const char* section, const char* key, const char* label, Ts4FieldKind kind, double min,
                                 double max)
{
	return Ts4FieldDesc {.menu = nullptr,
	                     .group = "Settings",
	                     .section = section,
	                     .key = key,
	                     .label = label,
	                     .help = "A field of the fake mod.",
	                     .kind = kind,
	                     .min = min,
	                     .max = max,
	                     .step = 0.0,
	                     .format = "%.2f",
	                     .logarithmic = 0,
	                     .restartOnly = 0,
	                     .choices = nullptr,
	                     .choiceCount = 0};
}

/// Copies a `Ts4Text` into a string.
struct TextBox
{
	std::string text;

	[[nodiscard]] Ts4Text Target()
	{
		return Ts4Text {.context = this, .assign = [](void* context, const char* utf8, size_t size) {
			                static_cast<TextBox*>(context)->text.assign(utf8, size);
		                }};
	}
};

/// Reads the registry with direct calls. The extender calls the functions of a mod with its guard; the app has no
/// mods that fail.
class DirectReader final: public MenuReader
{
public:
	bool ReadField(const FieldEntry& field, Ts4FieldState& state) override
	{
		field.get(field.user, &state);
		return true;
	}
	bool ActionEnabled(const ActionEntry& action) override { return action.enabled(action.user) != 0; }
	std::optional<std::string> ReadReadout(const ReadoutEntry& readout) override
	{
		TextBox box;
		auto target = box.Target();
		readout.read(readout.user, &target);
		return box.text;
	}
};

/// The two fake mods and the field of the extender for the size of the text.
struct FakeMods
{
	FakeMods()
	{
		registry.SetOwnerName(k_LoaderOwner, "Extender");
		registry.SetOwnerName(1, "Camera Mod");
		registry.SetOwnerName(2, "Better Needs");
		bindings = {
		    FieldBinding {.state = &state, .number = &state.fontScale},
		    FieldBinding {.state = &state, .toggle = &state.cameraEnabled},
		    FieldBinding {.state = &state, .number = &state.cameraZoom},
		    FieldBinding {.state = &state, .choice = &state.cameraMode},
		    FieldBinding {.state = &state, .toggle = &state.needsFreeze},
		    FieldBinding {.state = &state, .number = &state.needsDecay},
		};
		static constexpr const char* k_Modes[] = {"Classic", "Free", "Follow"};
		auto scale = Field("overlay", "fontScale", "Text size", TS4_FIELD_NUMBER, 0.5, 3.0);
		scale.group = "Menu bar";
		Check(registry.AddField(k_LoaderOwner, scale, &GetField, &SetField, &bindings[0]));
		Check(registry.AddSettings(1, "camera", &LoadSettings, nullptr));
		Check(registry.AddSettings(2, "needs", &LoadSettings, nullptr));
		Check(registry.AddField(1, Field("camera", "enabled", "Camera on", TS4_FIELD_TOGGLE, 0, 0), &GetField, &SetField,
		                        &bindings[1]));
		Check(registry.AddField(1, Field("camera", "zoom", "Zoom", TS4_FIELD_NUMBER, 0.5, 8.0), &GetField, &SetField,
		                        &bindings[2]));
		auto mode = Field("camera", "mode", "Mode", TS4_FIELD_CHOICE, 0, 0);
		mode.choices = k_Modes;
		mode.choiceCount = 3;
		Check(registry.AddField(1, mode, &GetField, &SetField, &bindings[3]));
		const Ts4ActionDesc reset {.menu = nullptr,
		                           .group = "Actions",
		                           .label = "Reset the camera",
		                           .help = "Set the zoom to 2.",
		                           .hotkey = "resetCamera",
		                           .defaultHotkey = "Ctrl+Shift+F5"};
		Check(registry.AddAction(1, reset, &ResetCamera, &CameraEnabled, &state));
		Check(registry.AddReadout(1, nullptr, "State", &ReadCamera, &state));
		Check(registry.AddField(2, Field("needs", "freeze", "Freeze the needs", TS4_FIELD_TOGGLE, 0, 0), &GetField, &SetField,
		                        &bindings[4]));
		Check(registry.AddField(2, Field("needs", "decay", "Decay", TS4_FIELD_NUMBER, 0.0, 4.0), &GetField, &SetField,
		                        &bindings[5]));
		Check(registry.AddReadout(2, nullptr, "State", &ReadNeeds, &state));
		const Ts4ActionDesc pin {.menu = nullptr,
		                         .group = nullptr,
		                         .label = nullptr,
		                         .help = nullptr,
		                         .hotkey = "toggleOverlay",
		                         .defaultHotkey = "Ctrl+Shift+F4"};
		Check(registry.AddAction(k_LoaderOwner, pin, &PinBar, nullptr, nullptr));
		std::vector<std::string> errors;
		hotkeys = registry.ResolveHotkeys({}, errors);
	}

	template <typename T>
	static void Check(const ts4::Result<T>& result)
	{
		if (!result)
		{
			std::fprintf(stderr, "registry: %s\n", result.GetError().message.c_str());
			std::exit(2);
		}
	}

	[[nodiscard]] OverlaySnapshot Snapshot()
	{
		requested.overlay.fontScale = state.fontScale;
		DirectReader reader;
		return OverlaySnapshot {
		    .settings = settings,
		    .requested = requested,
		    .menus = BuildMenus(registry, hotkeys, reader),
		    .mods = nlohmann::json::array({{{"name", "Camera Mod"}, {"enabled", true}, {"ready", true}, {"active", true}},
		                                   {{"name", "Better Needs"}, {"enabled", true}, {"ready", true}, {"active", false}}}),
		    .hooks = {{"ZoneLoad", {{"installed", true}, {"calls", 3}}}},
		    .lastReload = {{"time", "12:00:00"}, {"source", "start"}, {"applied", true}},
		    .settingsPath = "C:\\Users\\you\\AppData\\Local\\Sims4Mods\\mods.json",
		    .gameVersion = "1.128.90.1030",
		    .version = "0.1.0",
		    .live = true,
		    .hotkeysReady = true,
		    .dllList = nlohmann::json::array({mods::DescribeModDll({.file = "camera_mod.dll",
		                                                            .sha256 = std::string(64, 'a'),
		                                                            .state = mods::ModDllState::Loaded,
		                                                            .id = "camera_mod",
		                                                            .name = "Camera Mod",
		                                                            .version = "1.0.0"}),
		                                      mods::DescribeModDll({.file = "better_needs.dll",
		                                                            .sha256 = std::string(64, 'b'),
		                                                            .state = mods::ModDllState::Loaded,
		                                                            .id = "better_needs",
		                                                            .name = "Better Needs",
		                                                            .version = "0.3.0"}),
		                                      mods::DescribeModDll({.file = "broken.dll",
		                                                            .sha256 = std::string(64, 'c'),
		                                                            .state = mods::ModDllState::Refused,
		                                                            .error = "the DLL has no export Ts4ModGetInfo"})}),
		    .dllFolder = "C:\\Users\\you\\AppData\\Local\\Sims4Mods\\dlls",
		};
	}

	/// Do a command of the menu bar, as the extender does on the main thread.
	void Apply(const OverlayCommand& command)
	{
		switch (command.kind)
		{
		case OverlayCommand::Kind::Edit:
			if (const auto* field = registry.FindField(command.id); field != nullptr)
			{
				TextBox message;
				auto target = message.Target();
				(void)field->set(field->user, &command.value, command.save ? 1u : 0u, &target);
				spdlog::info("app: edit of {}: {}", field->Path(), message.text);
			}
			break;
		case OverlayCommand::Kind::Action:
			if (const auto* action = registry.FindAction(command.id); action != nullptr)
			{
				action->run(action->user);
				spdlog::info("app: action {}", action->label);
			}
			break;
		case OverlayCommand::Kind::Reload:
			spdlog::info("app: reload of mods.json");
			break;
		case OverlayCommand::Kind::Mod:
			(void)mods::EditModEnabled(requested, command.name, command.on);
			spdlog::info("app: mod {} {}", command.name, command.on ? "on" : "off");
			break;
		case OverlayCommand::Kind::ModDll:
			(void)mods::EditModDllEnabled(requested, command.name, command.on);
			spdlog::info("app: DLL {} {}", command.name, command.on ? "on" : "off");
			break;
		}
	}

	FakeState state;
	std::vector<FieldBinding> bindings;
	ModRegistry registry;
	std::vector<HotkeyBinding> hotkeys;
	LoaderSettings settings;
	LoaderSettings requested;
};

/// The tasks for the main thread, as the extender keeps them.
class MainThreadQueue
{
public:
	void Post(std::function<void()> task)
	{
		const std::scoped_lock lock(_mutex);
		_tasks.push_back(std::move(task));
	}

	void RunAll()
	{
		std::vector<std::function<void()>> tasks;
		{
			const std::scoped_lock lock(_mutex);
			tasks.swap(_tasks);
		}
		for (auto& task : tasks)
		{
			task();
		}
	}

private:
	std::mutex _mutex;
	std::vector<std::function<void()>> _tasks;
};

LRESULT CALLBACK AppWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_DESTROY)
	{
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

struct Graphics
{
	ComPtr<IDXGISwapChain> swapChain;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11RenderTargetView> target;
};

[[nodiscard]] std::optional<Graphics> MakeGraphics(HWND window)
{
	for (const auto driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
	{
		DXGI_SWAP_CHAIN_DESC description {};
		description.BufferDesc = {.Width = k_Width, .Height = k_Height, .Format = DXGI_FORMAT_B8G8R8A8_UNORM};
		description.SampleDesc = {.Count = 1, .Quality = 0};
		description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		description.BufferCount = 2;
		description.OutputWindow = window;
		description.Windowed = TRUE;
		description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		Graphics graphics;
		if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &description,
		                                         &graphics.swapChain, &graphics.device, nullptr, &graphics.context)))
		{
			continue;
		}
		ComPtr<ID3D11Texture2D> buffer;
		if (FAILED(graphics.swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))) ||
		    FAILED(graphics.device->CreateRenderTargetView(buffer.Get(), nullptr, &graphics.target)))
		{
			continue;
		}
		return graphics;
	}
	return std::nullopt;
}

[[nodiscard]] int Run(double seconds, double fontScale)
{
	const auto instance = GetModuleHandleW(nullptr);
	const WNDCLASSEXW windowClass {.cbSize = sizeof(WNDCLASSEXW),
	                               .lpfnWndProc = &AppWindowProc,
	                               .hInstance = instance,
	                               .hCursor = LoadCursorA(nullptr, IDC_ARROW),
	                               .lpszClassName = k_WindowClass};
	RegisterClassExW(&windowClass);
	RECT frame {0, 0, k_Width, k_Height};
	AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
	const auto window =
	    CreateWindowExW(0, k_WindowClass, k_WindowTitle, WS_OVERLAPPEDWINDOW, 100, 100, frame.right - frame.left,
	                    frame.bottom - frame.top, nullptr, nullptr, instance, nullptr);
	if (window == nullptr)
	{
		spdlog::error("app: the window could not open");
		return 2;
	}
	ShowWindow(window, SW_SHOWNOACTIVATE);
	auto graphics = MakeGraphics(window);
	if (!graphics)
	{
		spdlog::error("app: no Direct3D 11 device");
		return 2;
	}

	FakeMods mods;
	mods.state.fontScale = fontScale;
	MainThreadQueue queue;
	OverlayHost host {
	    .runOnMainThread = [&queue](std::function<void()> task) { queue.Post(std::move(task)); },
	    .apply = [&mods](const OverlayCommand& command) { mods.Apply(command); },
	    .log = spdlog::default_logger(),
	};
	auto overlay = Overlay::Install(std::move(host), OverlayOptions {.windowTitle = k_WindowTitle});
	if (!overlay)
	{
		spdlog::error("app: the menu bar did not install: {}", overlay.GetError().message);
		return 1;
	}
	spdlog::info("app: the window found by its title: {}", FindProcessWindow(k_WindowTitle) == window);

	const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
	uint64_t tick = 0;
	bool running = true;
	while (running && std::chrono::steady_clock::now() < end)
	{
		MSG message {};
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
		{
			if (message.message == WM_QUIT)
			{
				running = false;
			}
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		// The main thread of the app does the work of the main thread of the game: the tick, the tasks and the
		// snapshots. The first tick pins the bar, as the hotkey does.
		(void)(*overlay)->Tick(false, tick == 0);
		queue.RunAll();
		if (tick % k_SnapshotTicks == 0)
		{
			(*overlay)->Publish(mods.Snapshot());
		}
		++tick;
		const float shade = 0.15f + 0.05f * static_cast<float>(tick % 120) / 120.0f;
		const float colour[] = {shade, shade * 1.4f, shade * 2.0f, 1.0f};
		ID3D11RenderTargetView* targets[] = {graphics->target.Get()};
		graphics->context->OMSetRenderTargets(1, targets, nullptr);
		graphics->context->ClearRenderTargetView(graphics->target.Get(), colour);
		graphics->swapChain->Present(1, 0);
	}

	const auto status = (*overlay)->Describe();
	std::printf("%s\n", status.dump(2).c_str());
	const bool drawn = status.value("framesDrawn", uint64_t {0}) > 0;
	overlay->reset();
	if (IsWindow(window))
	{
		DestroyWindow(window);
	}
	spdlog::info("app: {} ticks; the menu bar {}", tick, drawn ? "drew" : "did not draw");
	return drawn ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
	double seconds = 10.0;
	double fontScale = 1.0;
	for (int index = 1; index + 1 < argc; ++index)
	{
		const std::string_view option = argv[index];
		if (option == "--seconds")
		{
			seconds = std::atof(argv[index + 1]);
		}
		else if (option == "--font-scale")
		{
			fontScale = std::atof(argv[index + 1]);
		}
	}
	spdlog::set_level(spdlog::level::info);
	return Run(seconds, fontScale);
}
