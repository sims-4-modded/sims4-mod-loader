/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <cstdint>

#include <string>
#include <string_view>

#include <core/HostTable.h>
#include <gtest/gtest.h>
#include <mod_api/Ts4ModApi.h>

#include "FakeServices.h"

using namespace ts4_mod::loader;
using ts4_mod::loader::test::FakeServices;
using ts4_mod::loader::test::Info;

namespace
{
void Call(void* /*user*/, const Ts4Call* /*call*/) {}

uint32_t Load(void* /*user*/, const char* /*json*/, Ts4Text* /*error*/)
{
	return 1;
}

uint32_t Query(void* user, const char* params, Ts4Text* result)
{
	const std::string answer = std::string(static_cast<const char*>(user)) + params;
	result->assign(result->context, answer.data(), answer.size());
	return 1;
}

void Assign(void* context, const char* utf8, size_t size)
{
	static_cast<std::string*>(context)->assign(utf8, size);
}
} // namespace

TEST(HostTable, GivesTheTextsAndLogsWithTheOwner)
{
	FakeServices services;
	const HostTable table(services, 7, Info());
	const auto& api = table.Api();
	EXPECT_EQ(api.apiVersion, TS4_MOD_API_VERSION);
	EXPECT_EQ(api.size, sizeof(Ts4HostApi));
	EXPECT_GE(api.size, TS4_HOST_API_FIRST_SIZE);
	EXPECT_STREQ(api.gameVersion, "1.128.90.1030");
	EXPECT_STREQ(api.userFolder, "user");
	EXPECT_STREQ(api.logFolder, "logs");
	api.log(api.context, TS4_LOG_INFO, "hello");
	EXPECT_EQ(services.log.back(), "7: hello");
	EXPECT_EQ(api.saveSettings(api.context, R"({"a": 1})"), 1u);
	EXPECT_EQ(services.saved.back(), R"({"a": 1})");
	api.showMessage(api.context, "Needs mod: on", 2.0);
	EXPECT_EQ(services.messages.back(), "Needs mod: on");
	EXPECT_EQ(api.postTask(api.context, nullptr, nullptr), 0u);
	EXPECT_EQ(api.addFunctionCallback(api.context, nullptr, TS4_HOOK_AFTER, "x", nullptr, nullptr), 0u);
	EXPECT_EQ(table.Refusals(), 2u);
}

TEST(HostTable, AddsAFunctionCallbackWithTheNormalSignature)
{
	FakeServices services;
	const HostTable table(services, 3, Info());
	const auto& api = table.Api();
	const Ts4FunctionDesc tick {
	    .module = TS4_MODULE_SIMULATION, .name = "Tick", .signature = "48 89 5c 24 ? 57", .parameterCount = 2};
	EXPECT_EQ(api.addFunctionCallback(api.context, &tick, TS4_HOOK_BEFORE, "count", &Call, nullptr), 1u);
	ASSERT_EQ(services.registry.FunctionCallbacks().size(), 1u);
	const auto& entry = services.registry.FunctionCallbacks()[0];
	EXPECT_EQ(entry.owner, 3u);
	EXPECT_EQ(entry.function.key.signature, "48 89 5C 24 ?? 57");
	EXPECT_EQ(entry.function.parameterCount, 2u);

	// A bad signature, a module that is not of the game, and too many parameters go to the log as refusals.
	Ts4FunctionDesc bad = tick;
	bad.signature = "48 8";
	EXPECT_EQ(api.addFunctionCallback(api.context, &bad, TS4_HOOK_BEFORE, "bad", &Call, nullptr), 0u);
	bad = tick;
	bad.module = "python37_x64.dll";
	EXPECT_EQ(api.addFunctionCallback(api.context, &bad, TS4_HOOK_BEFORE, "bad", &Call, nullptr), 0u);
	bad = tick;
	bad.parameterCount = 5;
	EXPECT_EQ(api.addFunctionCallback(api.context, &bad, TS4_HOOK_BEFORE, "bad", &Call, nullptr), 0u);
	bad = tick;
	bad.module = nullptr;
	EXPECT_EQ(api.addFunctionCallback(api.context, &bad, TS4_HOOK_BEFORE, "bad", &Call, nullptr), 0u);
	EXPECT_EQ(table.Refusals(), 4u);
	EXPECT_TRUE(services.Logged("the extender refuses the function callback 'bad'"));
	EXPECT_EQ(services.registry.FunctionCallbacks().size(), 1u);
}

TEST(HostTable, RefusesRegistrationsAfterTheInit)
{
	FakeServices services;
	HostTable table(services, 1, Info());
	const auto& api = table.Api();
	EXPECT_EQ(api.addSettings(api.context, "needs", &Load, nullptr), 1u);
	table.Close();
	EXPECT_EQ(api.addSettings(api.context, "late", &Load, nullptr), 0u);
	EXPECT_TRUE(services.Logged("only in Ts4ModInit"));
	// The log and the tasks still work.
	EXPECT_EQ(api.postTask(api.context, [](void*) {}, nullptr), 1u);
	EXPECT_EQ(services.tasks.size(), 1u);
}

TEST(HostTable, ListsAndCallsTheQueriesOfAllOwners)
{
	FakeServices services;
	services.registry.SetOwnerName(2, "Needs");
	const HostTable table(services, 2, Info());
	const auto& api = table.Api();
	static constexpr std::string_view k_Prefix = "answer:";
	EXPECT_EQ(api.addQuery(api.context, "sims.needs", "The needs.", &Query, const_cast<char*>(k_Prefix.data())), 1u);
	std::string text;
	Ts4Text target {.context = &text, .assign = &Assign};
	api.listQueries(api.context, &target);
	EXPECT_EQ(text, R"([{"help":"The needs.","mod":"Needs","name":"sims.needs"}])");
	EXPECT_EQ(api.callQuery(api.context, "sims.needs", R"({"a":1})", &target), 1u);
	EXPECT_EQ(text, R"(answer:{"a":1})");
	EXPECT_EQ(api.callQuery(api.context, "sims.needs", nullptr, &target), 1u);
	EXPECT_EQ(text, "answer:{}");
	EXPECT_EQ(api.callQuery(api.context, "sims.other", nullptr, &target), 0u);
	EXPECT_EQ(text, "the query 'sims.other' is not known");
}
