/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <array>
#include <filesystem>
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
	return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}
} // namespace

// The test program is not TS4_x64.exe, so the proxy only forwards the calls and starts no thread.
TEST(Proxy, ForwardsTheCallsToTheSystemVersionDll)
{
	const Module proxy(LoadLibraryW(ProxyPath().c_str()));
	ASSERT_TRUE(proxy) << "the proxy did not load: error " << GetLastError();
	const Module system(LoadLibraryW(SystemFile(L"version.dll").c_str()));
	ASSERT_TRUE(system);
	ASSERT_NE(proxy.get(), system.get());

	using SizeFunction = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
	const auto proxySize = Find<SizeFunction>(proxy.get(), "GetFileVersionInfoSizeW");
	const auto systemSize = Find<SizeFunction>(system.get(), "GetFileVersionInfoSizeW");
	ASSERT_NE(proxySize, nullptr);
	ASSERT_NE(systemSize, nullptr);
	const auto kernel = SystemFile(L"kernel32.dll");
	DWORD handle = 0;
	const auto size = proxySize(kernel.c_str(), &handle);
	EXPECT_GT(size, 0U);
	EXPECT_EQ(size, systemSize(kernel.c_str(), &handle));

	using LanguageFunction = DWORD(WINAPI*)(DWORD, LPWSTR, DWORD);
	const auto language = Find<LanguageFunction>(proxy.get(), "VerLanguageNameW");
	ASSERT_NE(language, nullptr);
	std::array<wchar_t, 128> name {};
	EXPECT_GT(language(0x0409, name.data(), static_cast<DWORD>(name.size())), 0U);
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
