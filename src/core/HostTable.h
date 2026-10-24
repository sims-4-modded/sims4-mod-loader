/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <atomic>
#include <string>
#include <string_view>

#include <common/Result.h>
#include <core/ModRegistry.h>
#include <mod_api/Ts4ModApi.h>

namespace ts4_mod::loader
{

/// The work of the extender behind the table of a mod. The game side gives the real one; the tests give a fake.
class HostServices
{
public:
	virtual ~HostServices() = default;

	/// The registrations of all mods.
	[[nodiscard]] virtual ModRegistry& Registry() = 0;
	virtual void Log(OwnerId owner, Ts4LogLevel level, std::string_view text) = 0;
	[[nodiscard]] virtual ts4::Result<void> PostTask(OwnerId owner, Ts4TaskFn task, void* user) = 0;
	[[nodiscard]] virtual ts4::Result<void> SaveSettings(OwnerId owner, std::string_view patch) = 0;
	virtual void ShowMessage(OwnerId owner, std::string_view text, double seconds) = 0;
	/// Call a query with the JSON text of its parameters, and return the JSON text of its answer or the error.
	[[nodiscard]] virtual ts4::Result<std::string> CallQuery(const QueryEntry& query, const std::string& params) = 0;
};

/// Call the function of a query and return its answer or its error. It does not guard the call: the caller does.
[[nodiscard]] ts4::Result<std::string> RunQueryFunction(const QueryEntry& query, const std::string& params);

/// The texts of the table. They stay valid as long as the table.
struct HostInfo
{
	std::string loaderVersion;
	std::string gameVersion; ///< For example "1.128.90.1030".
	std::string gameFolder;
	std::string userFolder; ///< Documents\Electronic Arts\The Sims 4.
	std::string settingsFolder;
	std::string logFolder;
};

/// The table (`Ts4HostApi`) that the extender gives to one mod. Each function of the table finds this object through
/// `context`, catches every exception, and writes each refusal to the log with the name of the mod.
///
/// The registrations are open only during `Ts4ModInit`: after `Close`, the table refuses them. The log, the tasks, the
/// changes of mods.json, the messages and the calls of the queries work until the process ends.
class HostTable
{
public:
	HostTable(HostServices& services, OwnerId owner, HostInfo info);
	HostTable(const HostTable&) = delete;
	HostTable& operator=(const HostTable&) = delete;

	[[nodiscard]] const Ts4HostApi& Api() const { return _api; }
	[[nodiscard]] OwnerId Owner() const { return _owner; }

	/// Refuse all registrations from now on.
	void Close() { _open = false; }
	[[nodiscard]] bool Open() const { return _open; }

	/// The count of the registrations that the table refused.
	[[nodiscard]] size_t Refusals() const { return _refusals; }

	/// The functions of the table use the services.
	[[nodiscard]] HostServices& Services() const { return _services; }
	/// Log a refusal of a registration. Return 0, the result of a refused registration.
	uint32_t Refuse(std::string_view what, const ts4::Error& error);

private:
	HostServices& _services;
	OwnerId _owner;
	HostInfo _info;
	Ts4HostApi _api {};
	std::atomic<bool> _open = true;
	std::atomic<size_t> _refusals = 0;
};

} // namespace ts4_mod::loader
