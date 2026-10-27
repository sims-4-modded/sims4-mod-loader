/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include "game/overlay/OverlayUi.h"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

#include <core/ModCatalogue.h>
#include <imgui.h>

namespace ts4_mod::overlay
{

namespace
{
/// The width of the controls in a menu, in multiples of the font size.
constexpr float k_ControlWidth = 14.0f;
/// The width of a long line of text, in multiples of the font size.
constexpr float k_WrapWidth = 40.0f;
/// The colour of an error and of a warning.
const ImVec4 k_ErrorColour(1.0f, 0.5f, 0.4f, 1.0f);
const ImVec4 k_WarningColour(1.0f, 0.85f, 0.3f, 1.0f);

[[nodiscard]] std::string Text(const nlohmann::json& object, const char* key)
{
	if (!object.is_object() || !object.contains(key))
	{
		return {};
	}
	const auto& value = object.at(key);
	return value.is_string() ? value.get<std::string>() : value.dump();
}

[[nodiscard]] bool Flag(const nlohmann::json& object, const char* key)
{
	return object.is_object() && object.contains(key) && object.at(key).is_boolean() && object.at(key).get<bool>();
}

void ErrorText(const char* prefix, const std::string& error)
{
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * k_WrapWidth);
	ImGui::TextColored(k_ErrorColour, "%s%s", prefix, error.c_str());
	ImGui::PopTextWrapPos();
}

void Tooltip(const MenuItem& item)
{
	if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
	{
		return;
	}
	std::string text = item.help;
	if (!item.path.empty())
	{
		text += "\n" + item.path;
	}
	if (item.restartOnly)
	{
		text += "\nThe change works after a restart of the game.";
		text += item.state.restartPending != 0 ? "\nA restart is necessary now." : "";
	}
	if (!item.hotkey.empty())
	{
		text += "\nHotkey: " + item.hotkey;
	}
	if (!text.empty())
	{
		ImGui::SetTooltip("%s", text.c_str());
	}
}

/// Change the field in the draft and give the command to the main thread.
void Edit(const OverlayUiFrame& frame, const MenuItem& item, const Ts4FieldValue& value, bool save)
{
	frame.draft[item.id] = value;
	frame.post(OverlayCommand {.kind = OverlayCommand::Kind::Edit, .id = item.id, .value = value, .save = save});
}

void DrawField(const OverlayUiFrame& frame, const MenuItem& item)
{
	ImGui::PushID(static_cast<int>(item.id));
	auto value = frame.draft.contains(item.id) ? frame.draft.at(item.id) : item.state.value;
	auto label = item.label;
	if (item.restartOnly)
	{
		label += item.state.restartPending != 0 ? " (restart now)" : " (restart)";
	}
	switch (item.fieldKind)
	{
	case TS4_FIELD_TOGGLE:
	{
		bool on = value.toggle != 0;
		if (ImGui::Checkbox(label.c_str(), &on))
		{
			value.toggle = on ? 1u : 0u;
			Edit(frame, item, value, true);
		}
		break;
	}
	case TS4_FIELD_NUMBER:
	{
		double number = value.number;
		const double min = item.state.min;
		const double max = item.state.max;
		ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
		if (item.logarithmic)
		{
			flags |= ImGuiSliderFlags_Logarithmic;
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * k_ControlWidth);
		const auto* format = item.format.empty() ? "%.2f" : item.format.c_str();
		if (ImGui::SliderScalar(label.c_str(), ImGuiDataType_Double, &number, &min, &max, format, flags))
		{
			// The mods use the value at once; mods.json gets it when the user releases the slider.
			value.number = number;
			Edit(frame, item, value, false);
		}
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			Edit(frame, item, frame.draft[item.id], true);
		}
		break;
	}
	case TS4_FIELD_CHOICE:
	{
		const auto current = static_cast<size_t>(value.choice);
		const auto* preview = current < item.choices.size() ? item.choices[current].c_str() : "";
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * k_ControlWidth);
		if (ImGui::BeginCombo(label.c_str(), preview))
		{
			for (size_t index = 0; index < item.choices.size(); ++index)
			{
				if (ImGui::Selectable(item.choices[index].c_str(), index == current) && index != current)
				{
					value.choice = static_cast<uint32_t>(index);
					Edit(frame, item, value, true);
				}
			}
			ImGui::EndCombo();
		}
		break;
	}
	}
	Tooltip(item);
	ImGui::PopID();
}

void DrawAction(const OverlayUiFrame& frame, const MenuItem& item)
{
	ImGui::PushID(static_cast<int>(item.id));
	ImGui::BeginDisabled(!item.enabled);
	if (ImGui::Button(item.label.c_str()))
	{
		frame.post(OverlayCommand {.kind = OverlayCommand::Kind::Action, .id = item.id});
	}
	ImGui::EndDisabled();
	Tooltip(item);
	ImGui::PopID();
}

/// Draw the items of one menu, with a heading for each group.
void DrawItems(const OverlayUiFrame& frame, const std::vector<MenuItem>& items)
{
	std::string group;
	for (const auto& item : items)
	{
		if (item.group != group)
		{
			group = item.group;
			if (!group.empty())
			{
				ImGui::SeparatorText(group.c_str());
			}
		}
		switch (item.kind)
		{
		case MenuItem::Kind::Field:
			DrawField(frame, item);
			break;
		case MenuItem::Kind::Action:
			DrawAction(frame, item);
			break;
		case MenuItem::Kind::Readout:
			ImGui::TextUnformatted(item.text.c_str());
			break;
		}
	}
}

void DrawModMenus(const OverlayUiFrame& frame)
{
	for (const auto& menu : frame.snapshot.menus.menus)
	{
		if (ImGui::BeginMenu(menu.name.c_str()))
		{
			DrawItems(frame, menu.items);
			ImGui::EndMenu();
		}
	}
}

void DrawStatus(const OverlayUiFrame& frame)
{
	ImGui::SeparatorText("Status");
	if (frame.snapshot.mods.is_array())
	{
		for (const auto& mod : frame.snapshot.mods)
		{
			ImGui::Text("%s: %s, %s, %s", Text(mod, "name").c_str(), Flag(mod, "enabled") ? "on" : "off",
			            Flag(mod, "ready") ? "ready" : "not ready", Flag(mod, "active") ? "active" : "not active");
			if (const auto error = Text(mod, "lastError"); !error.empty())
			{
				ErrorText("  last error: ", error);
			}
		}
	}
	if (frame.snapshot.hooks.is_object())
	{
		for (const auto& [name, hook] : frame.snapshot.hooks.items())
		{
			ImGui::Text("%s: %s, %s calls", name.c_str(), Flag(hook, "installed") ? "installed" : "not installed",
			            Text(hook, "calls").empty() ? "0" : Text(hook, "calls").c_str());
		}
	}
	const auto& status = frame.overlayStatus;
	ImGui::Text("Menu bar: %s, %s frames drawn", Text(status, "api").c_str(), Text(status, "framesDrawn").c_str());
	if (const auto error = Text(status, "lastError"); !error.empty())
	{
		ErrorText("Menu bar error: ", error);
	}
	const auto& reload = frame.snapshot.lastReload;
	if (reload.is_object() && !reload.empty())
	{
		ImGui::Text("Last reload: %s (%s), %s", Text(reload, "time").c_str(), Text(reload, "source").c_str(),
		            Flag(reload, "applied") ? "applied" : "not applied");
		if (const auto error = Text(reload, "error"); !error.empty())
		{
			ErrorText("  error: ", error);
		}
	}
}

void DrawModsMenu(const OverlayUiFrame& frame)
{
	if (!ImGui::BeginMenu("Mods"))
	{
		return;
	}
	if (ImGui::MenuItem("Reload mods.json"))
	{
		frame.post(OverlayCommand {.kind = OverlayCommand::Kind::Reload});
	}
	ImGui::TextDisabled("Settings file:");
	ImGui::TextUnformatted(frame.snapshot.settingsPath.c_str());
	if (ImGui::SmallButton("Copy the path"))
	{
		ImGui::SetClipboardText(frame.snapshot.settingsPath.c_str());
	}
	DrawStatus(frame);
	DrawItems(frame, frame.snapshot.menus.loaderItems);
	ImGui::SeparatorText("Hotkeys");
	if (!frame.snapshot.hotkeysReady)
	{
		ImGui::TextDisabled("The hotkeys are not ready (see the log).");
	}
	if (ImGui::BeginTable("hotkeys", 3, ImGuiTableFlags_SizingFixedFit))
	{
		for (const auto& row : frame.snapshot.menus.hotkeys)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(row.name.c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(row.hotkey.c_str());
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", row.owner.c_str());
		}
		ImGui::EndTable();
	}
	ImGui::EndMenu();
}

/// The tooltip of a DLL mod: its file, id, version and checksum.
void ModDllTooltip(const nlohmann::json& dll)
{
	if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled) || !ImGui::BeginTooltip())
	{
		return;
	}
	ImGui::Text("File: %s", Text(dll, "file").c_str());
	if (const auto id = Text(dll, "id"); !id.empty())
	{
		ImGui::Text("Id: %s, version %s", id.c_str(), Text(dll, "version").c_str());
	}
	ImGui::Text("SHA-256: %s", Text(dll, "sha256").c_str());
	ImGui::EndTooltip();
}

/// The DLL mods in the folder of the DLL mods: a switch for each DLL by its file name, and its state.
void DrawModListMenu(const OverlayUiFrame& frame)
{
	if (!ImGui::BeginMenu("Mod list"))
	{
		return;
	}
	ImGui::TextDisabled("The DLL mods in:");
	ImGui::TextUnformatted(frame.snapshot.dllFolder.c_str());
	ImGui::TextDisabled("Each DLL loads, except one that you turn off. No game file changes.");
	ImGui::TextDisabled("A change works after a restart of the game.");
	if (frame.draftLoader.mods != frame.snapshot.settings.mods || frame.draftLoader.modDlls != frame.snapshot.settings.modDlls)
	{
		ImGui::TextColored(k_WarningColour, "Restart the game to use the changes.");
	}
	ImGui::SeparatorText("DLL mods");
	const auto dlls = frame.snapshot.dllList.is_array() ? frame.snapshot.dllList : nlohmann::json::array();
	if (dlls.empty())
	{
		ImGui::TextDisabled("No DLL mods. Put a DLL mod into the folder above.");
	}
	for (size_t index = 0; index < dlls.size(); ++index)
	{
		const auto& dll = dlls[index];
		const auto file = Text(dll, "file");
		const auto id = Text(dll, "id");
		ImGui::PushID(static_cast<int>(index));
		bool on = !mods::DisabledByFile(frame.draftLoader, file).has_value();
		const auto label =
		    Text(dll, "name").empty() ? file : std::format("{} {} ({})", Text(dll, "name"), Text(dll, "version"), file);
		if (ImGui::Checkbox(label.c_str(), &on) && mods::EditModDllEnabled(frame.draftLoader, file, on))
		{
			frame.post(OverlayCommand {.kind = OverlayCommand::Kind::ModDll, .on = on, .name = file});
		}
		ModDllTooltip(dll);
		ImGui::SameLine();
		ImGui::TextDisabled("(%s)", Text(dll, "state").c_str());
		// The settings can also turn the mod off by its id. Then the switch of the file alone cannot turn it on.
		if (!id.empty() && mods::DisabledById(frame.draftLoader, id).has_value())
		{
			ImGui::TextColored(k_WarningColour, "  Off by its id %s.", id.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("Turn on by its id") && mods::EditModEnabled(frame.draftLoader, id, true))
			{
				frame.post(OverlayCommand {.kind = OverlayCommand::Kind::Mod, .on = true, .name = id});
			}
		}
		if (const auto error = Text(dll, "error"); !error.empty())
		{
			ErrorText("  ", error);
		}
		ImGui::PopID();
	}
	ImGui::EndMenu();
}

void DrawHelpMenu(const OverlayUiFrame& frame)
{
	if (!ImGui::BeginMenu("Help"))
	{
		return;
	}
	ImGui::Text("The Sims 4 mod loader %s", frame.snapshot.version.c_str());
	ImGui::Text("Game version: %s", frame.snapshot.gameVersion.c_str());
	ImGui::Text("Renderer: %s", Text(frame.overlayStatus, "api").c_str());
	ImGui::Text("Dear ImGui %s", IMGUI_VERSION);
	ImGui::Separator();
	const auto pin = std::ranges::find(frame.snapshot.menus.hotkeys, std::string("toggleOverlay"), &HotkeyRow::name);
	const auto pinText = pin != frame.snapshot.menus.hotkeys.end() ? pin->hotkey : std::string("The hotkey toggleOverlay");
	ImGui::Text("%s shows this bar while the game runs, or hides it.", pinText.c_str());
	ImGui::TextUnformatted("A change works at once and goes into mods.json.");
	ImGui::TextUnformatted("A field with (restart) works after a restart of the game.");
	ImGui::EndMenu();
}
} // namespace

void DrawOverlayMenuBar(const OverlayUiFrame& frame)
{
	if (!ImGui::BeginMainMenuBar())
	{
		return;
	}
	DrawModMenus(frame);
	DrawModListMenu(frame);
	DrawModsMenu(frame);
	DrawHelpMenu(frame);
	const auto summary =
	    std::format("{} mods{}", frame.snapshot.menus.menus.size(), Flag(frame.overlayStatus, "pinned") ? "  (pinned)" : "");
	ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(summary.c_str()).x -
	                (ImGui::GetStyle().ItemSpacing.x * 2.0f));
	ImGui::TextDisabled("%s", summary.c_str());
	ImGui::EndMainMenuBar();
}

} // namespace ts4_mod::overlay
