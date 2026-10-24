/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/HostTable.h>

namespace ts4_mod::loader::test
{

/// The work of the extender for the tests: the registry, the log in memory and the tasks in a list.
class FakeServices final: public HostServices
{
public:
	ModRegistry& Registry() override { return registry; }
	void Log(OwnerId owner, Ts4LogLevel /*level*/, std::string_view text) override
	{
		log.push_back(std::format("{}: {}", owner, text));
	}
	ts4::Result<void> PostTask(OwnerId /*owner*/, Ts4TaskFn task, void* user) override
	{
		tasks.emplace_back(task, user);
		return {};
	}
	ts4::Result<void> SaveSettings(OwnerId /*owner*/, std::string_view patch) override
	{
		saved.emplace_back(patch);
		return {};
	}
	void ShowMessage(OwnerId /*owner*/, std::string_view text, double /*seconds*/) override { messages.emplace_back(text); }
	ts4::Result<std::string> CallQuery(const QueryEntry& query, const std::string& params) override
	{
		return RunQueryFunction(query, params);
	}

	[[nodiscard]] bool Logged(std::string_view part) const
	{
		return std::ranges::any_of(log, [&](const std::string& line) { return line.find(part) != std::string::npos; });
	}

	ModRegistry registry;
	std::vector<std::string> log;
	std::vector<std::pair<Ts4TaskFn, void*>> tasks;
	std::vector<std::string> saved;
	std::vector<std::string> messages;
};

/// The texts of a table for the tests.
[[nodiscard]] inline HostInfo Info()
{
	return HostInfo {.loaderVersion = "test",
	                 .gameVersion = "1.128.90.1030",
	                 .gameFolder = "game",
	                 .userFolder = "user",
	                 .settingsFolder = "settings",
	                 .logFolder = "logs"};
}

} // namespace ts4_mod::loader::test
