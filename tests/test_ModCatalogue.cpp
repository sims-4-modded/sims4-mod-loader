/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <filesystem>
#include <string>
#include <vector>

#include <core/LoaderSettings.h>
#include <core/ModCatalogue.h>
#include <gtest/gtest.h>
#include <mod_core/StartupGuard.h>
#include <nlohmann/json.hpp>

using namespace ts4_mod;
using namespace ts4_mod::mods;
using loader::LoaderSettings;

TEST(ModCatalogue, TheDllModsAreInTheSettingsFolderOutsideTheGameFolder)
{
	const std::filesystem::path settings = "C:/Users/a/AppData/Local/Sims4Mods";
	EXPECT_EQ(ModDllFolder(settings), settings / "dlls");
	EXPECT_EQ(k_DllFolderName, "dlls");
}

TEST(ModCatalogue, DescribesADllModWithItsChecksum)
{
	const ModDllRecord record {.file = "better_needs.dll",
	                           .sha256 = std::string(64, 'a'),
	                           .state = ModDllState::Failed,
	                           .error = "an access violation in Ts4ModInit",
	                           .id = "better_needs",
	                           .name = "Better needs",
	                           .version = "1.2"};
	const auto json = DescribeModDll(record);
	EXPECT_EQ(json, nlohmann::json({{"file", "better_needs.dll"},
	                                {"sha256", std::string(64, 'a')},
	                                {"state", "failed"},
	                                {"error", "an access violation in Ts4ModInit"},
	                                {"id", "better_needs"},
	                                {"name", "Better needs"},
	                                {"version", "1.2"}}));
	EXPECT_EQ(ModDllStateName(ModDllState::Loaded), "loaded");
	EXPECT_EQ(ModDllStateName(ModDllState::Disabled), "off");
	EXPECT_EQ(ModDllStateName(ModDllState::Refused), "refused");
}

TEST(ModCatalogue, TellsWhyTheSettingsTurnADllOff)
{
	LoaderSettings settings;
	settings.modDlls.disabled = {"a.dll"};
	settings.mods.disabled = {"needs"};
	EXPECT_EQ(DisabledByFile(settings, "a.dll"), "modDlls.disabled in mods.json");
	EXPECT_FALSE(DisabledByFile(settings, "b.dll"));
	EXPECT_EQ(DisabledById(settings, "needs"), "mods.disabled in mods.json");
	EXPECT_FALSE(DisabledById(settings, "a"));
}

TEST(ModCatalogue, TurnsADllOnAndOffWithAPatchOfTheList)
{
	LoaderSettings settings;
	settings.modDlls.disabled = {"z.dll"};
	auto edit = EditModDllEnabled(settings, "a.dll", false);
	ASSERT_TRUE(edit) << edit.GetError().message;
	EXPECT_EQ(settings.modDlls.disabled, (std::vector<std::string> {"a.dll", "z.dll"}));
	EXPECT_EQ(edit->patch, nlohmann::json({{"modDlls", {{"disabled", {"a.dll", "z.dll"}}}}}));
	EXPECT_EQ(edit->message, "DLL mod a.dll: off after a restart of the game");
	// Two edits do not add the name two times.
	ASSERT_TRUE(EditModDllEnabled(settings, "a.dll", false));
	EXPECT_EQ(settings.modDlls.disabled.size(), 2u);
	edit = EditModDllEnabled(settings, "a.dll", true);
	ASSERT_TRUE(edit);
	EXPECT_EQ(settings.modDlls.disabled, std::vector<std::string> {"z.dll"});
	EXPECT_FALSE(EditModDllEnabled(settings, "../a.dll", false));
	EXPECT_FALSE(EditModDllEnabled(settings, "a.txt", false));
	EXPECT_EQ(settings.modDlls.disabled, std::vector<std::string> {"z.dll"});
}

TEST(ModCatalogue, TurnsAModOnAndOffByItsId)
{
	LoaderSettings settings;
	const auto edit = EditModEnabled(settings, "better_needs", false);
	ASSERT_TRUE(edit) << edit.GetError().message;
	EXPECT_EQ(settings.mods.disabled, std::vector<std::string> {"better_needs"});
	EXPECT_EQ(edit->patch, nlohmann::json({{"mods", {{"disabled", {"better_needs"}}}}}));
	ASSERT_TRUE(EditModEnabled(settings, "better_needs", true));
	EXPECT_TRUE(settings.mods.disabled.empty());
	EXPECT_FALSE(EditModEnabled(settings, "bad id!", false));
}

TEST(ModCatalogue, NamesEachDllAsAFeatureOfTheStartupGuard)
{
	EXPECT_EQ(DllFeature("a.dll"), "dll:a.dll");
	EXPECT_EQ(DllOfFeature("dll:a.dll"), "a.dll");
	EXPECT_FALSE(DllOfFeature("dll:"));
	EXPECT_FALSE(DllOfFeature(k_FeatureOverlay));
}

TEST(ModCatalogue, AsksTheStartupGuardForTheMenuBarAndEachDllThatIsOn)
{
	LoaderSettings settings;
	settings.modDlls.disabled = {"b.dll"};
	const std::vector<std::filesystem::path> dlls {"x/a.dll", "x/b.dll", "x/c.dll"};
	EXPECT_EQ(RequestedFeatures(settings, true, dlls),
	          (std::vector<std::string> {std::string(k_FeatureOverlay), "dll:a.dll", "dll:c.dll"}));
	// With no live settings, or with the menu bar off, the start does not use the menu bar.
	EXPECT_EQ(RequestedFeatures(settings, false, dlls), (std::vector<std::string> {"dll:a.dll", "dll:c.dll"}));
	settings.overlay.enabled = false;
	EXPECT_EQ(RequestedFeatures(settings, true, dlls), (std::vector<std::string> {"dll:a.dll", "dll:c.dll"}));
}

TEST(ModCatalogue, KeepsOffEachFeatureOfAStartThatDidNotComplete)
{
	LoaderSettings settings;
	settings.modDlls.disabled = {"z.dll"};
	const std::vector<std::filesystem::path> dlls {"a.dll", "b.dll"};
	const auto requested = RequestedFeatures(settings, true, dlls);
	const StartupMarker last {.processId = 1, .version = "0.1.0", .gameVersion = "1.128.90.1030", .features = requested};
	const auto decision = DecideStartup(FormatStartupMarker(last), requested);
	ASSERT_TRUE(decision.lastStartFailed);
	const auto off = TurnOffFailedFeatures(settings, decision.featuresOff);
	EXPECT_TRUE(off.overlay);
	EXPECT_FALSE(settings.overlay.enabled);
	EXPECT_EQ(off.dlls, (std::vector<std::string> {"a.dll", "b.dll"}));
	EXPECT_EQ(settings.modDlls.disabled, (std::vector<std::string> {"z.dll", "a.dll", "b.dll"}));
	EXPECT_EQ(off.patch,
	          nlohmann::json({{"overlay", {{"enabled", false}}}, {"modDlls", {{"disabled", {"z.dll", "a.dll", "b.dll"}}}}}));
	EXPECT_EQ(off.notes.size(), 3u);
	// The next start asks for no DLL that the guard turned off.
	EXPECT_TRUE(RequestedFeatures(settings, true, dlls).empty());
	EXPECT_TRUE(loader::CheckLoaderSettings(settings));
}

TEST(ModCatalogue, AStartThatCompletedTurnsNothingOff)
{
	LoaderSettings settings;
	const std::vector<std::filesystem::path> dlls {"a.dll"};
	const auto decision = DecideStartup(std::nullopt, RequestedFeatures(settings, true, dlls));
	EXPECT_FALSE(decision.lastStartFailed);
	const auto off = TurnOffFailedFeatures(settings, decision.featuresOff);
	EXPECT_FALSE(off.overlay);
	EXPECT_TRUE(off.dlls.empty());
	EXPECT_TRUE(off.patch.is_null());
	EXPECT_EQ(settings, LoaderSettings {});
}
