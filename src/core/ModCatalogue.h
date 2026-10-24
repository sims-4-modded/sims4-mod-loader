/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <cstdint>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <common/Result.h>
#include <core/LoaderSettings.h>
#include <mod_core/SettingsFields.h>
#include <nlohmann/json.hpp>

namespace ts4_mod::mods
{

/// The name of the folder of the DLL mods in the settings folder of the mods (`%LOCALAPPDATA%\Sims4Mods\dlls`). The
/// extender never loads a DLL mod from the game folder.
inline constexpr std::string_view k_DllFolderName = "dlls";

/// The folder of the DLL mods: the folder `dlls` next to `mods.json`, in the settings folder.
[[nodiscard]] std::filesystem::path ModDllFolder(const std::filesystem::path& settingsFolder);

/// The state of one DLL mod in the folder of the DLL mods.
enum class ModDllState : uint8_t
{
	Loaded,   ///< The init of the mod completed.
	Disabled, ///< mods.json or the start-up guard turned it off.
	Refused,  ///< The file, its exports, its interface version, its id or its init was not correct. The DLL is unloaded.
	Failed,   ///< An access violation in the init or later. The calls to the mod stop.
};

[[nodiscard]] std::string_view ModDllStateName(ModDllState state);

/// What the extender knows about one DLL mod, for the list of the mods, the inspector and the log.
struct ModDllRecord
{
	std::string file;   ///< The file name, for example "better_needs.dll".
	std::string sha256; ///< The SHA-256 checksum of the file (64 hex digits), or empty if the file cannot be read.
	ModDllState state = ModDllState::Refused;
	std::string error; ///< Why the mod is off, refused or failed. Empty for a loaded mod.
	std::string id;    ///< The id that `Ts4ModGetInfo` gives, or empty if the extender did not get it.
	std::string name;
	std::string version;
};

/// The DLL mod as JSON: "file", "sha256", "state", "error", "id", "name" and "version".
[[nodiscard]] nlohmann::json DescribeModDll(const ModDllRecord& record);

/// The reason that the settings turn off a DLL by its file name (`modDlls.disabled`), or nothing.
[[nodiscard]] std::optional<std::string> DisabledByFile(const loader::LoaderSettings& settings, std::string_view file);

/// The reason that the settings turn off a mod by its id (`mods.disabled`), or nothing.
[[nodiscard]] std::optional<std::string> DisabledById(const loader::LoaderSettings& settings, std::string_view id);

/// Turn a DLL on or off by its file name in the settings. Return the change of the settings file, as for a field of
/// the menu bar.
[[nodiscard]] ts4::Result<settings::SettingsEdit> EditModDllEnabled(loader::LoaderSettings& settings, std::string_view file,
                                                                    bool enabled);

/// Turn a mod on or off by its id in the settings. Return the change of the settings file.
[[nodiscard]] ts4::Result<settings::SettingsEdit> EditModEnabled(loader::LoaderSettings& settings, std::string_view id,
                                                                 bool enabled);

/// The prefix of the name of a DLL mod in the list of the features of the start-up guard.
inline constexpr std::string_view k_DllFeaturePrefix = "dll:";

/// The name of a DLL mod in the list of the features of the start-up guard, for example "dll:better_needs.dll".
[[nodiscard]] std::string DllFeature(std::string_view file);

/// The file name of the DLL of a feature of the start-up guard, or nothing if the feature is not a DLL.
[[nodiscard]] std::optional<std::string> DllOfFeature(std::string_view feature);

/// The features that a start asks the start-up guard for: the menu bar (with `live` settings and `overlay.enabled`),
/// and each DLL of `dlls` that `modDlls.disabled` does not turn off, in the order of `dlls`.
[[nodiscard]] std::vector<std::string> RequestedFeatures(const loader::LoaderSettings& settings, bool live,
                                                         std::span<const std::filesystem::path> dlls);

/// The features that the start-up guard keeps off after a start that did not complete.
struct StartupTurnOff
{
	bool overlay = false;           ///< The menu bar is off: `overlay.enabled` is false.
	std::vector<std::string> dlls;  ///< The file names of the DLLs that the extender added to `modDlls.disabled`.
	nlohmann::json patch;           ///< The change of mods.json, or null if nothing changed.
	std::vector<std::string> notes; ///< One line for the log for each feature.
};

/// Turn off in `settings` each feature of `featuresOff` (from `DecideStartup`). The extender writes the patch to
/// mods.json, so that the game does not stop at each start. The user turns the feature on again.
[[nodiscard]] StartupTurnOff TurnOffFailedFeatures(loader::LoaderSettings& settings, std::span<const std::string> featuresOff);

} // namespace ts4_mod::mods
