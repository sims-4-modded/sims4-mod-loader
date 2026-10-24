/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <set>
#include <string>

#include <core/LoaderSettings.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace ts4_mod;
using namespace ts4_mod::loader;

namespace
{

/// Write one value into the JSON of the settings at the path of the field.
nlohmann::json WithValue(const LoaderSettings& settings, const settings::Field<LoaderSettings>& field,
                         const nlohmann::json& value)
{
	auto json = LoaderSettingsJson(settings);
	if (field.section.empty())
	{
		json[std::string(field.key)] = value;
	}
	else
	{
		json[std::string(field.section)][std::string(field.key)] = value;
	}
	return json;
}

} // namespace

TEST(LoaderSettings, KeepsTheDefaultsAndIgnoresTheObjectsOfTheMods)
{
	const auto settings = ParseLoaderSettings(R"({"camera": {"enabled": true, "anything": 3}, "needs": [1]})");
	ASSERT_TRUE(settings) << settings.GetError().message;
	EXPECT_FALSE(settings->tickHook);
	EXPECT_TRUE(settings->liveReload);
	EXPECT_TRUE(settings->overlay.enabled);
	EXPECT_TRUE(settings->hotkeys.keys.empty());
	EXPECT_TRUE(settings->mods.disabled.empty());
	EXPECT_TRUE(settings->modDlls.disabled.empty());
}

TEST(LoaderSettings, ReadsTheHotkeysOfAllActions)
{
	const auto settings = ParseLoaderSettings(
	    R"({"hotkeys": {"enabled": false, "messageSeconds": 4, "toggleCamera": "Ctrl+Alt+C", "goToSpot1": "none"}})");
	ASSERT_TRUE(settings) << settings.GetError().message;
	EXPECT_FALSE(settings->hotkeys.enabled);
	EXPECT_DOUBLE_EQ(settings->hotkeys.messageSeconds, 4.0);
	EXPECT_EQ(FormatHotkey(settings->hotkeys.keys.at("toggleCamera")), "Ctrl+Alt+C");
	EXPECT_FALSE(settings->hotkeys.keys.at("goToSpot1").Enabled());
	EXPECT_FALSE(ParseLoaderSettings(R"({"hotkeys": {"toggleCamera": 5}})"));
	EXPECT_FALSE(ParseLoaderSettings(R"({"hotkeys": {"toggleCamera": "Meta+C"}})"));
	EXPECT_FALSE(ParseLoaderSettings(R"({"hotkeys": {"messageSeconds": 60}})"));
}

TEST(LoaderSettings, ReadsTheModDlls)
{
	const auto settings = ParseLoaderSettings(R"({"modDlls": {"disabled": ["better_needs.dll", "Upper.DLL"]}})");
	ASSERT_TRUE(settings);
	EXPECT_EQ(settings->modDlls.disabled, (std::vector<std::string> {"better_needs.dll", "Upper.DLL"}));
	EXPECT_FALSE(ParseLoaderSettings(R"({"modDlls": {"disabled": ["../x.dll"]}})"));
	EXPECT_FALSE(ParseLoaderSettings(R"({"modDlls": {"disabled": ["C:x.dll"]}})"));
	EXPECT_FALSE(ParseLoaderSettings(R"({"modDlls": {"disabled": [".dll"]}})"));
	EXPECT_FALSE(ParseLoaderSettings(R"({"modDlls": {"disabled": ["notes.txt"]}})"));
	EXPECT_FALSE(ParseLoaderSettings(R"({"modDlls": {"disabled": "a.dll"}})"));
	EXPECT_FALSE(ParseLoaderSettings(R"({"modDlls": {"other": []}})"));
	EXPECT_FALSE(ParseLoaderSettings(R"({"modDlls": []})"));
}

TEST(LoaderSettings, ReadsTheIdsOfTheModsThatAreOff)
{
	const auto settings = ParseLoaderSettings(R"({"mods": {"disabled": ["better_needs", "mod-2"]}})");
	ASSERT_TRUE(settings) << settings.GetError().message;
	EXPECT_EQ(settings->mods.disabled, (std::vector<std::string> {"better_needs", "mod-2"}));
	EXPECT_FALSE(ParseLoaderSettings(R"({"mods": {"disabled": ["bad id!"]}})"));
	EXPECT_FALSE(ParseLoaderSettings(R"({"mods": {"disabled": [""]}})"));
	EXPECT_FALSE(ParseLoaderSettings(R"({"mods": {"disabled": [3]}})"));
	// The keys of the mod zips of Masters of Albion are not settings of this extender.
	EXPECT_FALSE(ParseLoaderSettings(R"({"mods": {"map": "a"}})"));
}

TEST(LoaderSettings, ChecksTheNamesOfModsAndOfDlls)
{
	EXPECT_TRUE(IsModId("mod_template"));
	EXPECT_TRUE(IsModId(std::string(64, 'a')));
	EXPECT_FALSE(IsModId(std::string(65, 'a')));
	EXPECT_FALSE(IsModId("a.b"));
	EXPECT_TRUE(IsDllFileName("a.dll"));
	EXPECT_TRUE(IsDllFileName("A.DLL"));
	EXPECT_FALSE(IsDllFileName("dlls\\a.dll"));
	EXPECT_FALSE(IsDllFileName("dlls/a.dll"));
	EXPECT_FALSE(IsDllFileName("a.dll.txt"));
}

TEST(LoaderSettings, TheExtenderOwnsItsTopLevelKeys)
{
	EXPECT_EQ(k_LoaderKeys.size(), 6u);
	const auto json = LoaderSettingsJson(LoaderSettings {});
	EXPECT_EQ(json.size(), k_LoaderKeys.size());
	for (const auto key : k_LoaderKeys)
	{
		EXPECT_TRUE(json.contains(std::string(key))) << key;
	}
}

TEST(LoaderSettings, TheJsonReadsBackTheSame)
{
	auto settings = *ParseLoaderSettings(R"({"tickHook": true, "hotkeys": {"showSettings": "F9"},
		"overlay": {"fontScale": 1.5}, "mods": {"disabled": ["a"]}, "modDlls": {"disabled": ["x.dll"]}})");
	const auto again = ParseLoaderSettings(LoaderSettingsJson(settings).dump());
	ASSERT_TRUE(again) << again.GetError().message;
	EXPECT_EQ(*again, settings);
}

TEST(LoaderSettings, OnlyTheHotkeysAndTheTextSizeChangeLive)
{
	LoaderSettings current;
	auto next = current;
	next.overlay.fontScale = 2.0;
	next.overlay.enabled = false;
	next.tickHook = true;
	next.mods.disabled = {"a"};
	next.modDlls.disabled = {"a.dll"};
	const auto change = CompareLoaderSettings(current, next);
	EXPECT_TRUE(change.overlay);
	EXPECT_FALSE(change.hotkeys);
	EXPECT_EQ(change.restartOnly, (std::vector<std::string> {"tickHook", "overlay.enabled", "mods", "modDlls"}));
	const auto live = LiveLoaderSettings(current, next);
	EXPECT_DOUBLE_EQ(live.overlay.fontScale, 2.0);
	EXPECT_TRUE(live.overlay.enabled);
	EXPECT_FALSE(live.tickHook);
	EXPECT_TRUE(live.mods.disabled.empty());
	EXPECT_TRUE(live.modDlls.disabled.empty());
}

TEST(LoaderSettings, EachFieldIsInTheJsonOneTimeAndItsRangeAgreesWithTheFile)
{
	const LoaderSettings defaults;
	const auto json = LoaderSettingsJson(defaults);
	std::set<std::string> paths;
	for (const auto& field : LoaderFields())
	{
		const auto path = settings::FieldPath(field);
		EXPECT_TRUE(paths.insert(path).second) << path;
		const auto& object = field.section.empty() ? json : json.at(std::string(field.section));
		EXPECT_TRUE(object.contains(std::string(field.key))) << path;
		EXPECT_FALSE(field.help.empty()) << path;
		if (field.kind == settings::FieldKind::Number)
		{
			EXPECT_TRUE(ParseLoaderSettings(WithValue(defaults, field, field.min).dump())) << path;
			EXPECT_TRUE(ParseLoaderSettings(WithValue(defaults, field, field.max).dump())) << path;
		}
	}
}

TEST(LoaderSettings, EditOfAFieldGivesAPatchOfOneField)
{
	LoaderSettings settings;
	const auto* field = settings::FindField<LoaderSettings>(LoaderFields(), "overlay", "fontScale");
	ASSERT_NE(field, nullptr);
	const auto edit = EditLoaderField(settings, *field, settings::FieldValue {9.0});
	ASSERT_TRUE(edit) << edit.GetError().message;
	EXPECT_DOUBLE_EQ(settings.overlay.fontScale, 3.0);
	EXPECT_EQ(edit->patch, nlohmann::json({{"overlay", {{"fontScale", 3.0}}}}));
	const auto* live = settings::FindField<LoaderSettings>(LoaderFields(), "", "liveReload");
	ASSERT_NE(live, nullptr);
	EXPECT_TRUE(live->restartOnly);
	const auto off = EditLoaderField(settings, *live, settings::FieldValue {false});
	ASSERT_TRUE(off);
	EXPECT_EQ(off->patch, nlohmann::json({{"liveReload", false}}));
}
