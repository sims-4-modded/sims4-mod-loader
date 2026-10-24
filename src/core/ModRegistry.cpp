/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <algorithm>
#include <format>
#include <utility>

#include <core/LoaderSettings.h>
#include <core/ModRegistry.h>

namespace ts4_mod::loader
{

namespace
{
[[nodiscard]] std::string Copy(const char* text)
{
	return text == nullptr ? std::string() : std::string(text);
}

/// A name for mods.json: letters, digits and '_', with a letter first.
[[nodiscard]] bool IsKeyName(std::string_view name)
{
	if (name.empty() || name.size() > 64 || !((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z')))
	{
		return false;
	}
	return std::ranges::all_of(
	    name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; });
}

/// A name of a query: letters, digits, '.', '_' and '-', with a letter first, for example "sims.needs".
[[nodiscard]] bool IsQueryName(std::string_view name)
{
	if (name.empty() || name.size() > 64 || !((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z')))
	{
		return false;
	}
	return std::ranges::all_of(name, [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
	});
}

[[nodiscard]] bool IsLoaderKey(std::string_view section)
{
	return section.empty() || std::ranges::find(k_LoaderKeys, section) != k_LoaderKeys.end();
}

template <typename Entries>
void EraseOwner(Entries& entries, OwnerId owner)
{
	std::erase_if(entries, [owner](const auto& entry) { return entry.owner == owner; });
}
} // namespace

std::string FieldEntry::Path() const
{
	return section.empty() ? key : std::format("{}.{}", section, key);
}

void ModRegistry::SetOwnerName(OwnerId owner, std::string name)
{
	_names[owner] = std::move(name);
}

std::string ModRegistry::OwnerName(OwnerId owner) const
{
	const auto found = _names.find(owner);
	return found != _names.end() ? found->second : std::format("owner {}", owner);
}

bool ModRegistry::OwnsSection(OwnerId owner, std::string_view section) const
{
	if (owner == k_LoaderOwner)
	{
		return IsLoaderKey(section);
	}
	return std::ranges::any_of(_settings,
	                           [&](const SettingsEntry& entry) { return entry.owner == owner && entry.section == section; });
}

ts4::Result<void> ModRegistry::AddFunctionCallback(OwnerId owner, FunctionTarget function, Ts4HookPhase phase, std::string name,
                                                   Ts4CallFn call, void* user)
{
	if (call == nullptr || name.empty() || function.key.module.empty() || function.key.signature.empty() ||
	    function.name.empty())
	{
		return ts4::MakeError("a function callback needs a module, a signature, a function name, a name and a function");
	}
	if (phase != TS4_HOOK_BEFORE && phase != TS4_HOOK_AFTER)
	{
		return ts4::MakeError("the callback '{}' has an unknown phase", name);
	}
	if (function.parameterCount > TS4_CALL_MAX_ARGUMENTS)
	{
		return ts4::MakeError("the function '{}' has {} parameters; a callback gets at most {}", function.name,
		                      function.parameterCount, TS4_CALL_MAX_ARGUMENTS);
	}
	const auto same = [&](const FunctionCallbackEntry& entry) {
		return entry.owner == owner && entry.function.key == function.key && entry.phase == phase && entry.name == name;
	};
	if (std::ranges::any_of(_callbacks, same))
	{
		return ts4::MakeError("the callback '{}' on {} exists", name, function.key.Text());
	}
	// One function has one hook. All callbacks on it must agree on its parameters.
	for (const auto& entry : _callbacks)
	{
		if (auto agreed = CheckSameFunction(entry.function, function, OwnerName(entry.owner)); !agreed)
		{
			return agreed.GetError();
		}
	}
	_callbacks.push_back(FunctionCallbackEntry {
	    .owner = owner, .function = std::move(function), .phase = phase, .name = std::move(name), .call = call, .user = user});
	return {};
}

ts4::Result<void> ModRegistry::AddSettings(OwnerId owner, std::string section, Ts4SettingsFn load, void* user)
{
	if (load == nullptr || !IsKeyName(section))
	{
		return ts4::MakeError("the settings section '{}' needs a name of letters, digits and '_', and a function", section);
	}
	if (IsLoaderKey(section))
	{
		return ts4::MakeError("the settings section '{}' belongs to the extender", section);
	}
	const auto existing = std::ranges::find(_settings, section, &SettingsEntry::section);
	if (existing != _settings.end())
	{
		return ts4::MakeError("the settings section '{}' belongs to {}", section, OwnerName(existing->owner));
	}
	_settings.push_back(SettingsEntry {.owner = owner, .section = std::move(section), .load = load, .user = user});
	return {};
}

ts4::Result<size_t> ModRegistry::AddField(OwnerId owner, const Ts4FieldDesc& field, Ts4FieldGetFn get, Ts4FieldSetFn set,
                                          void* user)
{
	FieldEntry entry {.owner = owner,
	                  .id = 0,
	                  .menu = Copy(field.menu),
	                  .group = Copy(field.group),
	                  .section = Copy(field.section),
	                  .key = Copy(field.key),
	                  .label = Copy(field.label),
	                  .help = Copy(field.help),
	                  .kind = field.kind,
	                  .min = field.min,
	                  .max = field.max,
	                  .step = field.step,
	                  .format = Copy(field.format),
	                  .logarithmic = field.logarithmic != 0,
	                  .restartOnly = field.restartOnly != 0,
	                  .choices = {},
	                  .get = get,
	                  .set = set,
	                  .user = user};
	if (get == nullptr || set == nullptr || entry.key.empty() || entry.label.empty())
	{
		return ts4::MakeError("a field needs a key, a label and two functions");
	}
	if (!OwnsSection(owner, entry.section))
	{
		return ts4::MakeError("the field '{}' is in the section '{}', which {} does not own", entry.Path(), entry.section,
		                      OwnerName(owner));
	}
	if (std::ranges::any_of(_fields, [&](const FieldEntry& f) { return f.section == entry.section && f.key == entry.key; }))
	{
		return ts4::MakeError("the field '{}' exists", entry.Path());
	}
	switch (field.kind)
	{
	case TS4_FIELD_TOGGLE:
		break;
	case TS4_FIELD_NUMBER:
		if (!(field.min <= field.max))
		{
			return ts4::MakeError("the field '{}' has a minimum above its maximum", entry.Path());
		}
		break;
	case TS4_FIELD_CHOICE:
		if (field.choices == nullptr || field.choiceCount == 0)
		{
			return ts4::MakeError("the choice field '{}' has no names", entry.Path());
		}
		for (uint32_t i = 0; i < field.choiceCount; ++i)
		{
			entry.choices.push_back(Copy(field.choices[i]));
		}
		break;
	default:
		return ts4::MakeError("the field '{}' has an unknown kind", entry.Path());
	}
	entry.id = _nextId++;
	_fields.push_back(std::move(entry));
	return _fields.back().id;
}

ts4::Result<size_t> ModRegistry::AddAction(OwnerId owner, const Ts4ActionDesc& action, Ts4TaskFn run, Ts4EnabledFn enabled,
                                           void* user)
{
	ActionEntry entry {.owner = owner,
	                   .id = 0,
	                   .menu = Copy(action.menu),
	                   .group = Copy(action.group),
	                   .label = Copy(action.label),
	                   .help = Copy(action.help),
	                   .hotkey = Copy(action.hotkey),
	                   .defaultHotkey = {},
	                   .run = run,
	                   .enabled = enabled,
	                   .user = user};
	if (run == nullptr || (entry.label.empty() && entry.hotkey.empty()))
	{
		return ts4::MakeError("an action needs a function and a label or a hotkey");
	}
	if (!entry.hotkey.empty())
	{
		if (!IsKeyName(entry.hotkey) || entry.hotkey == "enabled" || entry.hotkey == "messages" ||
		    entry.hotkey == "messageSeconds")
		{
			return ts4::MakeError("the hotkey name '{}' is not permitted", entry.hotkey);
		}
		if (const auto* other = FindActionByHotkey(entry.hotkey); other != nullptr)
		{
			return ts4::MakeError("the hotkey name '{}' belongs to {}", entry.hotkey, OwnerName(other->owner));
		}
		if (action.defaultHotkey != nullptr)
		{
			auto hotkey = ParseHotkey(action.defaultHotkey);
			if (!hotkey)
			{
				return ts4::MakeError("the default of the hotkey '{}': {}", entry.hotkey, hotkey.GetError().message);
			}
			entry.defaultHotkey = std::move(*hotkey);
		}
	}
	entry.id = _nextId++;
	_actions.push_back(std::move(entry));
	return _actions.back().id;
}

ts4::Result<size_t> ModRegistry::AddReadout(OwnerId owner, const char* menu, const char* group, Ts4ReadFn read, void* user)
{
	if (read == nullptr)
	{
		return ts4::MakeError("a readout needs a function");
	}
	_readouts.push_back(
	    ReadoutEntry {.owner = owner, .id = _nextId++, .menu = Copy(menu), .group = Copy(group), .read = read, .user = user});
	return _readouts.back().id;
}

ts4::Result<void> ModRegistry::SetStatusReader(OwnerId owner, Ts4ReadFn read, void* user)
{
	if (read == nullptr)
	{
		return ts4::MakeError("a status reader needs a function");
	}
	EraseOwner(_statuses, owner);
	_statuses.push_back(StatusEntry {.owner = owner, .read = read, .user = user});
	return {};
}

ts4::Result<void> ModRegistry::AddQuery(OwnerId owner, std::string name, std::string help, Ts4QueryFn query, void* user)
{
	if (query == nullptr || !IsQueryName(name))
	{
		return ts4::MakeError("the query '{}' needs a name of letters, digits, '.', '_' and '-', and a function", name);
	}
	if (const auto* existing = FindQuery(name); existing != nullptr)
	{
		return ts4::MakeError("the query '{}' belongs to {}", name, OwnerName(existing->owner));
	}
	_queries.push_back(
	    QueryEntry {.owner = owner, .name = std::move(name), .help = std::move(help), .query = query, .user = user});
	return {};
}

const QueryEntry* ModRegistry::FindQuery(std::string_view name) const
{
	const auto found = std::ranges::find(_queries, name, &QueryEntry::name);
	return found != _queries.end() ? &*found : nullptr;
}

void ModRegistry::RemoveOwner(OwnerId owner)
{
	EraseOwner(_callbacks, owner);
	EraseOwner(_settings, owner);
	EraseOwner(_fields, owner);
	EraseOwner(_actions, owner);
	EraseOwner(_readouts, owner);
	EraseOwner(_statuses, owner);
	EraseOwner(_queries, owner);
}

void ModRegistry::SetFailed(OwnerId owner, std::string reason)
{
	_failed.emplace(owner, std::move(reason));
}

bool ModRegistry::Failed(OwnerId owner) const
{
	return _failed.contains(owner);
}

std::string ModRegistry::FailReason(OwnerId owner) const
{
	const auto found = _failed.find(owner);
	return found != _failed.end() ? found->second : std::string();
}

std::vector<FunctionKey> ModRegistry::FunctionsOf(OwnerId owner) const
{
	std::vector<FunctionKey> functions;
	for (const auto& entry : _callbacks)
	{
		if (entry.owner == owner && std::ranges::find(functions, entry.function.key) == functions.end())
		{
			functions.push_back(entry.function.key);
		}
	}
	return functions;
}

std::vector<FunctionTarget> ModRegistry::HookedFunctions() const
{
	std::vector<FunctionTarget> functions;
	for (const auto& entry : _callbacks)
	{
		const auto found = std::ranges::find(functions, entry.function.key, &FunctionTarget::key);
		if (found == functions.end())
		{
			functions.push_back(entry.function);
		}
	}
	return functions;
}

const FieldEntry* ModRegistry::FindField(size_t id) const
{
	const auto found = std::ranges::find(_fields, id, &FieldEntry::id);
	return found != _fields.end() ? &*found : nullptr;
}

const ActionEntry* ModRegistry::FindAction(size_t id) const
{
	const auto found = std::ranges::find(_actions, id, &ActionEntry::id);
	return found != _actions.end() ? &*found : nullptr;
}

const ActionEntry* ModRegistry::FindActionByHotkey(std::string_view hotkey) const
{
	const auto found = std::ranges::find(_actions, hotkey, &ActionEntry::hotkey);
	return found != _actions.end() ? &*found : nullptr;
}

std::vector<HotkeyBinding> ModRegistry::ResolveHotkeys(const std::map<std::string, Hotkey>& overrides,
                                                       std::vector<std::string>& errors) const
{
	std::vector<HotkeyBinding> bindings;
	for (const auto& action : _actions)
	{
		if (action.hotkey.empty())
		{
			continue;
		}
		const auto found = overrides.find(action.hotkey);
		auto hotkey = found != overrides.end() ? found->second : action.defaultHotkey;
		if (hotkey.Enabled())
		{
			const auto same = std::ranges::find(bindings, hotkey, &HotkeyBinding::hotkey);
			if (same != bindings.end())
			{
				errors.push_back(std::format("the hotkeys '{}' and '{}' use the same keys ({}); '{}' is off", same->name,
				                             action.hotkey, FormatHotkey(hotkey), action.hotkey));
				hotkey = {};
			}
		}
		bindings.push_back(HotkeyBinding {.actionId = action.id, .name = action.hotkey, .hotkey = std::move(hotkey)});
	}
	for (const auto& [name, hotkey] : overrides)
	{
		if (FindActionByHotkey(name) == nullptr)
		{
			errors.push_back(std::format("mods.json has the hotkey '{}', but no mod has that action", name));
		}
	}
	return bindings;
}

} // namespace ts4_mod::loader
