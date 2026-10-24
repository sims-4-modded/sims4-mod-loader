/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <algorithm>
#include <format>
#include <utility>

#include <core/ModCatalogue.h>
#include <mod_core/StartupGuard.h>

namespace ts4_mod::mods
{

namespace
{
[[nodiscard]] std::string Utf8(const std::filesystem::path& path)
{
	const auto text = path.u8string();
	return {text.begin(), text.end()};
}

[[nodiscard]] bool Contains(const std::vector<std::string>& names, std::string_view name)
{
	return std::ranges::find(names, name) != names.end();
}

/// Put `name` in the list or take it out, and keep the list in the order of the names.
void SetListed(std::vector<std::string>& names, std::string_view name, bool listed)
{
	std::erase(names, name);
	if (listed)
	{
		names.emplace_back(name);
	}
	std::ranges::sort(names);
}
} // namespace

std::filesystem::path ModDllFolder(const std::filesystem::path& settingsFolder)
{
	return settingsFolder / k_DllFolderName;
}

std::string_view ModDllStateName(ModDllState state)
{
	switch (state)
	{
	case ModDllState::Loaded:
		return "loaded";
	case ModDllState::Disabled:
		return "off";
	case ModDllState::Refused:
		return "refused";
	case ModDllState::Failed:
		return "failed";
	}
	return "unknown";
}

nlohmann::json DescribeModDll(const ModDllRecord& record)
{
	return {
	    {"file", record.file},       {"sha256", record.sha256}, {"state", ModDllStateName(record.state)},
	    {"error", record.error},     {"id", record.id},         {"name", record.name},
	    {"version", record.version},
	};
}

std::optional<std::string> DisabledByFile(const loader::LoaderSettings& settings, std::string_view file)
{
	return Contains(settings.modDlls.disabled, file) ? std::optional<std::string>("modDlls.disabled in mods.json")
	                                                 : std::nullopt;
}

std::optional<std::string> DisabledById(const loader::LoaderSettings& settings, std::string_view id)
{
	return Contains(settings.mods.disabled, id) ? std::optional<std::string>("mods.disabled in mods.json") : std::nullopt;
}

ts4::Result<settings::SettingsEdit> EditModDllEnabled(loader::LoaderSettings& settings, std::string_view file, bool enabled)
{
	if (!loader::IsDllFileName(file))
	{
		return ts4::MakeError("'{}' is not a file name of a DLL", file);
	}
	auto next = settings;
	SetListed(next.modDlls.disabled, file, !enabled);
	if (auto checked = loader::CheckLoaderSettings(next); !checked)
	{
		return checked.GetError();
	}
	nlohmann::json patch;
	patch["modDlls"]["disabled"] = next.modDlls.disabled;
	settings = std::move(next);
	return settings::SettingsEdit {.message =
	                                   std::format("DLL mod {}: {} after a restart of the game", file, enabled ? "on" : "off"),
	                               .patch = std::move(patch)};
}

ts4::Result<settings::SettingsEdit> EditModEnabled(loader::LoaderSettings& settings, std::string_view id, bool enabled)
{
	if (!loader::IsModId(id))
	{
		return ts4::MakeError("'{}' is not a mod id", id);
	}
	auto next = settings;
	SetListed(next.mods.disabled, id, !enabled);
	if (auto checked = loader::CheckLoaderSettings(next); !checked)
	{
		return checked.GetError();
	}
	nlohmann::json patch;
	patch["mods"]["disabled"] = next.mods.disabled;
	settings = std::move(next);
	return settings::SettingsEdit {.message = std::format("Mod {}: {} after a restart of the game", id, enabled ? "on" : "off"),
	                               .patch = std::move(patch)};
}

std::string DllFeature(std::string_view file)
{
	return std::format("{}{}", k_DllFeaturePrefix, file);
}

std::optional<std::string> DllOfFeature(std::string_view feature)
{
	if (!feature.starts_with(k_DllFeaturePrefix) || feature.size() == k_DllFeaturePrefix.size())
	{
		return std::nullopt;
	}
	return std::string(feature.substr(k_DllFeaturePrefix.size()));
}

std::vector<std::string> RequestedFeatures(const loader::LoaderSettings& settings, bool live,
                                           std::span<const std::filesystem::path> dlls)
{
	std::vector<std::string> requested;
	if (live && settings.overlay.enabled)
	{
		requested.emplace_back(k_FeatureOverlay);
	}
	for (const auto& dll : dlls)
	{
		const auto file = Utf8(dll.filename());
		if (!DisabledByFile(settings, file))
		{
			requested.push_back(DllFeature(file));
		}
	}
	return requested;
}

StartupTurnOff TurnOffFailedFeatures(loader::LoaderSettings& settings, std::span<const std::string> featuresOff)
{
	StartupTurnOff off;
	for (const auto& feature : featuresOff)
	{
		if (feature == k_FeatureOverlay)
		{
			settings.overlay.enabled = false;
			off.overlay = true;
			off.patch["overlay"]["enabled"] = false;
			off.notes.emplace_back("the menu bar is off: the last start used it and did not complete. The extender wrote "
			                       "overlay.enabled false to mods.json. Set it to true again to try the menu bar.");
		}
		else if (auto file = DllOfFeature(feature); file && loader::IsDllFileName(*file))
		{
			if (!Contains(settings.modDlls.disabled, *file))
			{
				settings.modDlls.disabled.push_back(*file);
			}
			off.patch["modDlls"]["disabled"] = settings.modDlls.disabled;
			off.notes.push_back(std::format("the DLL mod {} is off: the last start used it and did not complete. The "
			                                "extender added it to modDlls.disabled in mods.json. Remove it from that list to "
			                                "try it again.",
			                                *file));
			off.dlls.push_back(std::move(*file));
		}
	}
	return off;
}

} // namespace ts4_mod::mods
