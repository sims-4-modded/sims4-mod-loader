/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

// The options of Dear ImGui in the extender. ModImGui.cmake gives this file to ImGui as IMGUI_USER_CONFIG, for the ImGui
// sources and for the extender.

#pragma once

// The menu bar does not use the old functions of ImGui.
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
// The menu bar uses no gamepad, no input method editor and no shell. Thus the extender loads no XInput, imm32 or
// shell32.
#define IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
#define IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS

// A failed check of ImGui must not stop the game. The menu bar continues, or stops after an error of its own code.
#define IM_ASSERT(_EXPR) ((void)0)
