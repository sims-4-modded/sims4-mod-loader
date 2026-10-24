/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <cstddef>

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <common/Result.h>
#include <core/HookRules.h>
#include <mod_api/Ts4ModApi.h>
#include <mod_core/Hotkey.h>

namespace ts4_mod::loader
{

/// The owner of a registration: 0 is the extender, and each DLL mod gets the next number.
using OwnerId = size_t;
inline constexpr OwnerId k_LoaderOwner = 0;

/// A callback before or after a native function.
struct FunctionCallbackEntry
{
	OwnerId owner = 0;
	FunctionTarget function;
	Ts4HookPhase phase = TS4_HOOK_AFTER;
	std::string name;
	Ts4CallFn call = nullptr;
	void* user = nullptr;
};

struct SettingsEntry
{
	OwnerId owner = 0;
	std::string section;
	Ts4SettingsFn load = nullptr;
	void* user = nullptr;
};

struct FieldEntry
{
	OwnerId owner = 0;
	size_t id = 0;
	std::string menu;
	std::string group;
	std::string section;
	std::string key;
	std::string label;
	std::string help;
	Ts4FieldKind kind = TS4_FIELD_TOGGLE;
	double min = 0.0;
	double max = 0.0;
	double step = 0.0;
	std::string format;
	bool logarithmic = false;
	bool restartOnly = false;
	std::vector<std::string> choices;
	Ts4FieldGetFn get = nullptr;
	Ts4FieldSetFn set = nullptr;
	void* user = nullptr;

	/// The path of the field in mods.json, for example "camera.zoomSmoothing".
	[[nodiscard]] std::string Path() const;
};

struct ActionEntry
{
	OwnerId owner = 0;
	size_t id = 0;
	std::string menu;
	std::string group;
	std::string label; ///< Empty: the action has no button.
	std::string help;
	std::string hotkey; ///< Empty: the action has no hotkey.
	Hotkey defaultHotkey;
	Ts4TaskFn run = nullptr;
	Ts4EnabledFn enabled = nullptr;
	void* user = nullptr;
};

struct ReadoutEntry
{
	OwnerId owner = 0;
	size_t id = 0;
	std::string menu;
	std::string group;
	Ts4ReadFn read = nullptr;
	void* user = nullptr;
};

struct StatusEntry
{
	OwnerId owner = 0;
	Ts4ReadFn read = nullptr;
	void* user = nullptr;
};

/// A query of the inspector, for example "sims.needs".
struct QueryEntry
{
	OwnerId owner = 0;
	std::string name;
	std::string help;
	Ts4QueryFn query = nullptr;
	void* user = nullptr;
};

/// The hotkey of an action, after the hotkeys of mods.json replace the defaults.
struct HotkeyBinding
{
	size_t actionId = 0;
	std::string name;
	Hotkey hotkey;
};

/// All that the extender and the mods registered: function callbacks, settings sections, menu fields, actions,
/// readouts, status readers and queries. It checks each registration, so that two mods cannot own the same settings
/// section, the same hotkey name or the same query name. It holds only plain data and C function pointers; the game
/// side installs the hooks and calls the functions. Main thread only, except during the start (one thread).
class ModRegistry
{
public:
	/// Give a name to an owner, for the messages.
	void SetOwnerName(OwnerId owner, std::string name);
	[[nodiscard]] std::string OwnerName(OwnerId owner) const;

	/// Add a callback before or after a native function. All callbacks on one function must get the same number of
	/// parameters, because the function has one hook.
	[[nodiscard]] ts4::Result<void> AddFunctionCallback(OwnerId owner, FunctionTarget function, Ts4HookPhase phase,
	                                                    std::string name, Ts4CallFn call, void* user);
	[[nodiscard]] ts4::Result<void> AddSettings(OwnerId owner, std::string section, Ts4SettingsFn load, void* user);
	[[nodiscard]] ts4::Result<size_t> AddField(OwnerId owner, const Ts4FieldDesc& field, Ts4FieldGetFn get, Ts4FieldSetFn set,
	                                           void* user);
	[[nodiscard]] ts4::Result<size_t> AddAction(OwnerId owner, const Ts4ActionDesc& action, Ts4TaskFn run, Ts4EnabledFn enabled,
	                                            void* user);
	[[nodiscard]] ts4::Result<size_t> AddReadout(OwnerId owner, const char* menu, const char* group, Ts4ReadFn read,
	                                             void* user);
	[[nodiscard]] ts4::Result<void> SetStatusReader(OwnerId owner, Ts4ReadFn read, void* user);
	/// Add a query of the inspector. Each query name has one owner.
	[[nodiscard]] ts4::Result<void> AddQuery(OwnerId owner, std::string name, std::string help, Ts4QueryFn query, void* user);

	/// Remove every registration of an owner, for example after its init failed.
	void RemoveOwner(OwnerId owner);
	/// Stop the calls to an owner, for example after an access violation in its code. The registrations stay, so that
	/// the menu bar can tell it.
	void SetFailed(OwnerId owner, std::string reason);
	[[nodiscard]] bool Failed(OwnerId owner) const;
	[[nodiscard]] std::string FailReason(OwnerId owner) const;

	[[nodiscard]] const std::vector<FunctionCallbackEntry>& FunctionCallbacks() const { return _callbacks; }
	[[nodiscard]] const std::vector<SettingsEntry>& Settings() const { return _settings; }
	[[nodiscard]] const std::vector<FieldEntry>& Fields() const { return _fields; }
	[[nodiscard]] const std::vector<ActionEntry>& Actions() const { return _actions; }
	[[nodiscard]] const std::vector<ReadoutEntry>& Readouts() const { return _readouts; }
	[[nodiscard]] const std::vector<StatusEntry>& Statuses() const { return _statuses; }
	[[nodiscard]] const std::vector<QueryEntry>& Queries() const { return _queries; }

	/// The native functions that have callbacks of an owner.
	[[nodiscard]] std::vector<FunctionKey> FunctionsOf(OwnerId owner) const;
	/// The native functions that have callbacks of any owner, in the order of their first callback. The game side
	/// installs one hook for each.
	[[nodiscard]] std::vector<FunctionTarget> HookedFunctions() const;
	[[nodiscard]] const FieldEntry* FindField(size_t id) const;
	[[nodiscard]] const ActionEntry* FindAction(size_t id) const;
	[[nodiscard]] const ActionEntry* FindActionByHotkey(std::string_view hotkey) const;
	[[nodiscard]] const QueryEntry* FindQuery(std::string_view name) const;

	/// The hotkeys of the actions: the hotkey in `overrides` (the object `hotkeys` of mods.json) or the default. A hotkey
	/// that an earlier action uses already is off for the later action; `errors` gets the reason.
	[[nodiscard]] std::vector<HotkeyBinding> ResolveHotkeys(const std::map<std::string, Hotkey>& overrides,
	                                                        std::vector<std::string>& errors) const;

private:
	[[nodiscard]] bool OwnsSection(OwnerId owner, std::string_view section) const;

	std::map<OwnerId, std::string> _names;
	std::map<OwnerId, std::string> _failed;
	std::vector<FunctionCallbackEntry> _callbacks;
	std::vector<SettingsEntry> _settings;
	std::vector<FieldEntry> _fields;
	std::vector<ActionEntry> _actions;
	std::vector<ReadoutEntry> _readouts;
	std::vector<StatusEntry> _statuses;
	std::vector<QueryEntry> _queries;
	size_t _nextId = 1;
};

} // namespace ts4_mod::loader
