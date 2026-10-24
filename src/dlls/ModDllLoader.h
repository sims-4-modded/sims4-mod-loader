/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <common/Result.h>
#include <core/HostTable.h>
#include <core/ModCatalogue.h>
#include <core/ModRegistry.h>
#include <mod_api/Ts4ModApi.h>
#include <nlohmann/json.hpp>

struct _EXCEPTION_POINTERS;

namespace ts4_mod::loader
{

using mods::ModDllRecord;
using mods::ModDllState;

/// Unloads a DLL with `FreeLibrary`.
struct ModuleFree
{
	void operator()(void* module) const;
};

/// A loaded DLL. The extender releases the handle with no unload if the code of the DLL can still be in use.
using ModuleHandle = std::unique_ptr<void, ModuleFree>;

/// One DLL mod: the record for the list of the mods, and what the extender keeps to call it.
struct ModDll: ModDllRecord
{
	OwnerId owner = 0;
	ModuleHandle module;
	Ts4ModShutdownFn shutdown = nullptr;
	std::unique_ptr<HostTable> table;
};

/// The filter of a guard around a call into a mod: return EXCEPTION_EXECUTE_HANDLER (1) to catch the exception.
using ExceptionFilter = int (*)(_EXCEPTION_POINTERS* pointers);

/// Call `function(context)`. Return false if an exception that `filter` catches stops it (for example an access
/// violation in a mod). The stack then unwinds with no destructors, so the caller stops all calls to that mod.
[[nodiscard]] bool GuardedCall(void (*function)(void*), void* context, ExceptionFilter filter);

/// The filter of `GuardedCall` that catches only access violations.
[[nodiscard]] int CatchAccessViolation(_EXCEPTION_POINTERS* pointers);

/// Return the DLL files in a folder, in the order of their names. A link or a folder with the extension is not a DLL.
[[nodiscard]] std::vector<std::filesystem::path> FindModDlls(const std::filesystem::path& folder);

/// Loads the DLL mods and calls their exports. It never lets a bad mod stop the game: the extender does not use a mod
/// with a missing export, a different interface version, a bad id or a failed init, and removes its registrations.
class ModDllLoader
{
public:
	ModDllLoader(HostServices& services, HostInfo info, ExceptionFilter filter = &CatchAccessViolation);
	/// Call `ShutdownAll`, then unload the DLLs of the mods that loaded.
	~ModDllLoader();
	ModDllLoader(const ModDllLoader&) = delete;
	ModDllLoader& operator=(const ModDllLoader&) = delete;

	/// Load one DLL and call its init with a new owner. A mod whose id is in `disabledIds` (`mods.disabled`) gets the
	/// state `Disabled` before its init, and the extender unloads it. The result tells the state; the list keeps it.
	const ModDll& Load(const std::filesystem::path& path, std::span<const std::string> disabledIds = {});
	/// Add a DLL that does not load, for the list of the menu bar. The record gets the checksum of the file.
	const ModDll& AddDisabled(const std::filesystem::path& path, std::string reason);

	/// Call `Ts4ModShutdown` of each loaded mod, in the reverse order of the loads. The DLLs stay in memory until the
	/// loader goes out of scope.
	void ShutdownAll();

	/// Stop the calls to a mod after an error in its code.
	void MarkFailed(OwnerId owner, const std::string& reason);

	[[nodiscard]] const std::vector<std::unique_ptr<ModDll>>& Mods() const { return _mods; }
	[[nodiscard]] const ModDll* Find(OwnerId owner) const;
	/// The list of the DLL mods as a JSON array, in the order of the loads (see `mods::DescribeModDll`).
	[[nodiscard]] nlohmann::json Describe() const;

private:
	ModDll& Refuse(ModDll& mod, std::string error, bool unload);

	HostServices& _services;
	HostInfo _info;
	ExceptionFilter _filter;
	std::vector<std::unique_ptr<ModDll>> _mods;
	OwnerId _nextOwner = 1;
	bool _shutDown = false;
};

} // namespace ts4_mod::loader
