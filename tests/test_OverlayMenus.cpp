/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <cstdint>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <core/ModRegistry.h>
#include <core/OverlayMenus.h>
#include <gtest/gtest.h>
#include <mod_core/Hotkey.h>

using namespace ts4_mod;
using namespace ts4_mod::loader;
using namespace ts4_mod::overlay;

namespace
{

void Task(void* /*user*/) {}
uint32_t Load(void* /*user*/, const char* /*json*/, Ts4Text* /*error*/)
{
	return 1;
}
void Get(void* /*user*/, Ts4FieldState* /*state*/) {}
uint32_t Set(void* /*user*/, const Ts4FieldValue* /*value*/, uint32_t /*save*/, Ts4Text* /*message*/)
{
	return 1;
}
uint32_t Enabled(void* /*user*/)
{
	return 1;
}
void Read(void* /*user*/, Ts4Text* /*text*/) {}

[[nodiscard]] Ts4FieldDesc Field(const char* menu, const char* section, const char* key, const char* label)
{
	return Ts4FieldDesc {.menu = menu,
	                     .group = "G",
	                     .section = section,
	                     .key = key,
	                     .label = label,
	                     .help = "Help",
	                     .kind = TS4_FIELD_NUMBER,
	                     .min = 1.0,
	                     .max = 5.0,
	                     .step = 0.0,
	                     .format = "%.1f",
	                     .logarithmic = 0,
	                     .restartOnly = 1,
	                     .choices = nullptr,
	                     .choiceCount = 0};
}

[[nodiscard]] Ts4ActionDesc Action(const char* menu, const char* label, const char* hotkey, const char* defaultHotkey)
{
	return Ts4ActionDesc {
	    .menu = menu, .group = "Actions", .label = label, .help = nullptr, .hotkey = hotkey, .defaultHotkey = defaultHotkey};
}

/// A reader that gives fixed values, and fails for the ids in `failing`.
class FakeReader final: public MenuReader
{
public:
	bool ReadField(const FieldEntry& field, Ts4FieldState& state) override
	{
		fieldsRead.push_back(field.id);
		if (failing.contains(field.id))
		{
			return false;
		}
		state.value.number = 2.5;
		state.restartPending = 1;
		return true;
	}
	bool ActionEnabled(const ActionEntry& action) override { return !failing.contains(action.id); }
	std::optional<std::string> ReadReadout(const ReadoutEntry& readout) override
	{
		if (failing.contains(readout.id))
		{
			return std::nullopt;
		}
		return "text " + std::to_string(readout.id);
	}

	std::set<size_t> failing;
	std::vector<size_t> fieldsRead;
};

/// The extender (owner 0) and two mods with fields, actions and readouts.
struct Registrations
{
	Registrations()
	{
		registry.SetOwnerName(1, "Camera");
		registry.SetOwnerName(2, "Needs");
		EXPECT_TRUE(registry.AddSettings(1, "camera", &Load, nullptr));
		EXPECT_TRUE(registry.AddSettings(2, "needs", &Load, nullptr));
		// Owner 2 registers first, so that the order of the owners comes from the owner, not from the time.
		needsReadout = *registry.AddReadout(2, nullptr, "State", &Read, nullptr);
		needsDecay = *registry.AddField(2, Field(nullptr, "needs", "decay", "Decay"), &Get, &Set, nullptr);
		cameraZoom = *registry.AddField(1, Field(nullptr, "camera", "zoom", "Zoom"), &Get, &Set, nullptr);
		cameraReset = *registry.AddAction(1, Action(nullptr, "Reset", "resetCamera", "Ctrl+F5"), &Task, &Enabled, nullptr);
		cameraHidden = *registry.AddAction(1, Action(nullptr, nullptr, "hiddenCamera", "Ctrl+F6"), &Task, nullptr, nullptr);
		cameraDebug = *registry.AddField(1, Field("Debug", "camera", "trace", "Trace"), &Get, &Set, nullptr);
		loaderScale = *registry.AddField(0, Field(nullptr, "overlay", "fontScale", "Text size"), &Get, &Set, nullptr);
		loaderDebug = *registry.AddReadout(0, "Debug", nullptr, &Read, nullptr);
	}

	[[nodiscard]] std::vector<HotkeyBinding> Bindings() const
	{
		std::vector<std::string> errors;
		auto bindings = registry.ResolveHotkeys({}, errors);
		EXPECT_TRUE(errors.empty());
		return bindings;
	}

	ModRegistry registry;
	size_t needsReadout = 0;
	size_t needsDecay = 0;
	size_t cameraZoom = 0;
	size_t cameraReset = 0;
	size_t cameraHidden = 0;
	size_t cameraDebug = 0;
	size_t loaderScale = 0;
	size_t loaderDebug = 0;
};

[[nodiscard]] std::vector<std::string> MenuNames(const MenuSet& set)
{
	std::vector<std::string> names;
	for (const auto& menu : set.menus)
	{
		names.push_back(menu.name);
	}
	return names;
}

[[nodiscard]] std::vector<size_t> ItemIds(const Menu& menu)
{
	std::vector<size_t> ids;
	for (const auto& item : menu.items)
	{
		ids.push_back(item.id);
	}
	return ids;
}

} // namespace

TEST(OverlayMenus, EachItemGoesToTheMenuOfItsOwnerOrToTheMenuThatItNames)
{
	Registrations r;
	FakeReader reader;
	const auto bindings = r.Bindings();
	const auto set = BuildMenus(r.registry, bindings, reader);

	// The menus come in the order of the owners: the extender, then the mods by their number.
	EXPECT_EQ(MenuNames(set), (std::vector<std::string> {"Debug", "Camera", "Needs"}));
	EXPECT_EQ(ItemIds(set.menus[0]), (std::vector<size_t> {r.loaderDebug, r.cameraDebug}));
	// The action with no label has no button.
	EXPECT_EQ(ItemIds(set.menus[1]), (std::vector<size_t> {r.cameraZoom, r.cameraReset}));
	EXPECT_EQ(ItemIds(set.menus[2]), (std::vector<size_t> {r.needsReadout, r.needsDecay}));
	ASSERT_EQ(set.loaderItems.size(), 1u);
	EXPECT_EQ(set.loaderItems[0].id, r.loaderScale);
	EXPECT_EQ(set.loaderItems[0].path, "overlay.fontScale");
}

TEST(OverlayMenus, ItemsGetTheValuesTheRangesAndTheHotkeys)
{
	Registrations r;
	FakeReader reader;
	const auto bindings = r.Bindings();
	const auto set = BuildMenus(r.registry, bindings, reader);

	const auto& zoom = set.menus[1].items[0];
	EXPECT_EQ(zoom.kind, MenuItem::Kind::Field);
	EXPECT_EQ(zoom.label, "Zoom");
	EXPECT_EQ(zoom.path, "camera.zoom");
	EXPECT_EQ(zoom.fieldKind, TS4_FIELD_NUMBER);
	EXPECT_EQ(zoom.format, "%.1f");
	EXPECT_TRUE(zoom.restartOnly);
	EXPECT_DOUBLE_EQ(zoom.state.min, 1.0);
	EXPECT_DOUBLE_EQ(zoom.state.max, 5.0);
	EXPECT_DOUBLE_EQ(zoom.state.value.number, 2.5);
	EXPECT_EQ(zoom.state.restartPending, 1u);

	const auto& reset = set.menus[1].items[1];
	EXPECT_EQ(reset.kind, MenuItem::Kind::Action);
	EXPECT_TRUE(reset.enabled);
	EXPECT_EQ(reset.hotkey, FormatHotkey(*ParseHotkey("Ctrl+F5")));

	const auto& readout = set.menus[2].items[0];
	EXPECT_EQ(readout.kind, MenuItem::Kind::Readout);
	EXPECT_EQ(readout.group, "State");
	EXPECT_EQ(readout.text, "text " + std::to_string(r.needsReadout));

	// The list of the hotkeys has each action with a hotkey, also the action with no button.
	ASSERT_EQ(set.hotkeys.size(), 2u);
	EXPECT_EQ(set.hotkeys[0].name, "resetCamera");
	EXPECT_EQ(set.hotkeys[0].owner, "Camera");
	EXPECT_EQ(set.hotkeys[1].name, "hiddenCamera");
}

TEST(OverlayMenus, AFailedCallKeepsTheItemWithItsDefaults)
{
	Registrations r;
	FakeReader reader;
	reader.failing = {r.cameraZoom, r.cameraReset, r.needsReadout};
	const auto set = BuildMenus(r.registry, r.Bindings(), reader);

	const auto& zoom = set.menus[1].items[0];
	EXPECT_DOUBLE_EQ(zoom.state.value.number, 0.0);
	EXPECT_DOUBLE_EQ(zoom.state.min, 1.0);
	EXPECT_FALSE(set.menus[1].items[1].enabled);
	EXPECT_TRUE(set.menus[2].items[0].text.empty());
}

TEST(OverlayMenus, AFailedOwnerGivesNoItemsAndNoCalls)
{
	Registrations r;
	r.registry.SetFailed(1, "an access violation");
	FakeReader reader;
	const auto set = BuildMenus(r.registry, r.Bindings(), reader);

	EXPECT_EQ(MenuNames(set), (std::vector<std::string> {"Debug", "Needs"}));
	EXPECT_EQ(ItemIds(set.menus[0]), (std::vector<size_t> {r.loaderDebug}));
	EXPECT_EQ(std::ranges::count(reader.fieldsRead, r.cameraZoom), 0);
	EXPECT_EQ(std::ranges::count(reader.fieldsRead, r.cameraDebug), 0);
}

TEST(OverlayMenus, FieldValuesHasEachFieldOfAllMenus)
{
	Registrations r;
	FakeReader reader;
	OverlaySnapshot snapshot;
	snapshot.menus = BuildMenus(r.registry, r.Bindings(), reader);
	snapshot.menus.menus[1].items[0].state.value.number = 4.0;

	const auto values = FieldValues(snapshot);
	EXPECT_EQ(values.size(), 4u);
	EXPECT_DOUBLE_EQ(values.at(r.cameraZoom).number, 4.0);
	EXPECT_DOUBLE_EQ(values.at(r.loaderScale).number, 2.5);
	EXPECT_TRUE(values.contains(r.cameraDebug));
	EXPECT_TRUE(values.contains(r.needsDecay));
	EXPECT_FALSE(values.contains(r.needsReadout));
}
