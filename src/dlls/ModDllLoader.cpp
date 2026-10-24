/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

#include <core/Sha256.h>
#include <dlls/ModDllLoader.h>
#include <windows.h>

namespace ts4_mod::loader
{

namespace
{
[[nodiscard]] std::string Utf8(const std::filesystem::path& path)
{
	const auto text = path.u8string();
	return {text.begin(), text.end()};
}

/// Copy a text of a mod, with at most `limit` characters. The copy stops at the limit if the text has no zero before it.
[[nodiscard]] std::string Copy(const char* text, size_t limit)
{
	if (text == nullptr)
	{
		return {};
	}
	const auto* end = std::find(text, text + limit, '\0');
	return {text, end};
}

/// The contexts of the guarded calls.
struct ApiVersionCall
{
	Ts4ModApiVersionFn function;
	uint32_t result;
};

struct InfoCall
{
	Ts4ModGetInfoFn function;
	std::string id;
	std::string name;
	std::string version;
};

struct InitCall
{
	Ts4ModInitFn function;
	const Ts4HostApi* api;
	uint32_t result;
};

/// The checksum of the file, or empty if the file cannot be read.
[[nodiscard]] std::string ChecksumOf(const std::filesystem::path& path)
{
	auto checksum = Sha256OfFile(path);
	return checksum ? std::move(*checksum) : std::string();
}
} // namespace

void ModuleFree::operator()(void* module) const
{
	if (module != nullptr)
	{
		FreeLibrary(static_cast<HMODULE>(module));
	}
}

bool GuardedCall(void (*function)(void*), void* context, ExceptionFilter filter)
{
	__try
	{
		function(context);
		return true;
	}
	__except (filter(GetExceptionInformation()))
	{
		return false;
	}
}

int CatchAccessViolation(_EXCEPTION_POINTERS* pointers)
{
	return pointers != nullptr && pointers->ExceptionRecord != nullptr &&
	               pointers->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
	           ? EXCEPTION_EXECUTE_HANDLER
	           : EXCEPTION_CONTINUE_SEARCH;
}

std::vector<std::filesystem::path> FindModDlls(const std::filesystem::path& folder)
{
	std::vector<std::filesystem::path> files;
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(folder, error))
	{
		std::error_code kindError;
		if (IsDllFileName(Utf8(entry.path().filename())) && !entry.is_symlink(kindError) && entry.is_regular_file(kindError))
		{
			files.push_back(entry.path());
		}
	}
	std::ranges::sort(files, {}, [](const std::filesystem::path& path) { return path.filename().wstring(); });
	return files;
}

ModDllLoader::ModDllLoader(HostServices& services, HostInfo info, ExceptionFilter filter)
    : _services(services)
    , _info(std::move(info))
    , _filter(filter)
{
}

ModDllLoader::~ModDllLoader()
{
	ShutdownAll();
	for (auto& mod : _mods)
	{
		if (mod->module)
		{
			// The registry must not keep a function of a DLL that the extender unloads.
			_services.Registry().RemoveOwner(mod->owner);
			mod->module.reset();
		}
	}
}

ModDll& ModDllLoader::Refuse(ModDll& mod, std::string error, bool unload)
{
	// The extender calls the mod no more, also for a task that the mod posted in its init.
	_services.Registry().RemoveOwner(mod.owner);
	_services.Registry().SetFailed(mod.owner, error);
	mod.state = ModDllState::Refused;
	mod.error = std::move(error);
	mod.shutdown = nullptr;
	if (unload)
	{
		mod.module.reset();
	}
	else
	{
		// The code of the mod can still be in use (a thread, a pointer), so the DLL stays in memory.
		(void)mod.module.release();
	}
	_services.Log(mod.owner, TS4_LOG_ERROR, std::format("the DLL mod {} is not used: {}", mod.file, mod.error));
	return mod;
}

const ModDll& ModDllLoader::AddDisabled(const std::filesystem::path& path, std::string reason)
{
	auto mod = std::make_unique<ModDll>();
	mod->file = Utf8(path.filename());
	mod->sha256 = ChecksumOf(path);
	mod->state = ModDllState::Disabled;
	mod->error = std::move(reason);
	_mods.push_back(std::move(mod));
	return *_mods.back();
}

const ModDll& ModDllLoader::Load(const std::filesystem::path& path, std::span<const std::string> disabledIds)
{
	auto& mod = *_mods.emplace_back(std::make_unique<ModDll>());
	mod.file = Utf8(path.filename());
	mod.owner = _nextOwner++;
	_services.Registry().SetOwnerName(mod.owner, mod.file);

	// The checksum comes before the load, so the log tells which file the extender tried.
	auto checksum = Sha256OfFile(path);
	if (!checksum)
	{
		return Refuse(mod, checksum.GetError().message, false);
	}
	mod.sha256 = std::move(*checksum);

	// Only the folder of the DLL and the system folders give the DLLs that the mod needs.
	const auto module =
	    LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (module == nullptr)
	{
		return Refuse(mod, std::format("Windows cannot load it (error {})", GetLastError()), false);
	}
	mod.module.reset(module);
	const auto apiVersion = reinterpret_cast<Ts4ModApiVersionFn>(GetProcAddress(module, TS4_MOD_EXPORT_API_VERSION));
	const auto getInfo = reinterpret_cast<Ts4ModGetInfoFn>(GetProcAddress(module, TS4_MOD_EXPORT_GET_INFO));
	const auto init = reinterpret_cast<Ts4ModInitFn>(GetProcAddress(module, TS4_MOD_EXPORT_INIT));
	const auto shutdown = reinterpret_cast<Ts4ModShutdownFn>(GetProcAddress(module, TS4_MOD_EXPORT_SHUTDOWN));
	if (apiVersion == nullptr || getInfo == nullptr || init == nullptr || shutdown == nullptr)
	{
		return Refuse(mod,
		              std::format("it is not a mod: it must export {}, {}, {} and {}", TS4_MOD_EXPORT_API_VERSION,
		                          TS4_MOD_EXPORT_GET_INFO, TS4_MOD_EXPORT_INIT, TS4_MOD_EXPORT_SHUTDOWN),
		              true);
	}

	ApiVersionCall versionCall {.function = apiVersion, .result = 0};
	if (!GuardedCall(
	        [](void* c) {
		        auto& call = *static_cast<ApiVersionCall*>(c);
		        call.result = call.function();
	        },
	        &versionCall, _filter))
	{
		return Refuse(mod, "an access violation in Ts4ModApiVersion", false);
	}
	if (versionCall.result != TS4_MOD_API_VERSION)
	{
		return Refuse(
		    mod,
		    std::format("it uses the interface version {}; this extender uses {}", versionCall.result, TS4_MOD_API_VERSION),
		    true);
	}

	InfoCall infoCall {.function = getInfo, .id = {}, .name = {}, .version = {}};
	if (!GuardedCall(
	        [](void* c) {
		        auto& call = *static_cast<InfoCall*>(c);
		        const auto* info = call.function();
		        if (info != nullptr)
		        {
			        call.id = Copy(info->id, 65);
			        call.name = Copy(info->name, 128);
			        call.version = Copy(info->version, 64);
		        }
	        },
	        &infoCall, _filter))
	{
		return Refuse(mod, "an access violation in Ts4ModGetInfo", false);
	}
	if (!IsModId(infoCall.id))
	{
		return Refuse(mod, std::format("the id '{}' must be 1 to 64 letters, digits, '-' and '_'", infoCall.id), true);
	}
	const auto same = std::ranges::find_if(_mods, [&](const std::unique_ptr<ModDll>& other) {
		return other.get() != &mod && other->state == ModDllState::Loaded && other->id == infoCall.id;
	});
	if (same != _mods.end())
	{
		return Refuse(mod, std::format("the mod {} in {} has the same id", infoCall.id, (*same)->file), true);
	}
	mod.id = infoCall.id;
	mod.name = infoCall.name.empty() ? infoCall.id : infoCall.name;
	mod.version = infoCall.version;
	_services.Registry().SetOwnerName(mod.owner, mod.name);
	if (std::ranges::find(disabledIds, mod.id) != disabledIds.end())
	{
		// The user turned the mod off by its id. The DLL did not run its init, so the extender unloads it.
		mod.module.reset();
		mod.state = ModDllState::Disabled;
		mod.error = "mods.disabled in mods.json";
		return mod;
	}

	mod.table = std::make_unique<HostTable>(_services, mod.owner, _info);
	InitCall initCall {.function = init, .api = &mod.table->Api(), .result = 0};
	const bool completed = GuardedCall(
	    [](void* c) {
		    auto& call = *static_cast<InitCall*>(c);
		    call.result = call.function(call.api, TS4_MOD_API_VERSION);
	    },
	    &initCall, _filter);
	mod.table->Close();
	if (!completed)
	{
		Refuse(mod, "an access violation in Ts4ModInit", false);
		mod.state = ModDllState::Failed;
		return mod;
	}
	if (initCall.result == 0)
	{
		return Refuse(mod, "its Ts4ModInit returned 0 (see the lines of the mod above)", true);
	}
	mod.shutdown = shutdown;
	mod.state = ModDllState::Loaded;
	_services.Log(mod.owner, TS4_LOG_INFO,
	              std::format("the DLL mod {} loaded: {} {} ({}), SHA-256 {}, {} refused registrations", mod.file, mod.name,
	                          mod.version, mod.id, mod.sha256, mod.table->Refusals()));
	return mod;
}

void ModDllLoader::ShutdownAll()
{
	if (std::exchange(_shutDown, true))
	{
		return;
	}
	for (auto& mod : std::ranges::reverse_view(_mods))
	{
		if (mod->state != ModDllState::Loaded || mod->shutdown == nullptr)
		{
			continue;
		}
		if (!GuardedCall([](void* c) { (*static_cast<Ts4ModShutdownFn*>(c))(); }, &mod->shutdown, _filter))
		{
			_services.Log(mod->owner, TS4_LOG_ERROR, std::format("an access violation in Ts4ModShutdown of {}", mod->file));
		}
	}
}

void ModDllLoader::MarkFailed(OwnerId owner, const std::string& reason)
{
	for (auto& mod : _mods)
	{
		if (mod->owner == owner && mod->state == ModDllState::Loaded)
		{
			mod->state = ModDllState::Failed;
			mod->error = reason;
			// The code of the mod can still be in use, so the DLL stays in memory.
			(void)mod->module.release();
		}
	}
}

const ModDll* ModDllLoader::Find(OwnerId owner) const
{
	const auto found = std::ranges::find_if(_mods, [owner](const auto& mod) { return mod->owner == owner; });
	return found != _mods.end() ? found->get() : nullptr;
}

nlohmann::json ModDllLoader::Describe() const
{
	auto list = nlohmann::json::array();
	for (const auto& mod : _mods)
	{
		list.push_back(mods::DescribeModDll(*mod));
	}
	return list;
}

} // namespace ts4_mod::loader
