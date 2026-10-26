/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <filesystem>
#include <string>

#include <mod_core/Folders.h>

#include "game/loader/LoaderLog.h"

namespace ts4_mod::loader
{

/// What the start thread knows when the program is decrypted.
struct ExtenderStart
{
	ModFolders folders;
	std::string version;     ///< The version of the extender.
	std::string gameVersion; ///< For example "1.128.90.1030".
	std::filesystem::path gameFolder;
	std::filesystem::path userFolder;
};

/// Start the extender in the game process after the decryption. The start thread calls it one time. It reads mods.json,
/// starts the start-up guard and puts the bootstrap hook on the tick function. The main thread does the rest at its
/// first tick: the DLL mods, the hook manager, the hooks of the mods, the hotkeys, the menu bar and the tasks. Each failure
/// turns off only its part and writes one line to the log.
///
/// The state of the extender lives until the process stops, because the hooks use it.
void StartExtender(LoaderLog& log, const ExtenderStart& start);

} // namespace ts4_mod::loader
