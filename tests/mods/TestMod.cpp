/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

// A small DLL mod for the tests of the extender. TS4_TEST_MOD_KIND selects what it does:
// 1: a correct mod that registers a settings section, a field, an action, a function callback and a query.
// 2: a mod whose init registers and then returns 0.
// 3: a mod of another interface version.
// 4: a mod whose init causes an access violation.
// 5: a correct mod with the same id as kind 1.
// 6: a mod with an id that is not permitted.

#include <cstdint>

#include <memory>
#include <string>

#include <mod_api/Ts4ModApi.h>
#include <mod_sdk/ModHost.h>

#ifndef TS4_TEST_MOD_KIND
#error "TS4_TEST_MOD_KIND is not set"
#endif

namespace
{
/// The state of the mod, from the init to the shutdown.
struct State
{
	std::unique_ptr<ts4_mod::sdk::ModHost> host;
	double speed = 1.0;
	int calls = 0;
	int actions = 0;
};

/// The one state object of the DLL.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): the exports cannot get the state in another way.
std::unique_ptr<State> s_state;

[[nodiscard]] const char* ModId()
{
#if TS4_TEST_MOD_KIND == 6
	return "bad id!";
#elif TS4_TEST_MOD_KIND == 5 || TS4_TEST_MOD_KIND == 1
	return "test_mod";
#else
	return "test_mod_other";
#endif
}

uint32_t Init(const Ts4HostApi* api, uint32_t apiVersion)
{
	auto host = ts4_mod::sdk::ModHost::Create(api, apiVersion, ModId());
	if (!host)
	{
		return 0;
	}
	s_state = std::make_unique<State>();
	auto& state = *s_state;
	state.host = std::move(*host);
	bool ok = state.host->AddSettings("testmod", [](const nlohmann::json& object) -> ts4::Result<void> {
		s_state->speed = object.value("speed", 1.0);
		return {};
	});
	const Ts4FieldDesc field {.menu = nullptr,
	                          .group = "Test",
	                          .section = "testmod",
	                          .key = "speed",
	                          .label = "Speed",
	                          .help = "A test value.",
	                          .kind = TS4_FIELD_NUMBER,
	                          .min = 0.0,
	                          .max = 10.0,
	                          .step = 0.1,
	                          .format = "%.1f",
	                          .logarithmic = 0,
	                          .restartOnly = 0,
	                          .choices = nullptr,
	                          .choiceCount = 0};
	ok = ok && state.host->AddField(
	               field, {.get =
	                           [] {
		                           return ts4_mod::sdk::FieldState {
		                               .value = s_state->speed, .range = {.min = 0.0, .max = 10.0}, .restartPending = false};
	                           },
	                       .set = [](const ts4_mod::settings::FieldValue& value, bool /*save*/) -> ts4::Result<std::string> {
		                       s_state->speed = std::get<double>(value);
		                       return std::string("Speed changed");
	                       }});
	const Ts4ActionDesc action {.menu = nullptr,
	                            .group = "Test",
	                            .label = "Do it",
	                            .help = nullptr,
	                            .hotkey = "testAction",
	                            .defaultHotkey = "Ctrl+Shift+F2"};
	ok = ok && state.host->AddAction(action, [] { ++s_state->actions; });
	ok = ok && state.host->AddFunctionCallback(
	               {.module = TS4_MODULE_SIMULATION, .name = "TestTick", .signature = "48 89 5c 24 ? 57", .parameterCount = 1},
	               TS4_HOOK_AFTER, "count", [](const Ts4Call& /*call*/) { ++s_state->calls; });
	ok = ok && state.host->AddQuery("test.speed", "The speed of the test mod.", [](const nlohmann::json& params) {
		return nlohmann::json {{"speed", s_state->speed + params.value("add", 0.0)}};
	});
#if TS4_TEST_MOD_KIND == 2
	(void)ok;
	return 0;
#elif TS4_TEST_MOD_KIND == 4
	(void)ok;
	volatile int* pointer = nullptr;
	*pointer = 1;
	return 1;
#else
	return ok ? 1 : 0;
#endif
}
} // namespace

extern "C" {
__declspec(dllexport) uint32_t Ts4ModApiVersion()
{
#if TS4_TEST_MOD_KIND == 3
	return TS4_MOD_API_VERSION + 1;
#else
	return TS4_MOD_API_VERSION;
#endif
}

__declspec(dllexport) const Ts4ModInfo* Ts4ModGetInfo()
{
	static const Ts4ModInfo info {.id = ModId(), .name = "Test mod", .version = "1.0"};
	return &info;
}

__declspec(dllexport) uint32_t Ts4ModInit(const Ts4HostApi* api, uint32_t apiVersion)
{
	try
	{
		return Init(api, apiVersion);
	}
	catch (...)
	{
		return 0;
	}
}

__declspec(dllexport) void Ts4ModShutdown()
{
	s_state.reset();
}

/// For the test: the count of the calls of the function callback and of the action.
__declspec(dllexport) int TestModCalls()
{
	return s_state ? (s_state->calls * 1000) + s_state->actions : -1;
}
}
