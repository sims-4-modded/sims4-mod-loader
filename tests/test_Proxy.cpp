/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <cstdint>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>
#include <windows.h>

namespace
{
struct ModuleFree
{
	void operator()(HMODULE module) const { FreeLibrary(module); }
};
using Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleFree>;

[[nodiscard]] std::wstring SystemFile(std::wstring_view name)
{
	std::array<wchar_t, MAX_PATH> folder {};
	const auto length = GetSystemDirectoryW(folder.data(), static_cast<UINT>(folder.size()));
	return std::wstring(folder.data(), length) + L"\\" + std::wstring(name);
}

/// The path of the proxy that the build made, with the separators of Windows.
[[nodiscard]] std::wstring ProxyPath()
{
	return std::filesystem::path(TS4_PROXY_DLL).make_preferred().wstring();
}

template <typename Function>
[[nodiscard]] Function Find(HMODULE module, const char* name)
{
	return reinterpret_cast<Function>(GetProcAddress(module, name));
}

/// The exports of a loaded module by their ordinals. An export with no name has an empty name.
[[nodiscard]] std::map<std::uint32_t, std::string> ReadExports(HMODULE module)
{
	const auto* base = reinterpret_cast<const std::byte*>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	const auto& entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
	std::map<std::uint32_t, std::string> exports;
	if (entry.VirtualAddress == 0)
	{
		return exports;
	}
	const auto* directory = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + entry.VirtualAddress);
	const auto* functions = reinterpret_cast<const std::uint32_t*>(base + directory->AddressOfFunctions);
	const auto* names = reinterpret_cast<const std::uint32_t*>(base + directory->AddressOfNames);
	const auto* ordinals = reinterpret_cast<const std::uint16_t*>(base + directory->AddressOfNameOrdinals);
	for (DWORD i = 0; i < directory->NumberOfFunctions; ++i)
	{
		if (functions[i] != 0)
		{
			exports[directory->Base + i] = {};
		}
	}
	for (DWORD i = 0; i < directory->NumberOfNames; ++i)
	{
		exports[directory->Base + ordinals[i]] = reinterpret_cast<const char*>(base + names[i]);
	}
	return exports;
}
} // namespace

// The test program is not TS4_x64.exe, so the proxy only forwards the calls and starts no thread.
TEST(Proxy, ForwardsTheCallsToTheSystemWinmmDll)
{
	const Module proxy(LoadLibraryW(ProxyPath().c_str()));
	ASSERT_TRUE(proxy) << "the proxy did not load: error " << GetLastError();
	const Module system(LoadLibraryW(SystemFile(L"winmm.dll").c_str()));
	ASSERT_TRUE(system);
	ASSERT_NE(proxy.get(), system.get());

	using TimeFunction = DWORD(WINAPI*)();
	const auto proxyTime = Find<TimeFunction>(proxy.get(), "timeGetTime");
	const auto systemTime = Find<TimeFunction>(system.get(), "timeGetTime");
	ASSERT_NE(proxyTime, nullptr);
	ASSERT_NE(systemTime, nullptr);
	ASSERT_NE(reinterpret_cast<void*>(proxyTime), reinterpret_cast<void*>(systemTime));
	const auto before = systemTime();
	const auto value = proxyTime();
	const auto after = systemTime();
	EXPECT_LE(value - before, after - before);

	using PeriodFunction = UINT(WINAPI*)(UINT);
	const auto begin = Find<PeriodFunction>(proxy.get(), "timeBeginPeriod");
	const auto end = Find<PeriodFunction>(proxy.get(), "timeEndPeriod");
	ASSERT_NE(begin, nullptr);
	ASSERT_NE(end, nullptr);
	EXPECT_EQ(begin(1), 0U);
	EXPECT_EQ(end(1), 0U);

	using CountFunction = UINT(WINAPI*)();
	const auto proxyDevices = Find<CountFunction>(proxy.get(), "waveOutGetNumDevs");
	const auto systemDevices = Find<CountFunction>(system.get(), "waveOutGetNumDevs");
	ASSERT_NE(proxyDevices, nullptr);
	EXPECT_EQ(proxyDevices(), systemDevices());
}

TEST(Proxy, ExportsTheNamesAndTheOrdinalsOfTheSystemWinmmDll)
{
	const Module proxy(LoadLibraryW(ProxyPath().c_str()));
	ASSERT_TRUE(proxy);
	const Module system(LoadLibraryW(SystemFile(L"winmm.dll").c_str()));
	ASSERT_TRUE(system);
	const auto proxyExports = ReadExports(proxy.get());
	const auto systemExports = ReadExports(system.get());
	ASSERT_FALSE(systemExports.empty());
	for (const auto& [ordinal, name] : systemExports)
	{
		const auto found = proxyExports.find(ordinal);
		ASSERT_NE(found, proxyExports.end()) << "the proxy has no ordinal " << ordinal << " (" << name << ")";
		EXPECT_EQ(found->second, name) << "ordinal " << ordinal;
	}
	// The proxy has one more export: the marker of the extender.
	ASSERT_EQ(proxyExports.size(), systemExports.size() + 1);
	EXPECT_EQ(std::ranges::count_if(proxyExports, [](const auto& item) { return item.second == "Ts4ModLoaderVersion"; }), 1);
	// The export with no name forwards by its ordinal.
	for (const auto& [ordinal, name] : systemExports)
	{
		if (name.empty())
		{
			EXPECT_NE(GetProcAddress(proxy.get(), MAKEINTRESOURCEA(ordinal)), nullptr) << "ordinal " << ordinal;
		}
	}
}

TEST(Proxy, ExportsTheMarkerOfTheExtender)
{
	const Module proxy(LoadLibraryW(ProxyPath().c_str()));
	ASSERT_TRUE(proxy);
	using VersionFunction = const char* (*)();
	const auto version = Find<VersionFunction>(proxy.get(), "Ts4ModLoaderVersion");
	ASSERT_NE(version, nullptr);
	EXPECT_EQ(std::string_view(version()), TS4_LOADER_VERSION);
}
