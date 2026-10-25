/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include <core/LoaderSettings.h>

namespace ts4_mod::loader
{

using settings::Field;
using settings::FieldKind;
using settings::ReadField;

namespace
{
/// Read a list of names from the field `disabled` of the object `name`. Each name must pass `valid`. Return the
/// error, if there is one.
template <typename Valid>
std::optional<ts4::Error> ReadDisabled(const nlohmann::json& object, std::string_view name, std::string_view what,
                                       Valid&& valid, std::vector<std::string>& value)
{
	if (!object.is_object())
	{
		return ts4::MakeError("'{}' must be an object", name);
	}
	for (const auto& [key, field] : object.items())
	{
		if (key != "disabled")
		{
			return ts4::MakeError("'{}.{}' is not a setting", name, key);
		}
	}
	const auto disabled = object.find("disabled");
	if (disabled == object.end())
	{
		return std::nullopt;
	}
	if (!disabled->is_array())
	{
		return ts4::MakeError("'{}.disabled' must be a list of {}", name, what);
	}
	value.clear();
	for (const auto& item : *disabled)
	{
		if (!item.is_string() || !valid(item.get<std::string>()))
		{
			return ts4::MakeError("'{}.disabled' must be a list of {}", name, what);
		}
		value.push_back(item.get<std::string>());
	}
	return std::nullopt;
}

/// Read the object `hotkeys`. Each field other than `enabled`, `messages` and `messageSeconds` is the hotkey of an
/// action. Return the error, if there is one.
std::optional<ts4::Error> ReadHotkeys(const nlohmann::json& object, LoaderSettings::Hotkeys& value)
{
	if (!object.is_object() || !ReadField(object, "enabled", value.enabled) || !ReadField(object, "messages", value.messages) ||
	    !ReadField(object, "messageSeconds", value.messageSeconds))
	{
		return ts4::MakeError("the object 'hotkeys' has a field with a wrong type");
	}
	if (value.messageSeconds < 0.5 || value.messageSeconds > 30.0)
	{
		return ts4::MakeError("'messageSeconds' must be 0.5 to 30 seconds");
	}
	for (const auto& [name, field] : object.items())
	{
		if (name == "enabled" || name == "messages" || name == "messageSeconds")
		{
			continue;
		}
		if (!field.is_string())
		{
			return ts4::MakeError("the hotkey '{}' must be a text such as \"Ctrl+Shift+F5\"", name);
		}
		auto hotkey = ParseHotkey(field.get<std::string>());
		if (!hotkey)
		{
			return ts4::MakeError("hotkey '{}': {}", name, hotkey.GetError().message);
		}
		value.keys[name] = std::move(*hotkey);
	}
	return std::nullopt;
}

// The accessors of each field. A lambda with no capture becomes a plain function pointer.
#define TS4_TOGGLE(path) \
	.getToggle = [](const LoaderSettings& s) { return s.path; }, .setToggle = [](LoaderSettings& s, bool v) { s.path = v; }
#define TS4_NUMBER(path) \
	.getNumber = [](const LoaderSettings& s) { return s.path; }, .setNumber = [](LoaderSettings& s, double v) { s.path = v; }

const std::array k_Fields {
    Field<LoaderSettings> {.group = "Hotkeys",
                           .section = "hotkeys",
                           .key = "enabled",
                           .label = "Hotkeys on",
                           .help = "Read the hotkeys of the mods in each frame.",
                           TS4_TOGGLE(hotkeys.enabled)},
    Field<LoaderSettings> {.group = "Hotkeys",
                           .section = "hotkeys",
                           .key = "messages",
                           .label = "Messages",
                           .help = "Show a short message of the game when a hotkey changes something.",
                           TS4_TOGGLE(hotkeys.messages)},
    Field<LoaderSettings> {.group = "Hotkeys",
                           .section = "hotkeys",
                           .key = "messageSeconds",
                           .label = "Message time",
                           .help = "The time that a message stays on the screen.",
                           .kind = FieldKind::Number,
                           .min = 0.5,
                           .max = 30.0,
                           .step = 0.5,
                           .format = "%.1f s",
                           TS4_NUMBER(hotkeys.messageSeconds)},
    Field<LoaderSettings> {.group = "Menu bar",
                           .section = "overlay",
                           .key = "fontScale",
                           .label = "Text size",
                           .help = "The size of the text of this menu bar.",
                           .kind = FieldKind::Number,
                           .min = 0.5,
                           .max = 3.0,
                           .step = 0.05,
                           .format = "%.2f",
                           TS4_NUMBER(overlay.fontScale)},
    Field<LoaderSettings> {.group = "Menu bar",
                           .section = "overlay",
                           .key = "enabled",
                           .label = "Menu bar on",
                           .help = "Install this menu bar when the game starts.",
                           .restartOnly = true,
                           TS4_TOGGLE(overlay.enabled)},
    Field<LoaderSettings> {.group = "Menu bar",
                           .section = "overlay",
                           .key = "pinned",
                           .label = "Always show",
                           .help = "Show this menu bar at all times, as if its hotkey pinned it.",
                           TS4_TOGGLE(overlay.pinned)},
    Field<LoaderSettings> {.group = "Start",
                           .section = "",
                           .key = "liveReload",
                           .label = "Live settings",
                           .help = "Monitor mods.json and read the hotkeys while the game runs.",
                           .restartOnly = true,
                           TS4_TOGGLE(liveReload)},
    Field<LoaderSettings> {.group = "Start",
                           .section = "",
                           .key = "tickHook",
                           .label = "Tick heartbeat",
                           .help = "Write a heartbeat line to the log of the extender.",
                           .restartOnly = true,
                           TS4_TOGGLE(tickHook)},
};

#undef TS4_TOGGLE
#undef TS4_NUMBER
} // namespace

bool IsModId(std::string_view id)
{
	return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
	});
}

bool IsDllFileName(std::string_view name)
{
	if (name.size() < 5 || name.find_first_of("/\\:") != std::string_view::npos || name.starts_with('.'))
	{
		return false;
	}
	auto extension = std::string(name.substr(name.size() - 4));
	for (auto& c : extension)
	{
		c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
	}
	return extension == ".dll";
}

ts4::Result<LoaderSettings> ParseLoaderSettings(std::string_view json)
{
	const auto root = nlohmann::json::parse(json, nullptr, false);
	if (root.is_discarded() || !root.is_object())
	{
		return ts4::MakeError("the mod settings are not a JSON object");
	}
	LoaderSettings settings;
	if (!ReadField(root, "tickHook", settings.tickHook))
	{
		return ts4::MakeError("the field 'tickHook' must be true or false");
	}
	if (!ReadField(root, "liveReload", settings.liveReload))
	{
		return ts4::MakeError("the field 'liveReload' must be true or false");
	}
	if (const auto overlay = root.find("overlay"); overlay != root.end())
	{
		auto& value = settings.overlay;
		if (!overlay->is_object() || !ReadField(*overlay, "enabled", value.enabled) ||
		    !ReadField(*overlay, "fontScale", value.fontScale) || !ReadField(*overlay, "pinned", value.pinned))
		{
			return ts4::MakeError("the object 'overlay' has a field with a wrong type");
		}
		if (value.fontScale < 0.5 || value.fontScale > 3.0)
		{
			return ts4::MakeError("'fontScale' must be 0.5 to 3");
		}
	}
	if (const auto mods = root.find("mods"); mods != root.end())
	{
		if (auto error = ReadDisabled(*mods, "mods", "mod ids", &IsModId, settings.mods.disabled))
		{
			return *error;
		}
	}
	if (const auto dlls = root.find("modDlls"); dlls != root.end())
	{
		if (auto error = ReadDisabled(*dlls, "modDlls", "DLL file names", &IsDllFileName, settings.modDlls.disabled))
		{
			return *error;
		}
	}
	if (const auto hotkeys = root.find("hotkeys"); hotkeys != root.end())
	{
		if (auto error = ReadHotkeys(*hotkeys, settings.hotkeys))
		{
			return *error;
		}
	}
	return settings;
}

nlohmann::json LoaderSettingsJson(const LoaderSettings& settings)
{
	nlohmann::json hotkeys {
	    {"enabled", settings.hotkeys.enabled},
	    {"messages", settings.hotkeys.messages},
	    {"messageSeconds", settings.hotkeys.messageSeconds},
	};
	for (const auto& [name, hotkey] : settings.hotkeys.keys)
	{
		hotkeys[name] = FormatHotkey(hotkey);
	}
	return {
	    {"tickHook", settings.tickHook},
	    {"liveReload", settings.liveReload},
	    {"hotkeys", std::move(hotkeys)},
	    {"overlay",
	     {{"enabled", settings.overlay.enabled},
	      {"fontScale", settings.overlay.fontScale},
	      {"pinned", settings.overlay.pinned}}},
	    {"mods", {{"disabled", settings.mods.disabled}}},
	    {"modDlls", {{"disabled", settings.modDlls.disabled}}},
	};
}

ts4::Result<void> CheckLoaderSettings(const LoaderSettings& settings)
{
	if (auto checked = ParseLoaderSettings(LoaderSettingsJson(settings).dump()); !checked)
	{
		return checked.GetError();
	}
	return {};
}

nlohmann::json LoaderSettingsChange::Describe() const
{
	return {{"hotkeys", hotkeys}, {"overlay", overlay}, {"restartOnly", restartOnly}};
}

LoaderSettingsChange CompareLoaderSettings(const LoaderSettings& current, const LoaderSettings& next)
{
	LoaderSettingsChange change;
	change.hotkeys = current.hotkeys != next.hotkeys;
	change.overlay = current.overlay.fontScale != next.overlay.fontScale || current.overlay.pinned != next.overlay.pinned;
	const std::array<std::pair<const char*, bool>, 5> restart {{
	    {"tickHook", current.tickHook != next.tickHook},
	    {"liveReload", current.liveReload != next.liveReload},
	    {"overlay.enabled", current.overlay.enabled != next.overlay.enabled},
	    {"mods", current.mods != next.mods},
	    {"modDlls", current.modDlls != next.modDlls},
	}};
	for (const auto& [name, changed] : restart)
	{
		if (changed)
		{
			change.restartOnly.emplace_back(name);
		}
	}
	return change;
}

LoaderSettings LiveLoaderSettings(const LoaderSettings& current, const LoaderSettings& next)
{
	auto live = next;
	live.tickHook = current.tickHook;
	live.liveReload = current.liveReload;
	live.overlay.enabled = current.overlay.enabled;
	live.mods = current.mods;
	live.modDlls = current.modDlls;
	return live;
}

std::span<const settings::Field<LoaderSettings>> LoaderFields()
{
	return k_Fields;
}

ts4::Result<settings::SettingsEdit> EditLoaderField(LoaderSettings& current, const settings::Field<LoaderSettings>& field,
                                                    const settings::FieldValue& value)
{
	return settings::EditField(current, field, value, settings::FieldRange {.min = field.min, .max = field.max},
	                           &CheckLoaderSettings);
}

} // namespace ts4_mod::loader
