/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <cstddef>

#include <functional>
#include <map>

#include <core/LoaderSettings.h>
#include <core/OverlayMenus.h>
#include <mod_api/Ts4ModApi.h>
#include <nlohmann/json.hpp>

namespace ts4_mod::overlay
{

/// What the menu bar draws in one frame.
struct OverlayUiFrame
{
	const OverlaySnapshot& snapshot;
	/// The values that the controls show and change, by the id of the field. A change goes to `post` too.
	std::map<size_t, Ts4FieldValue>& draft;
	/// The settings of the extender that the mod list changes.
	loader::LoaderSettings& draftLoader;
	const std::function<void(OverlayCommand)>& post;
	/// The status of the menu bar itself (`Overlay::Describe`).
	const nlohmann::json& overlayStatus;
};

/// Draw the main menu bar: a menu for each mod, the menus Mod list and Mods, and Help. Call it between
/// `ImGui::NewFrame` and `ImGui::Render`.
void DrawOverlayMenuBar(const OverlayUiFrame& frame);

} // namespace ts4_mod::overlay
