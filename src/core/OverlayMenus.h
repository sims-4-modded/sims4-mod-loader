/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <cstddef>

#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <core/LoaderSettings.h>
#include <core/ModRegistry.h>
#include <mod_api/Ts4ModApi.h>
#include <nlohmann/json.hpp>

namespace ts4_mod::overlay
{

/// One control or line of a menu, as the main thread read it from the registrations of the extender and the mods.
struct MenuItem
{
	enum class Kind
	{
		Field,   ///< A value of the settings.
		Action,  ///< A button.
		Readout, ///< Lines of text.
	};
	Kind kind = Kind::Field;
	size_t id = 0;
	std::string group {};
	std::string label {};
	std::string help {};
	/// Fields: the path in mods.json, the control, the range and the names of a choice.
	std::string path {};
	Ts4FieldKind fieldKind = TS4_FIELD_TOGGLE;
	std::string format {};
	bool logarithmic = false;
	bool restartOnly = false;
	std::vector<std::string> choices {};
	/// Fields: the value that the user asked for and the range now.
	Ts4FieldState state {};
	/// Actions: the button can be pressed now, and the text of its hotkey.
	bool enabled = true;
	std::string hotkey {};
	/// Readouts: the text.
	std::string text {};
};

/// One menu of the menu bar: the menu of a mod, or a menu that a mod named.
struct Menu
{
	std::string name {};
	std::vector<MenuItem> items {};
};

/// A hotkey of an action, for the list in the menu Mods.
struct HotkeyRow
{
	std::string name {};
	std::string hotkey {};
	std::string owner {};
};

/// The menus that the registrations make.
struct MenuSet
{
	std::vector<Menu> menus {};           ///< The menus of the mods, in the order of the owners.
	std::vector<MenuItem> loaderItems {}; ///< The items of the extender with no menu name, for the menu Mods.
	std::vector<HotkeyRow> hotkeys {};
};

/// Reads the values of the registrations for the menu bar. The extender calls each function of a mod with its guard.
/// The tests give a fake. The main thread calls each function.
class MenuReader
{
public:
	/// Read the value of a field now into `state`, which has the range of the registration. Return false if the call
	/// failed.
	[[nodiscard]] virtual bool ReadField(const loader::FieldEntry& field, Ts4FieldState& state) = 0;
	/// True if the button of the action can be pressed now. The reader calls this function only if the action has an
	/// `enabled` function.
	[[nodiscard]] virtual bool ActionEnabled(const loader::ActionEntry& action) = 0;
	/// The text of a readout, or nothing if the call failed.
	[[nodiscard]] virtual std::optional<std::string> ReadReadout(const loader::ReadoutEntry& readout) = 0;

protected:
	~MenuReader() = default;
};

/// Make the menus from the registrations of the extender and the mods:
/// - Each item goes into the menu that it names, or into the menu with the name of its owner.
/// - The items of the extender with no menu name go to `loaderItems`.
/// - The items stay in the order of the owners, and in the order of the registrations of each owner.
/// - An owner that failed gives no items. An action with no label has no button.
/// - Each action shows its hotkey from `bindings`.
[[nodiscard]] MenuSet BuildMenus(const loader::ModRegistry& registry, std::span<const loader::HotkeyBinding> bindings,
                                 MenuReader& reader);

/// What the menu bar shows of the mods. The main thread makes it and gives it to the menu bar.
struct OverlaySnapshot
{
	loader::LoaderSettings settings {};  ///< The settings that the extender uses now.
	loader::LoaderSettings requested {}; ///< The settings that the user asked for. A field that needs a restart can differ.
	MenuSet menus {};
	nlohmann::json mods {};       ///< The status of each DLL mod and of each part of the extender.
	nlohmann::json hooks {};      ///< The status of the hooks.
	nlohmann::json lastReload {}; ///< The last read of mods.json: "time", "source", "applied" and "error".
	std::string settingsPath {};
	std::string gameVersion {};
	std::string version {}; ///< The version of the extender.
	bool live = false;
	bool hotkeysReady = false;
	/// The DLL mods in the folder of the DLL mods (`mods::DescribeModDll` of each one).
	nlohmann::json dllList {};
	std::string dllFolder {}; ///< The folder of the DLL mods.
};

/// The value of each field of the snapshot, by the id of the field. The controls of the menu bar show these values,
/// except while the user changes a control.
[[nodiscard]] std::map<size_t, Ts4FieldValue> FieldValues(const OverlaySnapshot& snapshot);

/// A change that the user makes in the menu bar. The main thread does it.
struct OverlayCommand
{
	enum class Kind
	{
		Edit,   ///< Change one field (`id`, `value`).
		Reload, ///< Read mods.json again.
		Action, ///< Do an action (`id`).
		Mod,    ///< Turn the DLL mod with the id `name` on or off (`on`).
		ModDll, ///< Turn the DLL with the file name `name` on or off (`on`).
	};
	Kind kind = Kind::Edit;
	size_t id = 0;
	Ts4FieldValue value {};
	bool save = false; ///< Write the change to mods.json. A slider writes only when the user releases it.
	bool on = false;
	std::string name {};
};

} // namespace ts4_mod::overlay
