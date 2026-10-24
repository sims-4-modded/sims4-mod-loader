/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <core/HostTable.h>
#include <core/Sha256.h>
#include <dlls/ModDllLoader.h>
#include <gtest/gtest.h>
#include <windows.h>

#include "FakeServices.h"

using namespace ts4_mod::loader;
using ts4_mod::loader::test::FakeServices;
using ts4_mod::loader::test::Info;

namespace
{

[[nodiscard]] std::filesystem::path TestMod(const char* name)
{
	return std::filesystem::path(TS4_TEST_MODS_DIR) / name;
}

[[nodiscard]] std::filesystem::path SystemDll(const wchar_t* name)
{
	std::array<wchar_t, MAX_PATH> system {};
	const auto length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
	return std::filesystem::path(std::wstring(system.data(), length)) / name;
}

} // namespace

TEST(ModDllLoader, LoadsACorrectModAndKeepsItsRegistrations)
{
	FakeServices services;
	ModDllLoader loader(services, Info());
	const auto& mod = loader.Load(TestMod("ts4_test_mod_good.dll"));
	ASSERT_EQ(mod.state, ModDllState::Loaded) << mod.error;
	EXPECT_EQ(mod.id, "test_mod");
	EXPECT_EQ(mod.name, "Test mod");
	EXPECT_EQ(mod.version, "1.0");
	EXPECT_EQ(mod.sha256, *Sha256OfFile(TestMod("ts4_test_mod_good.dll")));
	EXPECT_EQ(mod.sha256.size(), 64u);
	ASSERT_EQ(services.registry.Settings().size(), 1u);
	ASSERT_EQ(services.registry.Fields().size(), 1u);
	ASSERT_EQ(services.registry.Actions().size(), 1u);
	ASSERT_EQ(services.registry.FunctionCallbacks().size(), 1u);
	ASSERT_EQ(services.registry.Queries().size(), 1u);
	EXPECT_EQ(services.registry.FunctionCallbacks()[0].function.key.Text(), "Simulation_x64.dll [48 89 5C 24 ?? 57]");
	EXPECT_TRUE(services.Logged("SHA-256 " + mod.sha256));

	// The extender calls the mod through the registry, as the game side does.
	const auto& settings = services.registry.Settings()[0];
	EXPECT_EQ(settings.load(settings.user, R"({"speed": 4.5})", nullptr), 1u);
	const auto& field = services.registry.Fields()[0];
	Ts4FieldState state {};
	field.get(field.user, &state);
	EXPECT_DOUBLE_EQ(state.value.number, 4.5);
	const auto& callback = services.registry.FunctionCallbacks()[0];
	const Ts4Call call {.arguments = {}, .result = 0};
	callback.call(callback.user, &call);
	callback.call(callback.user, &call);
	const auto& action = services.registry.Actions()[0];
	action.run(action.user);
	using Calls = int (*)();
	const auto calls = reinterpret_cast<Calls>(GetProcAddress(static_cast<HMODULE>(mod.module.get()), "TestModCalls"));
	ASSERT_NE(calls, nullptr);
	EXPECT_EQ(calls(), 2001);

	// The inspector lists the query of the mod and calls it through the table.
	const auto& api = mod.table->Api();
	ASSERT_EQ(TS4_HOST_API_HAS(&api, callQuery), 1);
	std::string text;
	Ts4Text target {.context = &text, .assign = [](void* context, const char* utf8, size_t size) {
		                static_cast<std::string*>(context)->assign(utf8, size);
	                }};
	api.listQueries(api.context, &target);
	EXPECT_EQ(text, R"([{"help":"The speed of the test mod.","mod":"Test mod","name":"test.speed"}])");
	EXPECT_EQ(api.callQuery(api.context, "test.speed", R"({"add": 1.5})", &target), 1u);
	EXPECT_EQ(text, R"({"speed":6.0})");
	EXPECT_EQ(api.callQuery(api.context, "test.speed", R"({"add": "x"})", &target), 0u);
	EXPECT_NE(text.find("type must be number"), std::string::npos);
	EXPECT_EQ(api.callQuery(api.context, "test.other", nullptr, &target), 0u);
	EXPECT_EQ(text, "the query 'test.other' is not known");

	// After the init, the table refuses new registrations.
	EXPECT_EQ(api.addSettings(api.context, "late", settings.load, nullptr), 0u);
	EXPECT_TRUE(services.Logged("only in Ts4ModInit"));
	loader.ShutdownAll();
	EXPECT_EQ(calls(), -1);
}

TEST(ModDllLoader, UnloadsAModWhoseInitFailsAndRemovesItsRegistrations)
{
	FakeServices services;
	ModDllLoader loader(services, Info());
	const auto& mod = loader.Load(TestMod("ts4_test_mod_init_fails.dll"));
	EXPECT_EQ(mod.state, ModDllState::Refused);
	EXPECT_NE(mod.error.find("returned 0"), std::string::npos);
	EXPECT_EQ(mod.module, nullptr);
	EXPECT_EQ(mod.sha256.size(), 64u);
	EXPECT_TRUE(services.registry.Failed(mod.owner));
	EXPECT_TRUE(services.registry.Settings().empty());
	EXPECT_TRUE(services.registry.Fields().empty());
	EXPECT_TRUE(services.registry.FunctionCallbacks().empty());
	EXPECT_TRUE(services.registry.Queries().empty());
	// The section of the failed mod is free for the next mod.
	EXPECT_EQ(loader.Load(TestMod("ts4_test_mod_good.dll")).state, ModDllState::Loaded);
}

TEST(ModDllLoader, RefusesAModOfAnotherInterfaceVersion)
{
	FakeServices services;
	ModDllLoader loader(services, Info());
	const auto& mod = loader.Load(TestMod("ts4_test_mod_old.dll"));
	EXPECT_EQ(mod.state, ModDllState::Refused);
	EXPECT_NE(mod.error.find("interface version 2"), std::string::npos);
	EXPECT_EQ(mod.module, nullptr);
	EXPECT_TRUE(services.registry.Settings().empty());
}

TEST(ModDllLoader, CatchesAnAccessViolationInTheInit)
{
	FakeServices services;
	ModDllLoader loader(services, Info());
	const auto& mod = loader.Load(TestMod("ts4_test_mod_crash.dll"));
	EXPECT_EQ(mod.state, ModDllState::Failed);
	EXPECT_NE(mod.error.find("access violation in Ts4ModInit"), std::string::npos);
	EXPECT_TRUE(services.registry.Failed(mod.owner));
	EXPECT_TRUE(services.registry.Settings().empty());
	EXPECT_TRUE(services.registry.FunctionCallbacks().empty());
	// The code of the mod can still be in use, so the DLL stays in memory with no handle of the extender.
	EXPECT_EQ(mod.module, nullptr);
	EXPECT_NE(GetModuleHandleW(TestMod("ts4_test_mod_crash.dll").c_str()), nullptr);
}

TEST(ModDllLoader, MarkFailedStopsTheCallsToALoadedMod)
{
	FakeServices services;
	ModDllLoader loader(services, Info());
	// The DLL stays in memory after the mark, so this test uses a DLL that no other test unloads.
	const auto& mod = loader.Load(TestMod("ts4_test_mod_same_id.dll"));
	ASSERT_EQ(mod.state, ModDllState::Loaded);
	loader.MarkFailed(mod.owner, "an access violation in a callback");
	EXPECT_EQ(mod.state, ModDllState::Failed);
	EXPECT_EQ(mod.error, "an access violation in a callback");
	EXPECT_EQ(loader.Find(mod.owner), &mod);
	EXPECT_EQ(loader.Find(99), nullptr);
}

TEST(ModDllLoader, RefusesASecondModWithTheSameIdAndABadId)
{
	FakeServices services;
	ModDllLoader loader(services, Info());
	EXPECT_EQ(loader.Load(TestMod("ts4_test_mod_good.dll")).state, ModDllState::Loaded);
	const auto& same = loader.Load(TestMod("ts4_test_mod_same_id.dll"));
	EXPECT_EQ(same.state, ModDllState::Refused);
	EXPECT_NE(same.error.find("same id"), std::string::npos);
	const auto& bad = loader.Load(TestMod("ts4_test_mod_bad_id.dll"));
	EXPECT_EQ(bad.state, ModDllState::Refused);
	EXPECT_NE(bad.error.find("'bad id!'"), std::string::npos);
	EXPECT_EQ(loader.Mods().size(), 3u);
	EXPECT_EQ(services.registry.Settings().size(), 1u);
}

TEST(ModDllLoader, TurnsOffAModByItsIdBeforeItsInit)
{
	FakeServices services;
	ModDllLoader loader(services, Info());
	const std::vector<std::string> disabled {"test_mod"};
	const auto& mod = loader.Load(TestMod("ts4_test_mod_good.dll"), disabled);
	EXPECT_EQ(mod.state, ModDllState::Disabled);
	EXPECT_EQ(mod.error, "mods.disabled in mods.json");
	EXPECT_EQ(mod.id, "test_mod");
	EXPECT_EQ(mod.module, nullptr);
	EXPECT_EQ(mod.table, nullptr);
	EXPECT_TRUE(services.registry.Settings().empty());
	EXPECT_FALSE(services.registry.Failed(mod.owner));
	// A mod with another id loads.
	EXPECT_EQ(loader.Load(TestMod("ts4_test_mod_init_fails.dll"), disabled).state, ModDllState::Refused);
}

TEST(ModDllLoader, RefusesADllThatIsNotAModAndAFileThatIsMissing)
{
	FakeServices services;
	ModDllLoader loader(services, Info());
	const auto& plain = loader.Load(SystemDll(L"version.dll"));
	EXPECT_EQ(plain.state, ModDllState::Refused);
	EXPECT_NE(plain.error.find("not a mod"), std::string::npos);
	EXPECT_EQ(plain.sha256.size(), 64u);
	const auto& missing = loader.Load(TestMod("no_such_mod.dll"));
	EXPECT_EQ(missing.state, ModDllState::Refused);
	EXPECT_TRUE(missing.sha256.empty());
	const auto& off = loader.AddDisabled(TestMod("ts4_test_mod_good.dll"), "modDlls.disabled in mods.json");
	EXPECT_EQ(off.state, ModDllState::Disabled);
	EXPECT_EQ(off.sha256.size(), 64u);
	EXPECT_EQ(ts4_mod::mods::ModDllStateName(off.state), "off");
}

TEST(ModDllLoader, DescribesEachDllWithItsChecksumAndState)
{
	FakeServices services;
	ModDllLoader loader(services, Info());
	(void)loader.Load(TestMod("ts4_test_mod_good.dll"));
	(void)loader.AddDisabled(TestMod("ts4_test_mod_old.dll"), "modDlls.disabled in mods.json");
	const auto list = loader.Describe();
	ASSERT_EQ(list.size(), 2u);
	EXPECT_EQ(list[0]["file"], "ts4_test_mod_good.dll");
	EXPECT_EQ(list[0]["state"], "loaded");
	EXPECT_EQ(list[0]["id"], "test_mod");
	EXPECT_EQ(list[0]["sha256"], *Sha256OfFile(TestMod("ts4_test_mod_good.dll")));
	EXPECT_EQ(list[1]["state"], "off");
	EXPECT_EQ(list[1]["error"], "modDlls.disabled in mods.json");
}

TEST(ModDllLoader, UnloadsTheModsAndClearsTheirRegistrationsWhenItGoesOutOfScope)
{
	FakeServices services;
	{
		ModDllLoader loader(services, Info());
		ASSERT_EQ(loader.Load(TestMod("ts4_test_mod_good.dll")).state, ModDllState::Loaded);
		EXPECT_EQ(services.registry.Settings().size(), 1u);
	}
	EXPECT_TRUE(services.registry.Settings().empty());
	EXPECT_TRUE(services.registry.FunctionCallbacks().empty());
	EXPECT_EQ(GetModuleHandleW(TestMod("ts4_test_mod_good.dll").c_str()), nullptr);
}

TEST(ModDllLoader, FindsTheDllsOfAFolderInTheOrderOfTheirNames)
{
	const auto files = FindModDlls(TS4_TEST_MODS_DIR);
	ASSERT_GE(files.size(), 6u);
	EXPECT_TRUE(std::ranges::is_sorted(files, {}, [](const auto& path) { return path.filename().wstring(); }));
	EXPECT_TRUE(std::ranges::all_of(files, [](const auto& path) { return path.extension() == ".dll"; }));
	EXPECT_TRUE(FindModDlls(TestMod("no_such_folder")).empty());
}
