/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

// The start of the extender. The game loads the proxy winmm.dll from Game\Bin. DllMain loads the system
// winmm.dll, fills the table of the proxy stubs and, only in TS4_x64.exe, starts one thread. That thread waits
// until the activation wrapper decrypted the program, and then writes the log of the start.
//
// The activation wrapper starts TS4_x64.exe two times. The first process (the stub) can load winmm.dll before the
// decryption and stops after a short time. The EA app then starts the game process. In the stub, the thread writes
// one line and waits; when the stub stops, DllMain writes one more line. The proxy does nothing else there.
//
// This stage installs no hook and loads no DLL mod. It lists the DLL mods with their checksums.

#include <cstdlib>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <core/GameStart.h>
#include <core/ModCatalogue.h>
#include <core/Sha256.h>
#include <dlls/ModDllLoader.h>
#include <mod_core/Folders.h>
#include <mod_core/GameVersion.h>
#include <mod_core/SettingsFile.h>
#include <mod_core/StartupGuard.h>
#include <shlobj.h>
#include <windows.h>

#include "game/loader/ExeImage.h"
#include "game/loader/LoaderLog.h"
#include "game/proxy/WinmmExports.h"

// The proxy stubs (ProxyStubs.asm) jump through this table to the functions of the system winmm.dll. It is the
// only global table, because the stubs are assembly and cannot get a pointer in another way. DllMain fills it before
// any call can reach a stub, and nothing changes it after that.
extern "C" void* g_winmmFunctions[ts4_mod::proxy::k_WinmmExports.size()] = {};

/// The install tool and game_builds.py find this export name in the file to know that a winmm.dll is the extender.
extern "C" const char* Ts4ModLoaderVersion()
{
	return TS4_LOADER_VERSION;
}

namespace ts4_mod::loader
{

namespace
{
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

/// The longest time that the start thread waits for the decryption of the program.
constexpr auto k_DecryptTimeout = std::chrono::minutes(5);
/// The time between two samples of the entropy of the code section.
constexpr auto k_DecryptPollInterval = Milliseconds(50);
/// The longest time that the start thread monitors the load of the game modules after the decryption.
constexpr auto k_ModuleWatchTime = std::chrono::minutes(3);
/// The time between two checks of the game modules.
constexpr auto k_ModulePollInterval = Milliseconds(100);
/// The time after the load of the Python DLL after which this stage calls the start stable.
constexpr auto k_StableTime = std::chrono::seconds(30);
/// The name of the log file of the extender in the folder of the log files.
constexpr std::wstring_view k_LogFileName = L"loader.log";
/// The game modules that the log monitors.
constexpr std::array k_GameModules {L"Simulation_x64.dll", L"python37_x64.dll"};

/// The steps of the start thread. DllMain reads it when the process stops.
enum class Phase
{
	/// The thread did not start, or it waits for the decryption.
	Waiting,
	/// The program is decrypted and the extender started.
	Started,
	/// The extender does not start in this process.
	Off,
};

/// The state of the proxy in one process. DllMain makes it when the DLL loads, and it stays until the process stops.
/// The start thread writes it; DllMain reads it when the process stops.
struct ProxyState
{
	std::atomic<Phase> phase {Phase::Waiting};
	/// The path of the log file and the last line of a process that stopped before the decryption. The start
	/// thread sets them before it sets `exitLineReady`, and does not change them after that.
	std::wstring logPath;
	std::string exitLine;
	std::atomic<bool> exitLineReady {false};
	/// The path of this DLL and the age of the process when Windows loaded it. DllMain sets them.
	std::filesystem::path dllPath;
	std::chrono::milliseconds loadAge {0};
};

/// The one state object of the proxy. See `DllMain` for its lifetime.
std::unique_ptr<ProxyState> s_state;

struct ModuleFree
{
	void operator()(HMODULE module) const { FreeLibrary(module); }
};

/// Load the system winmm.dll by its full path and fill the table of the proxy stubs. The system DLL stays loaded
/// until the process stops, because the stubs jump into it. A named export is found by its name, and the export with
/// no name by its ordinal.
[[nodiscard]] bool LoadSystemWinmm()
{
	std::array<wchar_t, MAX_PATH> folder {};
	const auto length = GetSystemDirectoryW(folder.data(), static_cast<UINT>(folder.size()));
	if (length == 0 || length >= folder.size())
	{
		return false;
	}
	const auto path = std::wstring(folder.data(), length) + L"\\winmm.dll";
	std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleFree> module(LoadLibraryW(path.c_str()));
	if (!module)
	{
		return false;
	}
	for (std::size_t i = 0; i < proxy::k_WinmmExports.size(); ++i)
	{
		const auto& entry = proxy::k_WinmmExports[i];
		const char* name = entry.name != nullptr ? entry.name : MAKEINTRESOURCEA(entry.ordinal);
		g_winmmFunctions[i] = reinterpret_cast<void*>(GetProcAddress(module.get(), name));
		if (g_winmmFunctions[i] == nullptr)
		{
			return false;
		}
	}
	// The stubs use the module until the process stops.
	(void)module.release();
	return true;
}

[[nodiscard]] std::wstring ReadEnvironment(const wchar_t* name)
{
	std::array<wchar_t, 1024> value {};
	const auto length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
	return length == 0 || length >= value.size() ? std::wstring() : std::wstring(value.data(), length);
}

[[nodiscard]] std::filesystem::path ProgramPath()
{
	std::array<wchar_t, 32768> path {};
	const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	return std::filesystem::path(std::wstring(path.data(), length));
}

[[nodiscard]] std::string Utf8(const std::filesystem::path& path)
{
	const auto text = path.u8string();
	return {text.begin(), text.end()};
}

/// The time since the start of this process.
[[nodiscard]] Milliseconds ProcessAge()
{
	FILETIME creation {};
	FILETIME exit {};
	FILETIME kernel {};
	FILETIME user {};
	if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == FALSE)
	{
		return Milliseconds(0);
	}
	FILETIME now {};
	GetSystemTimeAsFileTime(&now);
	const auto ticks = [](const FILETIME& time) {
		return static_cast<std::int64_t>(static_cast<std::uint64_t>(time.dwHighDateTime) << 32 | time.dwLowDateTime);
	};
	// FILETIME counts in units of 100 nanoseconds.
	return Milliseconds((ticks(now) - ticks(creation)) / 10'000);
}

/// The user folder of the game: `Documents\Electronic Arts\The Sims 4`.
[[nodiscard]] std::optional<std::filesystem::path> UserFolder()
{
	PWSTR documents = nullptr;
	const auto result = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents);
	const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(documents, &CoTaskMemFree);
	if (FAILED(result) || documents == nullptr)
	{
		return std::nullopt;
	}
	return std::filesystem::path(documents) / L"Electronic Arts" / L"The Sims 4";
}

/// Read the version of the game from `GameVersion.txt` in the user folder.
[[nodiscard]] std::string ReadGameVersion()
{
	const auto folder = UserFolder();
	if (!folder)
	{
		return "unknown (no Documents folder)";
	}
	const auto text = ReadTextFile(*folder / L"GameVersion.txt");
	if (!text)
	{
		return std::format("unknown (no file {})", Utf8(*folder / L"GameVersion.txt"));
	}
	const auto version = ReadGameVersionFile(std::as_bytes(std::span(text->data(), text->size())));
	return version ? ToString(*version) : std::format("unknown ({})", version.GetError().message);
}

[[nodiscard]] std::string ModuleText(const wchar_t* name)
{
	const auto module = GetModuleHandleW(name);
	return module == nullptr ? std::string("not loaded yet")
	                         : std::format("loaded at 0x{:x}", reinterpret_cast<std::uintptr_t>(module));
}

/// Read the marker of the last start and write the marker of this start. This stage uses no feature that the guard
/// turns off, so the marker only tells the next start if this start did not complete.
[[nodiscard]] bool BeginStartupGuard(LoaderLog& log, const ModFolders& folders, const std::string& gameVersion)
{
	const auto decision = DecideStartup(ReadTextFile(folders.startupMarker), {});
	if (decision.lastStartFailed)
	{
		if (const auto& last = decision.lastStart)
		{
			log.Info("start-up: the last start did not complete: process {}, extender {}, game {}, started {}, stage '{}'",
			         last->processId, last->version, last->gameVersion, last->startedAt, last->stage);
		}
		else
		{
			log.Info("start-up: the marker of the last start is not correct: {}", decision.markerError);
		}
	}
	else
	{
		log.Info("start-up: no marker of the last start: the last start completed");
	}
	const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
	const StartupMarker marker {.processId = static_cast<std::uint32_t>(GetCurrentProcessId()),
	                            .version = TS4_LOADER_VERSION,
	                            .gameVersion = gameVersion,
	                            .startedAt = std::format("{:%Y-%m-%d %H:%M:%S}", now),
	                            .stage = "proxy",
	                            .features = {}};
	if (auto written = WriteTextFileAtomically(folders.startupMarker, FormatStartupMarker(marker)); !written)
	{
		log.Info("start-up: the extender cannot write the marker {}: {}", Utf8(folders.startupMarker),
		         written.GetError().message);
		return false;
	}
	log.Info("start-up: the marker {} stays until the start is stable", Utf8(folders.startupMarker));
	return true;
}

/// List the DLL mods with their checksums. This stage does not load them.
void ListModDlls(LoaderLog& log, const ModFolders& folders)
{
	const auto folder = mods::ModDllFolder(folders.settingsFolder);
	std::error_code error;
	if (!std::filesystem::is_directory(folder, error))
	{
		log.Info("mod DLLs: the folder {} does not exist", Utf8(folder));
		return;
	}
	const auto files = FindModDlls(folder);
	log.Info("mod DLLs: {} DLLs in {}; this stage does not load them", files.size(), Utf8(folder));
	for (const auto& file : files)
	{
		const auto sha = Sha256OfFile(file);
		log.Info("mod DLLs:   {} sha256 {}", Utf8(file.filename()), sha ? *sha : sha.GetError().message);
	}
}

/// Monitor the load of the game modules, and remove the marker when the start is stable.
void WatchGameModules(LoaderLog& log, const ModFolders& folders, bool markerWritten)
{
	std::array<bool, k_GameModules.size()> loaded {};
	std::optional<Clock::time_point> pythonLoaded;
	const auto deadline = Clock::now() + k_ModuleWatchTime;
	while (Clock::now() < deadline)
	{
		for (std::size_t i = 0; i < k_GameModules.size(); ++i)
		{
			if (!loaded[i] && GetModuleHandleW(k_GameModules[i]) != nullptr)
			{
				loaded[i] = true;
				log.Info("modules: {} {}, {} ms after the start of the process", Utf8(std::filesystem::path(k_GameModules[i])),
				         ModuleText(k_GameModules[i]), ProcessAge().count());
			}
		}
		if (loaded.back() && !pythonLoaded)
		{
			pythonLoaded = Clock::now();
		}
		if (pythonLoaded && Clock::now() - *pythonLoaded >= k_StableTime)
		{
			break;
		}
		std::this_thread::sleep_for(k_ModulePollInterval);
	}
	if (!pythonLoaded)
	{
		log.Info("modules: python37_x64.dll did not load in {} s; the marker stays",
		         std::chrono::duration_cast<std::chrono::seconds>(k_ModuleWatchTime).count());
		return;
	}
	if (markerWritten)
	{
		std::error_code error;
		std::filesystem::remove(folders.startupMarker, error);
		log.Info("start-up: stable {} s after the load of python37_x64.dll; {}",
		         std::chrono::duration_cast<std::chrono::seconds>(k_StableTime).count(),
		         error ? std::format("the extender cannot remove the marker: {}", error.message())
		               : std::string("the extender removed the marker"));
	}
}

/// The start of the extender after the decryption. This stage writes the log only.
void StartExtender(LoaderLog& log, const ModFolders& folders, ExeImage& image, const DecryptionWatch& watch,
                   Milliseconds waited)
{
	log.RotateIfLarge();
	log.Info("the extender {} starts in process {} ({})", TS4_LOADER_VERSION, GetCurrentProcessId(), Utf8(ProgramPath()));
	const auto gameVersion = ReadGameVersion();
	log.Info("game version {}", gameVersion);
	log.Info("TS4_x64.exe: base 0x{:x}, .text {} bytes", image.Base(), image.Text().virtualSize);
	log.Info(".text entropy: {:.3f} at the start, {:.3f} when decrypted ({} samples, {} ms of wait, {} ms after the "
	         "start of the process)",
	         watch.First().value_or(0.0), watch.Last().value_or(0.0), watch.Samples(), waited.count(), ProcessAge().count());
	for (const auto* module : k_GameModules)
	{
		log.Info("modules: {} {} at the decryption", Utf8(std::filesystem::path(module)), ModuleText(module));
	}
	const bool markerWritten = BeginStartupGuard(log, folders, gameVersion);
	ListModDlls(log, folders);
	WatchGameModules(log, folders, markerWritten);
	log.Info("the start of the extender is complete; this stage installs no hook and loads no DLL mod");
}

void Start(ProxyState& state)
{
	const auto program = ProgramPath();
	std::vector<std::filesystem::path> gameFolders {program.parent_path()};
	if (const auto game = GameFolderOfExe(program))
	{
		gameFolders.push_back(*game);
	}
	const auto folders = MakeModFolders(ModFolderInputs {.localAppData = ReadEnvironment(L"LOCALAPPDATA"),
	                                                     .settingsFile = ReadEnvironment(L"TS4_MODS_SETTINGS"),
	                                                     .logFolder = ReadEnvironment(L"TS4_LOADER_LOG_FOLDER")},
	                                    gameFolders);
	if (!folders)
	{
		// No log is possible: the extender never writes into the game folder.
		state.phase = Phase::Off;
		return;
	}
	LoaderLog log(folders->logFolder / k_LogFileName);
	auto image = ExeImage::Open();
	if (!image)
	{
		log.Info("{}; the extender does not start", image.GetError().message);
		state.phase = Phase::Off;
		return;
	}
	const auto started = Clock::now();
	DecryptionWatch watch;
	if (const auto entropy = image->TextEntropy())
	{
		(void)watch.Add(*entropy);
	}
	log.Info("proxy: Windows loaded {} {} ms after the start of the process {} ({}); .text entropy {:.3f} now ({} ms)",
	         Utf8(state.dllPath), state.loadAge.count(), GetCurrentProcessId(), Utf8(program.filename()),
	         watch.Last().value_or(0.0), ProcessAge().count());
	if (watch.State() == ImageState::Encrypted)
	{
		log.Info("{} loaded the proxy before the decryption (.text entropy {:.3f}, {} ms after the start of the "
		         "process); the proxy only forwards calls until the program is decrypted",
		         Utf8(program.filename()), watch.Last().value_or(0.0), ProcessAge().count());
		state.logPath = log.Path().wstring();
		state.exitLine = std::format("stub process: the process stopped before the decryption (.text entropy {:.3f} "
		                             "at the start); the proxy only forwarded calls",
		                             watch.Last().value_or(0.0));
		state.exitLineReady = true;
	}
	while (watch.State() != ImageState::Ready)
	{
		if (Clock::now() - started >= k_DecryptTimeout)
		{
			state.phase = Phase::Off;
			state.exitLineReady = false;
			log.Info("the program was not decrypted in {} s (.text entropy {:.3f}); the extender does not start",
			         std::chrono::duration_cast<std::chrono::seconds>(k_DecryptTimeout).count(), watch.Last().value_or(0.0));
			return;
		}
		std::this_thread::sleep_for(k_DecryptPollInterval);
		if (const auto entropy = image->TextEntropy())
		{
			(void)watch.Add(*entropy);
		}
	}
	state.exitLineReady = false;
	state.phase = Phase::Started;
	StartExtender(log, *folders, *image, watch, std::chrono::duration_cast<Milliseconds>(Clock::now() - started));
}

DWORD WINAPI StartThread(void* parameter)
{
	auto& state = *static_cast<ProxyState*>(parameter);
	// No C++ exception may leave the extender into the game.
	try
	{
		Start(state);
	}
	catch (...)
	{
		state.phase = Phase::Off;
		state.exitLineReady = false;
	}
	return 0;
}

struct HandleClose
{
	void operator()(HANDLE handle) const { CloseHandle(handle); }
};

/// Start the extender in this process if it is the game. DllMain calls it, so it only makes the state and one thread.
void StartInGameProcess(HINSTANCE instance)
{
	// TS4_MOD_LOADER=0 keeps only the proxy, so that the game runs with no extender.
	if (!IsGameExe(ProgramPath()) || ReadEnvironment(L"TS4_MOD_LOADER") == L"0")
	{
		return;
	}
	// The start thread uses the code of this DLL until the process stops. Thus the DLL must never unload.
	HMODULE pinned = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
	                       reinterpret_cast<LPCWSTR>(instance), &pinned) == FALSE)
	{
		return;
	}
	s_state = std::make_unique<ProxyState>();
	s_state->loadAge = ProcessAge();
	std::array<wchar_t, MAX_PATH> dll {};
	const auto length = GetModuleFileNameW(instance, dll.data(), static_cast<DWORD>(dll.size()));
	s_state->dllPath = std::filesystem::path(std::wstring(dll.data(), length));
	const std::unique_ptr<void, HandleClose> thread(CreateThread(nullptr, 0, StartThread, s_state.get(), 0, nullptr));
	if (!thread)
	{
		s_state.reset();
	}
}

/// Write the last line of a process that stopped before the decryption. The other threads stopped at a random point,
/// so the function takes no lock and makes no allocation.
void OnProcessExit()
{
	if (s_state == nullptr)
	{
		return;
	}
	if (s_state->phase.load() == Phase::Waiting && s_state->exitLineReady.load())
	{
		WriteLineAtExit(s_state->logPath.c_str(), s_state->exitLine);
	}
	// The stopped threads can hold the lock of the log, so the function keeps the state.
	(void)s_state.release();
}

} // namespace

} // namespace ts4_mod::loader

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_DETACH)
	{
		// When the process stops, `reserved` is not null. The extender pins the DLL in the game, so no other unload
		// occurs there.
		if (reserved != nullptr)
		{
			ts4_mod::loader::OnProcessExit();
		}
		return TRUE;
	}
	if (reason != DLL_PROCESS_ATTACH)
	{
		return TRUE;
	}
	DisableThreadLibraryCalls(instance);
	if (!ts4_mod::loader::LoadSystemWinmm())
	{
		return FALSE;
	}
	ts4_mod::loader::StartInGameProcess(instance);
	return TRUE;
}
