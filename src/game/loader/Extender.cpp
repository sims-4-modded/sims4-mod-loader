/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

// The extender in the game process: a port of the start of the Masters of Albion mod loader. The start thread reads
// mods.json, starts the start-up guard, loads the DLL mods and puts the bootstrap hook on the tick function (the update
// of the client services). The first tick runs on the main thread: it makes the hook manager there, puts the hooks of
// the mods on their functions and then gives the tick to the hook manager. Each tick then reads the hotkeys, shows the
// menu bar, does the late steps of the start and runs the tasks of the main thread.

#include "game/loader/Extender.h"

#include <cstdint>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <core/HookRules.h>
#include <core/HostTable.h>
#include <core/LoaderSettings.h>
#include <core/ModCatalogue.h>
#include <core/ModRegistry.h>
#include <core/OverlayMenus.h>
#include <dlls/ModDllLoader.h>
#include <mod_core/FrameCallbacks.h>
#include <mod_core/MainThreadTasks.h>
#include <mod_core/SettingsFile.h>
#include <mod_core/StartupGuard.h>
#include <nlohmann/json.hpp>
#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>
#include <windows.h>

#include "game/loader/TickHook.h"
#include "game/overlay/Overlay.h"
#include "mod_game/Hooks.h"
#include "mod_game/HotkeyInput.h"
#include "mod_game/Module.h"
#include "mod_game/Scan.h"

namespace ts4_mod::loader
{

using overlay::MenuReader;
using overlay::Overlay;
using overlay::OverlayCommand;
using overlay::OverlayHost;
using overlay::OverlaySnapshot;

namespace
{
/// The time between two checks of the settings file.
constexpr auto k_SettingsWatchInterval = std::chrono::seconds(1);
/// The ticks between two lines of the log about the thread of the tick, in the first `k_ThreadLogTicks` ticks.
constexpr std::uint64_t k_ThreadLogInterval = 600;
constexpr std::uint64_t k_ThreadLogTicks = 6000;
/// The ticks between two heartbeat lines when `tickHook` is on.
constexpr std::uint64_t k_HeartbeatInterval = 3600;
/// The visible frames of the menu bar between two new states of the mods for the bar.
constexpr std::uint64_t k_SnapshotInterval = 15;
/// The shortest time that the list of the settings stays in a message.
constexpr double k_SummarySeconds = 6.0;
/// The modules of the game in which a mod can hook a function.
constexpr std::string_view k_GameExe = TS4_MODULE_GAME;
constexpr std::string_view k_SimulationDll = TS4_MODULE_SIMULATION;

[[nodiscard]] std::string Utf8(const std::filesystem::path& path)
{
	const auto text = path.u8string();
	return {text.begin(), text.end()};
}

[[nodiscard]] std::string Now()
{
	const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
	return std::format("{:%Y-%m-%d %H:%M:%S}", now);
}

/// A place for a text that a mod gives.
struct TextBox
{
	std::string text;

	[[nodiscard]] Ts4Text Target()
	{
		return Ts4Text {.context = this, .assign = [](void* context, const char* utf8, size_t size) {
			                if (utf8 != nullptr)
			                {
				                static_cast<TextBox*>(context)->text.assign(utf8, size);
			                }
		                }};
	}
};

/// The value of a field as JSON, in the form of mods.json.
[[nodiscard]] nlohmann::json FieldJson(const FieldEntry& field, const Ts4FieldValue& value)
{
	switch (field.kind)
	{
	case TS4_FIELD_TOGGLE:
		return value.toggle != 0;
	case TS4_FIELD_NUMBER:
		return value.number;
	case TS4_FIELD_CHOICE:
		return value.choice < field.choices.size() ? nlohmann::json(field.choices[value.choice]) : nlohmann::json();
	}
	return {};
}

/// Gives the lines of spdlog (the hook manager, the hotkeys and the menu bar) to the log of the extender.
class LoaderLogSink final: public spdlog::sinks::base_sink<std::mutex>
{
public:
	explicit LoaderLogSink(LoaderLog& log)
	    : _log(log)
	{
	}

protected:
	void sink_it_(const spdlog::details::log_msg& message) override
	{
		const std::string_view text(message.payload.data(), message.payload.size());
		const std::string_view logger(message.logger_name.data(), message.logger_name.size());
		const auto level = message.level >= spdlog::level::err    ? "error: "
		                   : message.level == spdlog::level::warn ? "warning: "
		                                                          : "";
		_log.Write(std::format("{}{}{}", logger.empty() ? std::string() : std::format("{}: ", logger), level, text));
	}
	void flush_() override {}

private:
	LoaderLog& _log;
};

struct LoaderState;

/// A field of the extender in the registry: the state and the field of the table.
struct LoaderField
{
	LoaderState* state = nullptr;
	const settings::Field<LoaderSettings>* field = nullptr;
};

/// A query of the extender itself: the state and the function of the query.
struct LoaderQuery
{
	LoaderState* state = nullptr;
	std::function<nlohmann::json(LoaderState& state, const nlohmann::json& params)> run;
};

/// The threads and the times of the loads of the game modules, from the DLL notification of Windows. The callback runs
/// on the thread that loads the module, with the lock of the Windows loader, so it only writes these values.
struct ModuleLoads
{
	std::atomic<DWORD> simulationThread = 0;
	std::atomic<DWORD> pythonThread = 0;
	std::atomic<ULONGLONG> simulationTime = 0;
	std::atomic<ULONGLONG> pythonTime = 0;
	void* cookie = nullptr;
};

/// The state of the extender. It lives until the process stops.
struct LoaderState final: HostServices
{
	explicit LoaderState(const std::filesystem::path& logPath, ExtenderStart startValues)
	    : log(logPath)
	    , start(std::move(startValues))
	{
	}

	LoaderLog log;
	ExtenderStart start;
	/// The settings that the extender uses now. Only the main thread changes them after the start, with the lock held.
	LoaderSettings settings;
	mutable std::mutex settingsMutex;
	/// The settings that the user asked for last (the file, a hotkey or the menu bar). Main thread only.
	LoaderSettings requested;
	/// The text of mods.json that the extender read last, for the objects of the mods.
	nlohmann::json file = nlohmann::json::object();
	/// `liveReload` is on: the extender monitors the file and reads the hotkeys.
	bool live = false;
	ModRegistry registry;
	std::unique_ptr<ModDllLoader> dlls;
	std::vector<std::filesystem::path> dllFiles;
	std::vector<std::unique_ptr<LoaderField>> loaderFields;
	std::vector<std::unique_ptr<LoaderQuery>> loaderQueries;

	/// The main thread of the game: the thread of the first tick. Zero before the first tick.
	std::atomic<DWORD> mainThread = 0;
	std::unique_ptr<BootstrapHook> bootstrap;
	void* tick = nullptr;
	void* shutdown = nullptr;
	std::unique_ptr<HookManager> hooks;
	/// True after the extender tried each function that the mods hook. Main thread only.
	bool modHooksDone = false;
	/// The callbacks of the mods on the tick function itself. They go on after the bootstrap hook.
	std::vector<FunctionCallbackEntry> tickCallbacks;
	/// The result of the scan of each function that the mods hook, for the status.
	nlohmann::json hookReport = nlohmann::json::array();
	bool tickHandedOver = false;
	std::uint64_t ticks = 0;
	FrameCallbacks frame;
	ModuleLoads loads;
	std::atomic<bool> quitting = false;

	MainThreadTasks tasks;
	std::optional<HotkeyInput> hotkeyInput;
	std::vector<HotkeyBinding> hotkeyBindings;
	nlohmann::json lastReload;
	std::jthread watcher;
	std::shared_ptr<spdlog::logger> overlayLog;
	std::unique_ptr<Overlay> overlay;
	/// True in the tick after the user asked to change the pin of the menu bar.
	bool overlayPin = false;
	std::uint64_t overlayFrames = 0;
	StartupSequence startup;
	StartupMarker marker;
	bool markerWritten = false;
	std::vector<std::string> featuresOff;

	ModRegistry& Registry() override { return registry; }
	void Log(OwnerId owner, Ts4LogLevel level, std::string_view text) override
	{
		const auto who = owner == k_LoaderOwner ? std::string("extender") : registry.OwnerName(owner);
		const auto prefix = level >= TS4_LOG_ERROR ? "error: " : level == TS4_LOG_WARN ? "warning: " : "";
		log.Info("[{}] {}{}", who, prefix, text);
	}
	ts4::Result<void> PostTask(OwnerId owner, Ts4TaskFn task, void* user) override;
	ts4::Result<void> SaveSettings(OwnerId owner, std::string_view patch) override;
	void ShowMessage(OwnerId owner, std::string_view text, double seconds) override;
	ts4::Result<std::string> CallQuery(const QueryEntry& query, const std::string& params) override;
};

/// Stop the calls to a mod after an error in its code.
void FailOwner(LoaderState& state, OwnerId owner, const std::string& reason)
{
	if (owner == k_LoaderOwner || state.registry.Failed(owner))
	{
		return;
	}
	state.registry.SetFailed(owner, reason);
	if (state.dlls)
	{
		state.dlls->MarkFailed(owner, reason);
	}
	state.log.Info("[{}] error: {}; the extender calls the mod no more, the game continues", state.registry.OwnerName(owner),
	               reason);
}

/// Call a function of a mod with a guard. An access violation stops all calls to that mod. Return false if the call
/// did not complete.
template <typename Work>
bool CallMod(LoaderState& state, OwnerId owner, const char* what, Work& work)
{
	if (state.registry.Failed(owner))
	{
		return false;
	}
	const auto thunk = [](void* context) { (*static_cast<Work*>(context))(); };
	if (loader::GuardedCall(thunk, &work, &CatchAccessViolation))
	{
		return true;
	}
	FailOwner(state, owner, std::format("an access violation in {}", what));
	return false;
}

/// Write a change to mods.json. The extender changes only the fields of the change; the other fields of the file stay.
ts4::Result<void> SaveSettingsFile(const LoaderState& state, const nlohmann::json& patch)
{
	const auto& path = state.start.folders.settingsFile;
	auto text = PatchSettingsText(ReadTextFile(path).value_or(""), patch);
	if (!text)
	{
		return text.GetError();
	}
	if (auto checked = ParseLoaderSettings(*text); !checked)
	{
		return ts4::MakeError("the changed settings are not correct: {}", checked.GetError().message);
	}
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	return WriteTextFileAtomically(path, *text);
}

void SaveOrWarn(LoaderState& state, const nlohmann::json& patch)
{
	if (auto saved = SaveSettingsFile(state, patch); !saved)
	{
		state.log.Info("mod settings: warning: {}; the change is only for this game", saved.GetError().message);
	}
}

ts4::Result<void> LoaderState::PostTask(OwnerId owner, Ts4TaskFn task, void* user)
{
	(void)tasks.Post(MainThreadTasks::Task([this, owner, task, user] {
		auto work = [&] { task(user); };
		(void)CallMod(*this, owner, "a task", work);
		return nlohmann::json();
	}));
	return {};
}

ts4::Result<void> LoaderState::SaveSettings(OwnerId owner, std::string_view patch)
{
	const auto object = nlohmann::json::parse(patch, nullptr, false);
	if (object.is_discarded() || !object.is_object())
	{
		return ts4::MakeError("the change is not a JSON object");
	}
	for (const auto& [key, value] : object.items())
	{
		const auto owned = std::ranges::any_of(
		    registry.Settings(), [&](const SettingsEntry& entry) { return entry.owner == owner && entry.section == key; });
		if (!owned)
		{
			return ts4::MakeError("the mod does not own the object '{}' of mods.json", key);
		}
	}
	return SaveSettingsFile(*this, object);
}

void LoaderState::ShowMessage(OwnerId owner, std::string_view text, double /*seconds*/)
{
	// The extender has no messages on the screen of the game yet. The log keeps the message.
	log.Info("[{}] message: {}", owner == k_LoaderOwner ? std::string("extender") : registry.OwnerName(owner), text);
}

ts4::Result<std::string> LoaderState::CallQuery(const QueryEntry& query, const std::string& params)
{
	// The queries read and change the state of the game, so they run only on the main thread.
	if (GetCurrentThreadId() != mainThread.load())
	{
		return ts4::MakeError("the query '{}' runs only on the main thread of the game", query.name);
	}
	if (registry.Failed(query.owner))
	{
		return ts4::MakeError("the extender calls {} no more: {}", registry.OwnerName(query.owner),
		                      registry.FailReason(query.owner));
	}
	ts4::Result<std::string> answer = ts4::MakeError("the query '{}' did not complete", query.name);
	auto work = [&] { answer = RunQueryFunction(query, params); };
	if (!CallMod(*this, query.owner, "a query", work))
	{
		return ts4::MakeError("an access violation in the query '{}'", query.name);
	}
	return answer;
}

/// Tell the user about a change. The extender has no messages on the screen yet, so the log keeps the text.
void Notify(LoaderState& state, const std::string& text)
{
	state.log.Info("message: {}", text);
}

/// Read mods.json. If the file does not exist, the extender writes a file with the defaults. If the file is not
/// correct, the log tells the error and the extender uses the defaults.
void LoadSettingsFile(LoaderState& state)
{
	const auto& path = state.start.folders.settingsFile;
	const auto text = ReadTextFile(path);
	if (!text)
	{
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		const auto defaults = LoaderSettingsJson(LoaderSettings {}).dump(2) + "\n";
		if (auto written = WriteTextFileAtomically(path, defaults); !written)
		{
			state.log.Info("mod settings: no file at {}, and the extender cannot write one: {}; the defaults apply", Utf8(path),
			               written.GetError().message);
			return;
		}
		state.log.Info("mod settings: no file at {}; the extender wrote a file with the defaults", Utf8(path));
		return;
	}
	auto settings = ParseLoaderSettings(*text);
	if (!settings)
	{
		state.log.Info("mod settings: error: {}: {}; the defaults apply", Utf8(path), settings.GetError().message);
		return;
	}
	state.settings = *settings;
	state.requested = *settings;
	state.file = nlohmann::json::parse(*text);
	state.log.Info("mod settings: read {}", Utf8(path));
}

/// Give each mod its objects of mods.json. A mod that refuses its objects keeps the settings that it has.
nlohmann::json DeliverSettings(LoaderState& state)
{
	auto refused = nlohmann::json::array();
	for (const auto& entry : state.registry.Settings())
	{
		const auto section = state.file.contains(entry.section) ? state.file.at(entry.section) : nlohmann::json::object();
		const auto text = section.dump();
		TextBox error;
		uint32_t accepted = 0;
		auto target = error.Target();
		auto work = [&] { accepted = entry.load(entry.user, text.c_str(), &target); };
		if (!CallMod(state, entry.owner, "the settings function", work))
		{
			continue;
		}
		if (accepted == 0)
		{
			state.log.Info("[{}] error: the mod refuses the object '{}' of mods.json: {}; the mod keeps its settings",
			               state.registry.OwnerName(entry.owner), entry.section, error.text);
			refused.push_back({{"section", entry.section}, {"error", error.text}});
		}
	}
	return refused;
}

/// Use the hotkeys of the settings: the defaults of the actions, with the hotkeys of mods.json in their place.
void ApplyHotkeys(LoaderState& state)
{
	std::vector<std::string> errors;
	state.hotkeyBindings = state.registry.ResolveHotkeys(state.settings.hotkeys.keys, errors);
	for (const auto& error : errors)
	{
		state.log.Info("hotkeys: warning: {}", error);
	}
	if (!state.hotkeyInput)
	{
		return;
	}
	std::vector<HotkeyInput::NamedHotkey> named;
	std::string keys;
	for (const auto& binding : state.hotkeyBindings)
	{
		named.push_back({.name = binding.name, .hotkey = binding.hotkey});
		keys += std::format(" {}={}", binding.name, FormatHotkey(binding.hotkey));
	}
	state.hotkeyInput->SetHotkeys(state.settings.hotkeys.enabled, named);
	state.log.Info("hotkeys: {}{}", state.settings.hotkeys.enabled ? "on" : "off", keys);
}

/// Change the pin of the menu bar so that it agrees with `overlay.pinned`.
void ApplyPinSetting(LoaderState& state)
{
	if (state.overlay && state.overlay->Describe().value("pinned", false) != state.settings.overlay.pinned)
	{
		state.overlayPin = true;
	}
}

/// Use new settings of the extender in the running game. Main thread, between two ticks.
nlohmann::json ApplyLoaderSettings(LoaderState& state, const LoaderSettings& next)
{
	const auto change = CompareLoaderSettings(state.settings, next);
	for (const auto& name : change.restartOnly)
	{
		state.log.Info("mod settings: '{}' changed; the change works after a restart of the game", name);
	}
	state.requested = next;
	{
		const std::scoped_lock lock(state.settingsMutex);
		state.settings = LiveLoaderSettings(state.settings, next);
	}
	if (change.hotkeys)
	{
		ApplyHotkeys(state);
	}
	if (change.overlay)
	{
		ApplyPinSetting(state);
	}
	return change.Describe();
}

/// Use the settings that the extender read from the file. A file that is not correct changes nothing: the extender and
/// the mods keep the settings that they have, and the log tells the error.
nlohmann::json ApplyReload(LoaderState& state, const char* source)
{
	nlohmann::json result {{"time", Now()}, {"source", source}};
	const auto& path = state.start.folders.settingsFile;
	const auto text = ReadTextFile(path);
	auto settings = text ? ParseLoaderSettings(*text)
	                     : ts4::Result<LoaderSettings>(ts4::MakeError("the file {} cannot be read", Utf8(path)));
	if (!settings)
	{
		state.log.Info("mod settings: error: {}: {}; the mods keep their current settings", Utf8(path),
		               settings.GetError().message);
		result["applied"] = false;
		result["error"] = settings.GetError().message;
	}
	else
	{
		state.file = nlohmann::json::parse(*text);
		result["loader"] = ApplyLoaderSettings(state, *settings);
		result["refused"] = DeliverSettings(state);
		result["applied"] = true;
		state.log.Info("mod settings: read {} ({}): {}", Utf8(path), source, result.dump());
	}
	state.lastReload = result;
	return result;
}

/// Write the marker of this start with its current stage. If the write fails, the log tells it and the start continues
/// with no protection for the next start.
void WriteStartupMarker(LoaderState& state, std::string stage)
{
	state.marker.stage = std::move(stage);
	const auto& path = state.start.folders.startupMarker;
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	if (auto written = WriteTextFileAtomically(path, FormatStartupMarker(state.marker)); !written)
	{
		state.log.Info("start-up: warning: the extender cannot write the marker {}: {}", Utf8(path),
		               written.GetError().message);
		return;
	}
	state.markerWritten = true;
}

/// Remove the marker of this start: the start completed.
void ClearStartupMarker(LoaderState& state, const std::string& reason)
{
	if (!state.markerWritten)
	{
		return;
	}
	state.markerWritten = false;
	std::error_code error;
	std::filesystem::remove(state.start.folders.startupMarker, error);
	if (error)
	{
		state.log.Info("start-up: warning: the extender cannot remove the marker {}: {}",
		               Utf8(state.start.folders.startupMarker), error.message());
		return;
	}
	state.log.Info("start-up: {}; the extender removed the marker", reason);
}

/// Read the marker of the last start, decide which features this start uses, and write the marker of this start.
/// After a start that did not complete, each feature that it used stays off, also in mods.json, so that the game does
/// not stop at each start: the menu bar, and each DLL mod. The user turns the feature on again.
void BeginStartupGuard(LoaderState& state)
{
	const auto requested = mods::RequestedFeatures(state.settings, state.live, state.dllFiles);
	const auto decision = DecideStartup(ReadTextFile(state.start.folders.startupMarker), requested);
	if (decision.lastStartFailed)
	{
		state.log.Info("start-up: error: THE LAST START OF THE GAME DID NOT COMPLETE. The marker {} is there: the game "
		               "stopped, or the user stopped it, before it was stable.",
		               Utf8(state.start.folders.startupMarker));
		if (const auto& last = decision.lastStart)
		{
			state.log.Info("start-up: the last start: process {}, extender {}, game {}, started {}, last stage '{}', "
			               "features {}",
			               last->processId, last->version, last->gameVersion, last->startedAt, last->stage,
			               nlohmann::json(last->features).dump());
		}
		else
		{
			state.log.Info("start-up: the marker of the last start is not correct: {}", decision.markerError);
		}
	}
	else
	{
		state.log.Info("start-up: no marker of the last start: the last start completed");
	}
	auto turnOff = mods::TurnOffFailedFeatures(state.settings, decision.featuresOff);
	if (!turnOff.patch.is_null())
	{
		state.requested = state.settings;
		SaveOrWarn(state, turnOff.patch);
	}
	for (const auto& note : turnOff.notes)
	{
		state.log.Info("start-up: error: {}", note);
	}
	state.featuresOff = decision.featuresOff;
	state.marker = StartupMarker {.processId = static_cast<uint32_t>(GetCurrentProcessId()),
	                              .version = state.start.version,
	                              .gameVersion = state.start.gameVersion,
	                              .startedAt = Now(),
	                              .stage = {},
	                              .features = decision.featuresOn};
	WriteStartupMarker(state, "start");
	if (state.markerWritten)
	{
		state.log.Info("start-up: the marker {} stays until the game is stable (features {})",
		               Utf8(state.start.folders.startupMarker), nlohmann::json(decision.featuresOn).dump());
	}
}

/// Do an action of the extender or of a mod. Main thread.
void RunAction(LoaderState& state, size_t id)
{
	const auto* action = state.registry.FindAction(id);
	if (action == nullptr)
	{
		return;
	}
	state.log.Info("action: {} ({})", action->hotkey.empty() ? action->label : action->hotkey,
	               state.registry.OwnerName(action->owner));
	auto work = [action] { action->run(action->user); };
	(void)CallMod(state, action->owner, "an action", work);
}

/// Read the hotkeys. Main thread, one time in each tick.
void ReadControls(LoaderState& state)
{
	if (!state.hotkeyInput)
	{
		return;
	}
	for (const auto index : state.hotkeyInput->Poll())
	{
		if (index < state.hotkeyBindings.size())
		{
			RunAction(state, state.hotkeyBindings[index].actionId);
		}
	}
}

/// The DLL mods for the list of the mods and the status.
[[nodiscard]] nlohmann::json DescribeDlls(const LoaderState& state)
{
	return state.dlls ? state.dlls->Describe() : nlohmann::json::array();
}

/// A short text with the state of each DLL mod and the toggles named "enabled".
[[nodiscard]] std::string SettingsSummary(LoaderState& state)
{
	std::string text;
	if (state.dlls)
	{
		for (const auto& mod : state.dlls->Mods())
		{
			text += std::format("{} {}: {}; ", mod->name.empty() ? mod->file : mod->name, mod->version,
			                    ModDllStateName(mod->state));
		}
	}
	for (const auto& field : state.registry.Fields())
	{
		if (field.owner == k_LoaderOwner || field.kind != TS4_FIELD_TOGGLE || field.key != "enabled")
		{
			continue;
		}
		Ts4FieldState value {};
		auto work = [&] { field.get(field.user, &value); };
		if (CallMod(state, field.owner, "a field", work))
		{
			text += std::format("{}: {}; ", field.label, value.value.toggle != 0 ? "on" : "off");
		}
	}
	text += std::format("hotkeys: {}, menu bar: {}", state.settings.hotkeys.enabled ? "on" : "off",
	                    state.overlay ? "ready" : "off");
	return text;
}

/// Register the fields and the actions of the extender itself (owner 0).
void RegisterLoaderItems(LoaderState& state)
{
	state.registry.SetOwnerName(k_LoaderOwner, "Mods");
	for (const auto& field : LoaderFields())
	{
		auto binding = std::make_unique<LoaderField>(LoaderField {.state = &state, .field = &field});
		const auto section = std::string(field.section);
		const auto key = std::string(field.key);
		const auto group = std::string(field.group);
		const auto label = std::string(field.label);
		const auto help = std::string(field.help);
		const auto format = std::string(field.format);
		const Ts4FieldDesc desc {.menu = nullptr,
		                         .group = group.c_str(),
		                         .section = section.c_str(),
		                         .key = key.c_str(),
		                         .label = label.c_str(),
		                         .help = help.c_str(),
		                         .kind = field.kind == settings::FieldKind::Number ? TS4_FIELD_NUMBER : TS4_FIELD_TOGGLE,
		                         .min = field.min,
		                         .max = field.max,
		                         .step = field.step,
		                         .format = format.c_str(),
		                         .logarithmic = field.logarithmic ? 1u : 0u,
		                         .restartOnly = field.restartOnly ? 1u : 0u,
		                         .choices = nullptr,
		                         .choiceCount = 0};
		const auto get = [](void* user, Ts4FieldState* out) {
			const auto& b = *static_cast<LoaderField*>(user);
			const auto value = settings::ValueOf(*b.field, b.state->requested);
			const auto live = settings::ValueOf(*b.field, b.state->settings);
			*out = Ts4FieldState {.value = {}, .min = b.field->min, .max = b.field->max, .restartPending = 0};
			if (const auto* toggle = std::get_if<bool>(&value))
			{
				out->value.toggle = *toggle ? 1u : 0u;
			}
			else if (const auto* number = std::get_if<double>(&value))
			{
				out->value.number = *number;
			}
			out->restartPending = b.field->restartOnly && value != live ? 1u : 0u;
		};
		const auto set = [](void* user, const Ts4FieldValue* value, uint32_t /*save*/, Ts4Text* message) -> uint32_t {
			auto& b = *static_cast<LoaderField*>(user);
			auto next = b.state->requested;
			const settings::FieldValue input = b.field->kind == settings::FieldKind::Number
			                                       ? settings::FieldValue {value->number}
			                                       : settings::FieldValue {value->toggle != 0};
			auto edit = EditLoaderField(next, *b.field, input);
			const auto& text = edit ? edit->message : edit.GetError().message;
			message->assign(message->context, text.data(), text.size());
			if (!edit)
			{
				return 0;
			}
			(void)ApplyLoaderSettings(*b.state, next);
			return 1;
		};
		if (auto added = state.registry.AddField(k_LoaderOwner, desc, get, set, binding.get()); !added)
		{
			state.log.Info("extender: error: the field {} is refused: {}", key, added.GetError().message);
		}
		state.loaderFields.push_back(std::move(binding));
	}
	struct LoaderAction
	{
		const char* hotkey;
		const char* defaultHotkey;
		Ts4TaskFn run;
	};
	// The default hotkeys are the hotkeys of the Masters of Albion mod loader.
	const std::array actions {
	    LoaderAction {"reloadSettings", "Ctrl+Shift+F10",
	                  [](void* user) { (void)ApplyReload(*static_cast<LoaderState*>(user), "hotkey"); }},
	    LoaderAction {"showSettings", "Ctrl+Shift+F11",
	                  [](void* user) {
		                  auto& s = *static_cast<LoaderState*>(user);
		                  Notify(s, SettingsSummary(s));
	                  }},
	    LoaderAction {"toggleOverlay", "Ctrl+Shift+F4",
	                  [](void* user) {
		                  auto& s = *static_cast<LoaderState*>(user);
		                  if (!s.overlay)
		                  {
			                  Notify(s, "Menu bar: not ready (see the log)");
			                  return;
		                  }
		                  s.overlayPin = true;
	                  }},
	};
	for (const auto& action : actions)
	{
		const Ts4ActionDesc desc {.menu = nullptr,
		                          .group = "Hotkeys",
		                          .label = nullptr,
		                          .help = nullptr,
		                          .hotkey = action.hotkey,
		                          .defaultHotkey = action.defaultHotkey};
		if (auto added = state.registry.AddAction(k_LoaderOwner, desc, action.run, nullptr, &state); !added)
		{
			state.log.Info("extender: error: the action {} is refused: {}", action.hotkey, added.GetError().message);
		}
	}
}

/// Start the thread that monitors the settings file. It finds a changed file on its own thread, and the main thread
/// reads and uses it between two ticks.
void StartSettingsWatcher(LoaderState& state)
{
	state.watcher = std::jthread([&state](const std::stop_token& stop) {
		FileChangeMonitor monitor(state.start.folders.settingsFile);
		state.log.Info("mod settings: the extender monitors {} for changes", Utf8(monitor.Path()));
		while (!stop.stop_requested())
		{
			std::this_thread::sleep_for(k_SettingsWatchInterval);
			// No exception may stop the process from this thread.
			try
			{
				if (monitor.Changed())
				{
					(void)state.tasks.Post(MainThreadTasks::Task([&state] { return ApplyReload(state, "file"); }));
				}
			}
			catch (const std::exception& error)
			{
				state.log.Info("mod settings: error: the monitor of the file stops: {}", error.what());
				return;
			}
		}
	});
}

/// Reads the values of the registrations for the menu bar. Each call into a mod has its guard.
class GuardedMenuReader final: public MenuReader
{
public:
	explicit GuardedMenuReader(LoaderState& state)
	    : _state(state)
	{
	}

	bool ReadField(const FieldEntry& field, Ts4FieldState& state) override
	{
		auto work = [&] { field.get(field.user, &state); };
		return CallMod(_state, field.owner, "a field", work);
	}
	bool ActionEnabled(const ActionEntry& action) override
	{
		uint32_t enabled = 0;
		auto work = [&] { enabled = action.enabled(action.user); };
		return CallMod(_state, action.owner, "an action", work) && enabled != 0;
	}
	std::optional<std::string> ReadReadout(const ReadoutEntry& readout) override
	{
		TextBox text;
		auto target = text.Target();
		auto work = [&] { readout.read(readout.user, &target); };
		if (!CallMod(_state, readout.owner, "a readout", work))
		{
			return std::nullopt;
		}
		return text.text;
	}

private:
	LoaderState& _state;
};

/// The status of each DLL mod (its status reader) and of the parts of the extender. Main thread. With `keepNames`, a
/// status keeps its own name and gets the name of the mod as "mod".
[[nodiscard]] nlohmann::json ReadStatuses(LoaderState& state, bool keepNames = false)
{
	auto list = nlohmann::json::array();
	for (const auto& entry : state.registry.Statuses())
	{
		TextBox text;
		auto target = text.Target();
		auto work = [&] { entry.read(entry.user, &target); };
		if (!CallMod(state, entry.owner, "the status reader", work))
		{
			continue;
		}
		auto status = nlohmann::json::parse(text.text, nullptr, false);
		if (status.is_discarded() || !status.is_object())
		{
			status = {{"error", "the status is not a JSON object"}};
		}
		if (keepNames && status.contains("name"))
		{
			status["mod"] = state.registry.OwnerName(entry.owner);
		}
		else
		{
			status["name"] = state.registry.OwnerName(entry.owner);
		}
		list.push_back(std::move(status));
	}
	if (state.dlls)
	{
		for (const auto& mod : state.dlls->Mods())
		{
			if (mod->state == ModDllState::Failed)
			{
				list.push_back({{"name", mod->name.empty() ? mod->file : mod->name}, {"lastError", mod->error}});
			}
		}
	}
	return list;
}

/// The hooks of the extender for the status: the hook manager, the scan of each function and the tick.
[[nodiscard]] nlohmann::json DescribeHooks(const LoaderState& state)
{
	return {
	    {"manager", state.hooks ? state.hooks->Describe() : nlohmann::json()},
	    {"functions", state.hookReport},
	    {"tick",
	     {{"handedOver", state.tickHandedOver},
	      {"ticks", state.ticks},
	      {"mainThread", state.mainThread.load()},
	      {"bootstrapCalls", state.bootstrap ? state.bootstrap->Calls() : 0}}},
	    {"quitHook", state.shutdown != nullptr},
	};
}

/// The state of the mods for the menu bar. Main thread.
[[nodiscard]] OverlaySnapshot MakeOverlaySnapshot(LoaderState& state)
{
	GuardedMenuReader reader(state);
	return OverlaySnapshot {
	    .settings = state.settings,
	    .requested = state.requested,
	    .menus = overlay::BuildMenus(state.registry, state.hotkeyBindings, reader),
	    .mods = ReadStatuses(state),
	    .hooks = DescribeHooks(state),
	    .lastReload = state.lastReload,
	    .settingsPath = Utf8(state.start.folders.settingsFile),
	    .gameVersion = state.start.gameVersion,
	    .version = state.start.version,
	    .live = state.live,
	    .hotkeysReady = state.hotkeyInput.has_value(),
	    .dllList = DescribeDlls(state),
	    .dllFolder = Utf8(mods::ModDllFolder(state.start.folders.settingsFolder)),
	};
}

/// Write a field to mods.json after a change: the value that the owner of the field has now.
void SaveField(LoaderState& state, const FieldEntry& field)
{
	Ts4FieldState current {};
	auto work = [&] { field.get(field.user, &current); };
	if (!CallMod(state, field.owner, "a field", work))
	{
		return;
	}
	nlohmann::json patch;
	if (field.section.empty())
	{
		patch[field.key] = FieldJson(field, current.value);
	}
	else
	{
		patch[field.section][field.key] = FieldJson(field, current.value);
	}
	SaveOrWarn(state, patch);
}

/// Do a change of the menu bar on the main thread, between two ticks.
void ApplyOverlayCommand(LoaderState& state, const OverlayCommand& command)
{
	switch (command.kind)
	{
	case OverlayCommand::Kind::Reload:
		(void)ApplyReload(state, "menu bar");
		break;
	case OverlayCommand::Kind::Action:
		RunAction(state, command.id);
		break;
	case OverlayCommand::Kind::Mod:
	case OverlayCommand::Kind::ModDll:
	{
		auto next = state.requested;
		auto edit = command.kind == OverlayCommand::Kind::Mod ? mods::EditModEnabled(next, command.name, command.on)
		                                                      : mods::EditModDllEnabled(next, command.name, command.on);
		if (!edit)
		{
			state.log.Info("menu bar: warning: {}", edit.GetError().message);
			break;
		}
		(void)ApplyLoaderSettings(state, next);
		state.log.Info("menu bar: {}", edit->message);
		SaveOrWarn(state, edit->patch);
		break;
	}
	case OverlayCommand::Kind::Edit:
	{
		const auto* field = state.registry.FindField(command.id);
		if (field == nullptr)
		{
			break;
		}
		TextBox message;
		auto target = message.Target();
		uint32_t accepted = 0;
		auto work = [&] { accepted = field->set(field->user, &command.value, command.save ? 1u : 0u, &target); };
		if (!CallMod(state, field->owner, "a field", work))
		{
			break;
		}
		if (accepted == 0)
		{
			state.log.Info("menu bar: warning: {}: {}", field->Path(), message.text);
			break;
		}
		if (command.save)
		{
			state.log.Info("menu bar: {}", message.text);
			SaveField(state, *field);
		}
		break;
	}
	}
	if (state.overlay)
	{
		state.overlay->Publish(MakeOverlaySnapshot(state));
	}
}

/// Find a field of the extender or of a mod by its object of mods.json and its key.
[[nodiscard]] const FieldEntry* FindFieldByPath(const LoaderState& state, const std::string& section, const std::string& key)
{
	const auto& fields = state.registry.Fields();
	const auto found =
	    std::ranges::find_if(fields, [&](const FieldEntry& field) { return field.section == section && field.key == key; });
	return found != fields.end() ? &*found : nullptr;
}

/// The value of a JSON parameter for a field of the menu bar.
[[nodiscard]] Ts4FieldValue FieldValueOf(const FieldEntry& field, const nlohmann::json& value)
{
	Ts4FieldValue result {};
	switch (field.kind)
	{
	case TS4_FIELD_TOGGLE:
		if (!value.is_boolean())
		{
			throw std::runtime_error(std::format("'{}' needs true or false", field.Path()));
		}
		result.toggle = value.get<bool>() ? 1u : 0u;
		break;
	case TS4_FIELD_NUMBER:
		if (!value.is_number())
		{
			throw std::runtime_error(std::format("'{}' needs a number", field.Path()));
		}
		result.number = value.get<double>();
		break;
	case TS4_FIELD_CHOICE:
	{
		const auto choice =
		    value.is_string() ? std::ranges::find(field.choices, value.get<std::string>()) : field.choices.end();
		if (choice == field.choices.end())
		{
			throw std::runtime_error(std::format("'{}' needs one of its names", field.Path()));
		}
		result.choice = static_cast<uint32_t>(std::distance(field.choices.begin(), choice));
		break;
	}
	}
	return result;
}

/// The state of the settings file and of the settings that the extender uses now. Main thread.
[[nodiscard]] nlohmann::json DescribeSettings(const LoaderState& state)
{
	// The file can differ from the settings in use: a value that needs a restart, or a file with an error.
	const auto& path = state.start.folders.settingsFile;
	const auto text = ReadTextFile(path);
	const auto file = text ? ParseLoaderSettings(*text)
	                       : ts4::Result<LoaderSettings>(ts4::MakeError("the file {} cannot be read", Utf8(path)));
	nlohmann::json result {
	    {"path", Utf8(path)},
	    {"live", state.live},
	    {"hotkeysReady", state.hotkeyInput.has_value()},
	    {"current", LoaderSettingsJson(state.settings)},
	    {"requested", LoaderSettingsJson(state.requested)},
	    {"lastReload", state.lastReload},
	};
	result["file"] =
	    file ? nlohmann::json {{"correct", true}, {"restartOnly", CompareLoaderSettings(state.settings, *file).restartOnly}}
	         : nlohmann::json {{"correct", false}, {"error", file.GetError().message}};
	return result;
}

/// Change a field or turn a mod on or off, as the menu bar does. Main thread.
[[nodiscard]] nlohmann::json EditAsMenuBar(LoaderState& state, const nlohmann::json& params)
{
	if (params.contains("mod") || params.contains("dll"))
	{
		const bool byId = params.contains("mod");
		const OverlayCommand command {.kind = byId ? OverlayCommand::Kind::Mod : OverlayCommand::Kind::ModDll,
		                              .on = params.at("enabled").get<bool>(),
		                              .name = params.at(byId ? "mod" : "dll").get<std::string>()};
		ApplyOverlayCommand(state, command);
		return {{"posted", byId ? "mods.disabled" : "modDlls.disabled"}};
	}
	const auto section = params.value("section", std::string());
	const auto key = params.at("key").get<std::string>();
	const auto* field = FindFieldByPath(state, section, key);
	if (field == nullptr)
	{
		throw std::runtime_error("the menu bar has no such field");
	}
	const OverlayCommand command {.kind = OverlayCommand::Kind::Edit,
	                              .id = field->id,
	                              .value = FieldValueOf(*field, params.at("value")),
	                              .save = params.value("save", true)};
	ApplyOverlayCommand(state, command);
	Ts4FieldState current {};
	auto work = [&] { field->get(field->user, &current); };
	(void)CallMod(state, field->owner, "a field", work);
	return {{"posted", field->Path()}, {"value", FieldJson(*field, current.value)}};
}

/// Register the queries of the extender itself (owner 0). The extender calls them on the main thread.
void RegisterLoaderQueries(LoaderState& state)
{
	using Run = std::function<nlohmann::json(LoaderState&, const nlohmann::json&)>;
	struct Item
	{
		const char* name;
		const char* help;
		Run run;
	};
	const std::array items {
	    Item {"mods.status", "The state of the extender, of its parts and of each mod.",
	          [](LoaderState& s, const nlohmann::json& /*params*/) {
		          return nlohmann::json {
		              {"version", s.start.version},
		              {"gameVersion", s.start.gameVersion},
		              {"settings", Utf8(s.start.folders.settingsFile)},
		              {"tickHook", s.settings.tickHook},
		              {"live", s.live},
		              {"mods", ReadStatuses(s, true)},
		              {"dlls", DescribeDlls(s)},
		              {"overlay", s.overlay ? s.overlay->Describe() : nlohmann::json()},
		          };
	          }},
	    Item {"mods.list", "The DLL mods, as the list of the mods shows them.",
	          [](LoaderState& s, const nlohmann::json& /*params*/) {
		          return nlohmann::json {{"folder", Utf8(mods::ModDllFolder(s.start.folders.settingsFolder))},
		                                 {"dlls", DescribeDlls(s)}};
	          }},
	    Item {"mods.settings", "The settings file and the settings that the extender uses now.",
	          [](LoaderState& s, const nlohmann::json& /*params*/) { return DescribeSettings(s); }},
	    Item {"mods.reload", "Read mods.json again and give each mod its objects.",
	          [](LoaderState& s, const nlohmann::json& /*params*/) { return ApplyReload(s, "query"); }},
	    Item {"hooks.status", "The hooks of the extender and the count of the tasks that wait.",
	          [](LoaderState& s, const nlohmann::json& /*params*/) {
		          auto status = DescribeHooks(s);
		          status["pendingTasks"] = s.tasks.Pending();
		          return status;
	          }},
	    Item {"overlay.pin", "Pin the menu bar as its hotkey does. Parameter: pinned (true or false).",
	          [](LoaderState& s, const nlohmann::json& params) {
		          if (!s.overlay)
		          {
			          throw std::runtime_error("the menu bar is not installed (see the log)");
		          }
		          const bool pinned = params.value("pinned", true);
		          if (s.overlay->Describe().value("pinned", false) != pinned)
		          {
			          s.overlayPin = true;
		          }
		          return nlohmann::json {{"pinned", pinned}};
	          }},
	    Item {"overlay.edit",
	          "Change a field as the menu bar does: section, key, value and save. Or turn a DLL mod on or off by its id "
	          "(mod) or its file name (dll) with enabled.",
	          [](LoaderState& s, const nlohmann::json& params) { return EditAsMenuBar(s, params); }},
	    Item {"overlay.probe", "Make a new swap chain, as the game does when it changes the display mode.",
	          [](LoaderState& s, const nlohmann::json& /*params*/) {
		          if (!s.overlay || s.overlay->Hooks() == nullptr)
		          {
			          throw std::runtime_error("the menu bar is not installed (see the log)");
		          }
		          if (auto probe = s.overlay->Hooks()->ProbeAgain(); !probe)
		          {
			          throw std::runtime_error(probe.GetError().message);
		          }
		          return nlohmann::json {{"probe", true}};
	          }},
	};
	const auto trampoline = [](void* user, const char* params, Ts4Text* result) -> uint32_t {
		auto& query = *static_cast<LoaderQuery*>(user);
		const auto give = [result](const std::string& text) { result->assign(result->context, text.data(), text.size()); };
		// An exception is the normal error of a query: it goes to the caller.
		try
		{
			auto object = nlohmann::json::parse(params == nullptr ? "{}" : params, nullptr, false);
			if (object.is_null())
			{
				object = nlohmann::json::object();
			}
			if (object.is_discarded() || !object.is_object())
			{
				give("the parameters are not a JSON object");
				return 0;
			}
			give(query.run(*query.state, object).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
			return 1;
		}
		catch (const std::exception& error)
		{
			give(error.what());
		}
		catch (...)
		{
			give("an unknown exception in the query");
		}
		return 0;
	};
	for (const auto& item : items)
	{
		auto query = std::make_unique<LoaderQuery>(LoaderQuery {.state = &state, .run = item.run});
		if (auto added = state.registry.AddQuery(k_LoaderOwner, item.name, item.help, trampoline, query.get()); !added)
		{
			state.log.Info("extender: error: the query {} is refused: {}", item.name, added.GetError().message);
		}
		state.loaderQueries.push_back(std::move(query));
	}
}

/// Show or hide the menu bar for this tick. Before the late start there is no menu bar, and the function does nothing.
void TickOverlay(LoaderState& state)
{
	if (!state.overlay)
	{
		return;
	}
	// The extender does not know yet when the menu of the game is open, so the menu bar shows only when it is pinned.
	const auto change = state.overlay->Tick(false, std::exchange(state.overlayPin, false));
	if (change.shown)
	{
		state.overlayFrames = 0;
		state.log.Info("menu bar: shown");
	}
	if (change.hidden)
	{
		state.log.Info("menu bar: hidden");
	}
	if (change.visible && state.overlayFrames++ % k_SnapshotInterval == 0)
	{
		// An error here must not stop this function: then the bar does not hide when the user unpins it.
		try
		{
			state.overlay->Publish(MakeOverlaySnapshot(state));
		}
		catch (const std::exception& error)
		{
			state.log.Info("menu bar: error: the state of the mods for the bar failed: {}", error.what());
		}
	}
}

/// Install the menu bar. The late start calls it on the main thread. If a step fails, the log tells why and the game
/// continues with no menu bar.
void InstallOverlay(LoaderState& state)
{
	if (!state.settings.overlay.enabled)
	{
		state.log.Info("menu bar: off in the settings");
		return;
	}
	struct Call
	{
		LoaderState* state;
		std::unique_ptr<Overlay> overlay;
		std::string error;
	} call {&state, nullptr, {}};
	const auto work = [](void* context) {
		auto& c = *static_cast<Call*>(context);
		auto& s = *c.state;
		try
		{
			OverlayHost host {
			    .runOnMainThread =
			        [&s](std::function<void()> task) {
				        (void)s.tasks.Post(MainThreadTasks::Task([task = std::move(task)] {
					        task();
					        return nlohmann::json();
				        }));
			        },
			    .apply = [&s](const OverlayCommand& command) { ApplyOverlayCommand(s, command); },
			    .log = s.overlayLog,
			};
			if (auto overlay = Overlay::Install(std::move(host)); overlay)
			{
				c.overlay = std::move(*overlay);
			}
			else
			{
				c.error = overlay.GetError().message;
			}
		}
		catch (const std::exception& error)
		{
			c.error = error.what();
		}
		catch (...)
		{
			c.error = "an unknown exception";
		}
	};
	if (!loader::GuardedCall(work, &call, &CatchAccessViolation))
	{
		// The objects of the install can stay in memory. The extender does not use them.
		(void)call.overlay.release();
		state.log.Info("menu bar: error: an access violation in the install; the menu bar is off");
		return;
	}
	if (!call.overlay)
	{
		state.log.Info("menu bar: error: {}; the menu bar is off", call.error);
		return;
	}
	state.overlay = std::move(call.overlay);
	ApplyPinSetting(state);
	const auto* binding = [&]() -> const HotkeyBinding* {
		const auto found = std::ranges::find(state.hotkeyBindings, std::string("toggleOverlay"), &HotkeyBinding::name);
		return found != state.hotkeyBindings.end() ? &*found : nullptr;
	}();
	state.log.Info("menu bar: ready; it shows when the user pins it with {}{}",
	               binding != nullptr ? FormatHotkey(binding->hotkey) : std::string("its hotkey"),
	               state.settings.overlay.pinned ? " (overlay.pinned is true: pinned now)" : "");
}

/// Do the late steps of the start. The tick calls it on the main thread.
void RunStartupStep(LoaderState& state)
{
	switch (state.startup.OnFrame())
	{
	case StartupStep::None:
		return;
	case StartupStep::LateStart:
		state.log.Info("start-up: the game ran {} ticks; the late start begins", state.startup.Frames());
		WriteStartupMarker(state, "late start");
		if (state.live)
		{
			StartSettingsWatcher(state);
			if (std::ranges::find(state.featuresOff, std::string(k_FeatureOverlay)) != state.featuresOff.end())
			{
				state.log.Info("menu bar: error: the last start did not complete, so the menu bar is off");
			}
			else
			{
				WriteStartupMarker(state, "menu bar");
				InstallOverlay(state);
				WriteStartupMarker(state, "after the menu bar");
			}
		}
		state.log.Info("start-up: the late start is complete");
		return;
	case StartupStep::Stable:
		ClearStartupMarker(state, std::format("the game ran {} ticks and is stable", state.startup.Frames()));
		return;
	}
}

/// The thread of a module load, for the log.
[[nodiscard]] std::string LoadText(const std::atomic<DWORD>& thread, const std::atomic<ULONGLONG>& time)
{
	const auto id = thread.load();
	return id == 0 ? std::string("not loaded yet") : std::format("loaded on thread {} at tick count {}", id, time.load());
}

/// The work of the extender in each tick of the main thread.
void RunTick(LoaderState& state)
{
	if (state.quitting.load())
	{
		return;
	}
	++state.ticks;
	const auto thread = GetCurrentThreadId();
	if ((state.ticks == 1 || state.ticks % k_ThreadLogInterval == 0) && state.ticks <= k_ThreadLogTicks)
	{
		const auto simulation = state.loads.simulationThread.load();
		state.log.Info("tick: call {} on thread {}; Simulation_x64.dll {}{}", state.ticks, thread,
		               LoadText(state.loads.simulationThread, state.loads.simulationTime),
		               simulation == 0        ? ""
		               : simulation == thread ? " (the same thread)"
		                                      : " (A DIFFERENT THREAD)");
	}
	else if (state.settings.tickHook && state.ticks % k_HeartbeatInterval == 0)
	{
		state.log.Info("tick: heartbeat: call {} on thread {}", state.ticks, thread);
	}
	(void)state.frame.Run();
}

/// Add the work of each tick to the frame callbacks, in the order of the work.
void AddFrameWork(LoaderState& state)
{
	if (state.live)
	{
		(void)state.frame.Add("controls", [&state] { ReadControls(state); });
		// The menu bar starts late. Its function is in the list from the start, because a function of the list must
		// not add a function.
		(void)state.frame.Add("overlay", [&state] { TickOverlay(state); });
	}
	(void)state.frame.Add("startup", [&state] { RunStartupStep(state); });
	(void)state.frame.Add("tasks", [&state] { state.tasks.RunPending(); });
}

/// Stop the mods and the menu bar when the game quits. The hook on the shutdown of the application calls it on the
/// main thread, before the services of the game stop.
void OnQuit(LoaderState& state)
{
	if (state.quitting.exchange(true))
	{
		return;
	}
	state.log.Info("the game quits; the extender stops the DLL mods and the menu bar");
	// Each mod puts back the values of the game that it changed.
	if (state.dlls)
	{
		state.dlls->ShutdownAll();
	}
	// The window procedure of the game and the swap chain get their original functions back.
	if (state.overlay)
	{
		state.overlay->Shutdown();
	}
	state.watcher.request_stop();
	ClearStartupMarker(state, "the game quits");
	state.log.Info("the extender stopped");
}

/// Add the callbacks of the mods that are on one function to the hook manager. Return the count of the callbacks that
/// the manager accepted.
size_t AddModCallbacks(LoaderState& state, const void* address, const JoinedHook& hook)
{
	size_t added = 0;
	for (const auto& entry : state.registry.FunctionCallbacks())
	{
		if (state.registry.Failed(entry.owner) ||
		    std::ranges::find(hook.targets, entry.function.key, &FunctionTarget::key) == hook.targets.end())
		{
			continue;
		}
		const auto name = std::format("{}:{}", state.registry.OwnerName(entry.owner), entry.name);
		HookCallback callback {.owner = static_cast<HookOwner>(entry.owner),
		                       .name = name,
		                       .phase = entry.phase == TS4_HOOK_BEFORE ? HookPhase::Before : HookPhase::After,
		                       .parameterCount = hook.parameterCount,
		                       .function = [&state, entry](const Ts4Call& call) {
			                       // The manager stops the callbacks of an owner after a fault in them. This check also
			                       // stops them after a fault in another function of the same mod, and after the
			                       // shutdown of the mods when the game quits.
			                       if (!state.quitting.load() && !state.registry.Failed(entry.owner))
			                       {
				                       entry.call(entry.user, &call);
			                       }
		                       }};
		if (auto result = state.hooks->AddCallback(address, entry.function.name, std::move(callback)); !result)
		{
			state.log.Info("[{}] error: the callback '{}' on {} does not run: {}", state.registry.OwnerName(entry.owner),
			               entry.name, entry.function.key.Text(), result.GetError().message);
			continue;
		}
		++added;
	}
	return added;
}

/// Find each function that the mods hook and put its hook on it. A function whose signature does not match exactly one
/// place gets no hook, and the log tells which callbacks do not run. Main thread, outside the callbacks of the hook
/// manager. Return false if a module of a function is not loaded yet: the next tick tries again.
bool InstallModHooks(LoaderState& state)
{
	const auto functions = state.registry.HookedFunctions();
	const bool needsSimulation = std::ranges::any_of(
	    functions, [](const FunctionTarget& target) { return GameModuleName(target.key.module) == k_SimulationDll; });
	if (needsSimulation && GetModuleHandleA(std::string(k_SimulationDll).c_str()) == nullptr)
	{
		return false;
	}
	std::vector<ResolvedFunction> resolved;
	for (const auto& target : functions)
	{
		const auto moduleName = GameModuleName(target.key.module);
		auto report = nlohmann::json {{"function", target.name}, {"key", target.key.Text()}};
		auto module = ModuleImage::Find(moduleName);
		ts4::Result<const std::byte*> found = ts4::MakeError("the module {} is not loaded", moduleName);
		if (module)
		{
			if (auto decrypted = moduleName == k_GameExe ? CheckDecrypted(*module) : ts4::Result<void> {}; !decrypted)
			{
				found = decrypted.GetError();
			}
			else
			{
				found = FindUniqueSignature(*module, target.key.signature, target.name);
			}
		}
		if (!found)
		{
			report["error"] = found.GetError().message;
			state.log.Info("hooks: error: {}: {}", target.key.Text(), found.GetError().message);
			resolved.push_back({.target = target, .address = 0});
		}
		else
		{
			report["rva"] = std::format("0x{:x}", reinterpret_cast<std::uintptr_t>(*found) - module->Base());
			resolved.push_back({.target = target, .address = reinterpret_cast<std::uintptr_t>(*found)});
		}
		state.hookReport.push_back(std::move(report));
	}
	for (const auto& hook : JoinResolvedFunctions(resolved))
	{
		if (!hook.error.empty())
		{
			state.log.Info("hooks: error: {}; no callback on this function runs", hook.error);
			continue;
		}
		const auto* address = reinterpret_cast<const void*>(hook.address);
		if (address == state.tick)
		{
			// The bootstrap hook is on the tick function now. These callbacks go on when the hook manager takes the tick.
			for (const auto& entry : state.registry.FunctionCallbacks())
			{
				if (std::ranges::find(hook.targets, entry.function.key, &FunctionTarget::key) != hook.targets.end())
				{
					state.tickCallbacks.push_back(entry);
				}
			}
			continue;
		}
		const auto added = AddModCallbacks(state, address, hook);
		state.log.Info("hooks: {} callbacks on {}", added, hook.targets.front().key.Text());
	}
	// Each callback that could not get a hook does not run.
	for (const auto& function : resolved)
	{
		if (function.address != 0)
		{
			continue;
		}
		for (const auto& entry : state.registry.FunctionCallbacks())
		{
			if (entry.function.key == function.target.key)
			{
				state.log.Info("[{}] error: the callback '{}' does not run: no hook on {}",
				               state.registry.OwnerName(entry.owner), entry.name, entry.function.key.Text());
			}
		}
	}
	return true;
}

/// Give the tick to the hook manager after the bootstrap hook removed itself. Main thread.
void HandOverTick(LoaderState& state)
{
	HookCallback frame {.owner = k_ExtenderOwner,
	                    .name = "tick",
	                    .phase = HookPhase::After,
	                    .parameterCount = k_ServiceUpdateParameters,
	                    .function = [&state](const Ts4Call& /*call*/) { RunTick(state); }};
	if (auto added = state.hooks->AddCallback(state.tick, "the update of the client services", std::move(frame)); !added)
	{
		state.log.Info("tick: error: the hook manager cannot take the tick: {}; the hotkeys, the menu bar and the tasks "
		               "stop",
		               added.GetError().message);
		ClearStartupMarker(state, "no tick runs");
		return;
	}
	state.tickHandedOver = true;
	if (!state.tickCallbacks.empty())
	{
		auto target = state.tickCallbacks.front().function;
		const JoinedHook hook {.address = reinterpret_cast<std::uintptr_t>(state.tick),
		                       .parameterCount = target.parameterCount,
		                       .targets = {target},
		                       .error = {}};
		const auto added = AddModCallbacks(state, state.tick, hook);
		state.log.Info("hooks: {} callbacks on the tick function", added);
	}
	state.log.Info("tick: the hook manager runs the tick from now on (after {} ticks of the bootstrap hook)", state.ticks);
}

/// The first ticks, from the bootstrap hook, on the main thread and outside the callbacks of the hook manager. The
/// first tick makes the hook manager. Each tick tries the hooks of the mods until their modules are loaded, and then
/// asks to remove the bootstrap hook. Return true to remove it.
bool OnBootstrapTick(LoaderState& state)
{
	if (state.mainThread.load() == 0)
	{
		state.mainThread = GetCurrentThreadId();
		state.log.Info("tick: the first tick runs on thread {}; this is the main thread of the extender",
		               state.mainThread.load());
		HookManager::Options options;
		options.ownerFailed = [&state](HookOwner owner, const std::string& reason) {
			FailOwner(state, static_cast<OwnerId>(owner), reason);
		};
		if (auto manager = HookManager::Make(std::move(options)); manager)
		{
			state.hooks = std::move(*manager);
		}
		else
		{
			state.log.Info("hooks: error: {}; no hook of a mod runs, and the bootstrap hook keeps the tick",
			               manager.GetError().message);
		}
		if (state.hooks && state.shutdown != nullptr)
		{
			HookCallback quit {.owner = k_ExtenderOwner,
			                   .name = "quit",
			                   .phase = HookPhase::Before,
			                   .parameterCount = 1,
			                   .function = [&state](const Ts4Call& /*call*/) { OnQuit(state); }};
			if (auto added = state.hooks->AddCallback(state.shutdown, "the shutdown of the application", std::move(quit));
			    !added)
			{
				state.log.Info("quit hook: error: {}; the DLL mods do not stop when the game quits", added.GetError().message);
			}
		}
	}
	if (state.hooks && !state.modHooksDone)
	{
		state.modHooksDone = InstallModHooks(state);
	}
	RunTick(state);
	return state.hooks != nullptr && state.modHooksDone && !state.quitting.load();
}

/// Load each DLL mod of the folder of the DLL mods, except the DLLs that the settings or the start-up guard turn off.
void LoadModDlls(LoaderState& state)
{
	const auto folder = mods::ModDllFolder(state.start.folders.settingsFolder);
	HostInfo info {.loaderVersion = state.start.version,
	               .gameVersion = state.start.gameVersion,
	               .gameFolder = Utf8(state.start.gameFolder),
	               .userFolder = Utf8(state.start.userFolder),
	               .settingsFolder = Utf8(state.start.folders.settingsFolder),
	               .logFolder = Utf8(state.start.folders.logFolder)};
	state.dlls = std::make_unique<ModDllLoader>(state, std::move(info), &CatchAccessViolation);
	state.log.Info("mod DLLs: {} in {}", state.dllFiles.size(), Utf8(folder));
	for (const auto& file : state.dllFiles)
	{
		const auto name = Utf8(file.filename());
		if (const auto reason = mods::DisabledByFile(state.settings, name))
		{
			const auto& mod = state.dlls->AddDisabled(file, *reason);
			state.log.Info("mod DLLs:   {} is off ({}), sha256 {}", mod.file, mod.error, mod.sha256);
			continue;
		}
		WriteStartupMarker(state, std::format("mod DLL {}", name));
		const auto& mod = state.dlls->Load(file, state.settings.mods.disabled);
		state.log.Info("mod DLLs:   {} is {}{}, sha256 {}", mod.file, ModDllStateName(mod.state),
		               mod.error.empty() ? std::string() : ": " + mod.error, mod.sha256);
	}
}

/// The DLL notification of Windows (ntdll). Windows calls it on the thread that loads or unloads a DLL.
struct DllNotificationName
{
	USHORT length;
	USHORT maximumLength;
	PCWSTR buffer;
};
struct DllNotificationData
{
	ULONG flags;
	const DllNotificationName* fullDllName;
	const DllNotificationName* baseDllName;
	PVOID dllBase;
	ULONG sizeOfImage;
};
using DllNotificationFunction = VOID(CALLBACK*)(ULONG reason, const DllNotificationData* data, PVOID context);
using RegisterDllNotification = LONG(NTAPI*)(ULONG flags, DllNotificationFunction function, PVOID context, PVOID* cookie);
/// The reason of the notification when a DLL loads.
constexpr ULONG k_DllLoaded = 1;

/// Record the thread that loads `Simulation_x64.dll` and `python37_x64.dll`. The thread that loads the simulation also
/// starts its interpreter of the game Python. The callback runs with the lock of the Windows loader, so it only
/// compares the name and writes atomic values.
VOID CALLBACK OnDllNotification(ULONG reason, const DllNotificationData* data, PVOID context)
{
	if (reason != k_DllLoaded || data == nullptr || data->baseDllName == nullptr || data->baseDllName->buffer == nullptr)
	{
		return;
	}
	auto& loads = *static_cast<ModuleLoads*>(context);
	const auto length = static_cast<int>(data->baseDllName->length / sizeof(wchar_t));
	const auto equal = [&](const wchar_t* name) {
		return CompareStringOrdinal(data->baseDllName->buffer, length, name, -1, TRUE) == CSTR_EQUAL;
	};
	if (equal(L"Simulation_x64.dll"))
	{
		loads.simulationTime = GetTickCount64();
		loads.simulationThread = GetCurrentThreadId();
	}
	else if (equal(L"python37_x64.dll"))
	{
		loads.pythonTime = GetTickCount64();
		loads.pythonThread = GetCurrentThreadId();
	}
}

/// Start the record of the threads that load the game modules. A failure turns off only this record.
void WatchModuleLoads(LoaderState& state)
{
	if (GetModuleHandleW(L"Simulation_x64.dll") != nullptr)
	{
		state.log.Info("modules: Simulation_x64.dll is already loaded; the extender cannot tell its thread");
		return;
	}
	const auto ntdll = GetModuleHandleW(L"ntdll.dll");
	const auto registerFunction = ntdll == nullptr ? nullptr
	                                               : reinterpret_cast<RegisterDllNotification>(reinterpret_cast<void*>(
	                                                     GetProcAddress(ntdll, "LdrRegisterDllNotification")));
	if (registerFunction == nullptr || registerFunction(0, &OnDllNotification, &state.loads, &state.loads.cookie) < 0)
	{
		state.log.Info("modules: warning: the DLL notification of Windows is not available; the log does not tell the "
		               "thread that loads Simulation_x64.dll");
		return;
	}
	state.log.Info("modules: the extender records the thread that loads Simulation_x64.dll");
}

/// Find the tick function and the shutdown of the application in the decrypted program, and put the bootstrap hook on
/// the tick. Return false if the extender has no tick: then no hook, no menu bar and no task runs.
bool InstallBootstrap(LoaderState& state)
{
	auto exe = ModuleImage::Find(k_GameExe);
	if (!exe)
	{
		state.log.Info("tick: error: {}; the extender has no tick", exe.GetError().message);
		return false;
	}
	if (auto decrypted = CheckDecrypted(*exe); !decrypted)
	{
		state.log.Info("tick: error: {}; the extender has no tick", decrypted.GetError().message);
		return false;
	}
	auto tick = FindUniqueSignature(*exe, k_ServiceUpdateSignature, "the update of the client services");
	if (!tick)
	{
		state.log.Info("tick: error: {}; the extender has no tick: no hook, no menu bar and no task runs",
		               tick.GetError().message);
		return false;
	}
	state.tick = const_cast<std::byte*>(*tick);
	state.log.Info("tick: the update of the client services is at {} + 0x{:x}", k_GameExe,
	               reinterpret_cast<std::uintptr_t>(*tick) - exe->Base());
	if (auto shutdown = FindUniqueSignature(*exe, k_AppShutdownSignature, "the shutdown of the application"); shutdown)
	{
		state.shutdown = const_cast<std::byte*>(*shutdown);
		state.log.Info("quit hook: the shutdown of the application is at {} + 0x{:x}", k_GameExe,
		               reinterpret_cast<std::uintptr_t>(*shutdown) - exe->Base());
	}
	else
	{
		state.log.Info("quit hook: error: {}; the DLL mods do not stop when the game quits", shutdown.GetError().message);
	}
	auto bootstrap =
	    BootstrapHook::Install(state.tick, [&state] { return OnBootstrapTick(state); }, [&state] { HandOverTick(state); });
	if (!bootstrap)
	{
		state.log.Info("tick: error: {}; no hook, no menu bar and no task runs", bootstrap.GetError().message);
		return false;
	}
	state.bootstrap = std::move(*bootstrap);
	return true;
}

} // namespace

void StartExtender(LoaderLog& startLog, const ExtenderStart& start)
{
	// The hooks use the state until the process stops, so the extender gives the state to the process.
	auto* state = std::make_unique<LoaderState>(startLog.Path(), start).release();
	auto sink = std::make_shared<LoaderLogSink>(state->log);
	spdlog::set_default_logger(std::make_shared<spdlog::logger>("", sink));
	state->overlayLog = std::make_shared<spdlog::logger>("menu bar", sink);

	LoadSettingsFile(*state);
	state->live = state->settings.liveReload;
	const auto folder = mods::ModDllFolder(start.folders.settingsFolder);
	std::error_code error;
	state->dllFiles =
	    std::filesystem::is_directory(folder, error) ? FindModDlls(folder) : std::vector<std::filesystem::path> {};
	BeginStartupGuard(*state);
	RegisterLoaderItems(*state);
	RegisterLoaderQueries(*state);
	LoadModDlls(*state);
	(void)DeliverSettings(*state);
	if (state->live)
	{
		state->hotkeyInput.emplace();
	}
	ApplyHotkeys(*state);
	AddFrameWork(*state);
	WatchModuleLoads(*state);
	WriteStartupMarker(*state, "hooks");
	if (!InstallBootstrap(*state))
	{
		// No tick counts the frames, so no step of the start can become stable.
		ClearStartupMarker(*state, "no tick runs");
		return;
	}
	state->log.Info("the start thread of the extender is complete; the main thread does the rest at its first tick");
}

} // namespace ts4_mod::loader
