/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include "core/OverlayMenus.h"

#include <algorithm>
#include <iterator>
#include <tuple>
#include <utility>

#include <mod_core/Hotkey.h>

namespace ts4_mod::overlay
{

using loader::k_LoaderOwner;
using loader::OwnerId;

namespace
{
[[nodiscard]] MenuItem ReadField(const loader::FieldEntry& field, MenuReader& reader)
{
	MenuItem item {.kind = MenuItem::Kind::Field,
	               .id = field.id,
	               .group = field.group,
	               .label = field.label,
	               .help = field.help,
	               .path = field.Path(),
	               .fieldKind = field.kind,
	               .format = field.format,
	               .logarithmic = field.logarithmic,
	               .restartOnly = field.restartOnly,
	               .choices = field.choices};
	item.state = Ts4FieldState {.value = {}, .min = field.min, .max = field.max, .restartPending = 0};
	(void)reader.ReadField(field, item.state);
	return item;
}
} // namespace

MenuSet BuildMenus(const loader::ModRegistry& registry, std::span<const loader::HotkeyBinding> bindings, MenuReader& reader)
{
	struct Ordered
	{
		OwnerId owner;
		std::string menu;
		MenuItem item;
	};
	std::vector<Ordered> items;
	for (const auto& field : registry.Fields())
	{
		if (!registry.Failed(field.owner))
		{
			items.push_back({.owner = field.owner, .menu = field.menu, .item = ReadField(field, reader)});
		}
	}
	std::map<size_t, std::string> hotkeys;
	for (const auto& binding : bindings)
	{
		hotkeys[binding.actionId] = FormatHotkey(binding.hotkey);
	}
	for (const auto& action : registry.Actions())
	{
		if (action.label.empty() || registry.Failed(action.owner))
		{
			continue;
		}
		MenuItem item {.kind = MenuItem::Kind::Action,
		               .id = action.id,
		               .group = action.group,
		               .label = action.label,
		               .help = action.help,
		               .hotkey = hotkeys.contains(action.id) ? hotkeys.at(action.id) : std::string()};
		if (action.enabled != nullptr)
		{
			item.enabled = reader.ActionEnabled(action);
		}
		items.push_back({.owner = action.owner, .menu = action.menu, .item = std::move(item)});
	}
	for (const auto& readout : registry.Readouts())
	{
		if (registry.Failed(readout.owner))
		{
			continue;
		}
		const auto text = reader.ReadReadout(readout);
		items.push_back({.owner = readout.owner,
		                 .menu = readout.menu,
		                 .item = MenuItem {.kind = MenuItem::Kind::Readout,
		                                   .id = readout.id,
		                                   .group = readout.group,
		                                   .text = text.value_or(std::string())}});
	}
	// The order of the owners for the menus, and the order of the registrations in each menu. The ids of one registry
	// increase with each registration.
	std::ranges::stable_sort(
	    items, [](const Ordered& a, const Ordered& b) { return std::tie(a.owner, a.item.id) < std::tie(b.owner, b.item.id); });
	MenuSet set;
	for (auto& [owner, menuName, item] : items)
	{
		if (owner == k_LoaderOwner && menuName.empty())
		{
			set.loaderItems.push_back(std::move(item));
			continue;
		}
		const auto name = menuName.empty() ? registry.OwnerName(owner) : menuName;
		auto menu = std::ranges::find(set.menus, name, &Menu::name);
		if (menu == set.menus.end())
		{
			set.menus.push_back(Menu {.name = name, .items = {}});
			menu = std::prev(set.menus.end());
		}
		menu->items.push_back(std::move(item));
	}
	for (const auto& binding : bindings)
	{
		const auto* action = registry.FindAction(binding.actionId);
		set.hotkeys.push_back({.name = binding.name,
		                       .hotkey = FormatHotkey(binding.hotkey),
		                       .owner = action != nullptr ? registry.OwnerName(action->owner) : std::string()});
	}
	return set;
}

std::map<size_t, Ts4FieldValue> FieldValues(const OverlaySnapshot& snapshot)
{
	std::map<size_t, Ts4FieldValue> values;
	const auto add = [&values](const std::vector<MenuItem>& items) {
		for (const auto& item : items)
		{
			if (item.kind == MenuItem::Kind::Field)
			{
				values[item.id] = item.state.value;
			}
		}
	};
	add(snapshot.menus.loaderItems);
	for (const auto& menu : snapshot.menus.menus)
	{
		add(menu.items);
	}
	return values;
}

} // namespace ts4_mod::overlay
