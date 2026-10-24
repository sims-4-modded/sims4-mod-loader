/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <array>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <common/Result.h>
#include <mod_core/Hotkey.h>
#include <mod_core/SettingsFields.h>
#include <nlohmann/json.hpp>

namespace ts4_mod::loader
{

/// The settings of the extender in mods.json. The objects of the mods (for example "camera") belong to the mods: the
/// extender gives each mod its objects and does not read them. A missing field keeps its default.
struct LoaderSettings
{
	/// Write a heartbeat line to the log one time in each 3600 ticks of the main thread. The change works after a
	/// restart of the game.
	bool tickHook = false;

	/// Monitor the settings file and use a changed file while the game runs, and read the hotkeys. The change works
	/// after a restart of the game.
	bool liveReload = true;

	/// The keys that change the mods while the game runs. They work only with `liveReload`.
	struct Hotkeys
	{
		bool enabled = true;         ///< Read the hotkeys in each frame.
		bool messages = true;        ///< Show a short message of the game when a hotkey changes something.
		double messageSeconds = 2.0; ///< The time that the message stays on the screen.
		/// The hotkeys that the file gives, by the name of the action, for example "toggleCamera". An action that is not
		/// in the list uses its default hotkey.
		std::map<std::string, Hotkey> keys;

		friend bool operator==(const Hotkeys&, const Hotkeys&) = default;
	} hotkeys;

	/// The menu bar of the mods in the game. It shows while the menu of the game is open (the Escape key).
	struct Overlay
	{
		bool enabled = true;    ///< Install the menu bar. The change works after a restart of the game.
		double fontScale = 1.0; ///< The size of the text of the menu bar, as a multiple of the default size.

		friend bool operator==(const Overlay&, const Overlay&) = default;
	} overlay;

	/// The DLL mods by the id that each one gives. The values work only after a restart of the game.
	struct Mods
	{
		/// The ids of the mods that the user turned off. The extender reads the id of the DLL, then unloads it before
		/// its init.
		std::vector<std::string> disabled;

		friend bool operator==(const Mods&, const Mods&) = default;
	} mods;

	/// The DLL mods by their file names in the folder of the DLL mods. Each DLL loads, except the DLLs that the user or
	/// the start-up guard turned off. The values work only after a restart of the game.
	struct ModDlls
	{
		/// The file names of the DLLs that do not load, for example "better_needs.dll".
		std::vector<std::string> disabled;

		friend bool operator==(const ModDlls&, const ModDlls&) = default;
	} modDlls;

	friend bool operator==(const LoaderSettings&, const LoaderSettings&) = default;
};

/// The top-level keys of mods.json that belong to the extender.
inline constexpr std::array<std::string_view, 6> k_LoaderKeys {"tickHook", "liveReload", "hotkeys",
                                                               "overlay",  "mods",       "modDlls"};

/// True if the text is an id of a mod: 1 to 64 letters, digits, '-' and '_'.
[[nodiscard]] bool IsModId(std::string_view id);

/// True if the name is a plain file name of a DLL: no folder, and the extension ".dll" in any case.
[[nodiscard]] bool IsDllFileName(std::string_view name);

/// Read the settings of the extender from the JSON text of mods.json. The objects of the mods stay unread. Return an
/// error if the text is not a JSON object, a field has a wrong type, or a value is out of range.
[[nodiscard]] ts4::Result<LoaderSettings> ParseLoaderSettings(std::string_view json);

/// The settings of the extender as JSON, in the form that `ParseLoaderSettings` reads.
[[nodiscard]] nlohmann::json LoaderSettingsJson(const LoaderSettings& settings);

/// Check settings as the file would: write them as JSON and read them again.
[[nodiscard]] ts4::Result<void> CheckLoaderSettings(const LoaderSettings& settings);

/// What new settings change in the extender while the game runs.
struct LoaderSettingsChange
{
	bool hotkeys = false; ///< A value of `hotkeys` changed.
	bool overlay = false; ///< A value of `overlay` changed, other than `enabled`.
	/// The names of the changed fields that the extender uses only after a restart of the game.
	std::vector<std::string> restartOnly;

	[[nodiscard]] bool Any() const { return hotkeys || overlay; }
	[[nodiscard]] nlohmann::json Describe() const;
};

/// Compare the settings that the extender uses now with new settings.
[[nodiscard]] LoaderSettingsChange CompareLoaderSettings(const LoaderSettings& current, const LoaderSettings& next);

/// The settings that the extender can use now: `next`, with the values of `current` for the fields that need a
/// restart.
[[nodiscard]] LoaderSettings LiveLoaderSettings(const LoaderSettings& current, const LoaderSettings& next);

/// The fields of the extender in the menu bar.
[[nodiscard]] std::span<const settings::Field<LoaderSettings>> LoaderFields();

/// Change one field of the extender. Return the change of the settings file and a message.
[[nodiscard]] ts4::Result<settings::SettingsEdit>
EditLoaderField(LoaderSettings& current, const settings::Field<LoaderSettings>& field, const settings::FieldValue& value);

} // namespace ts4_mod::loader
