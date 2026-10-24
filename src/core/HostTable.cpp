/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <exception>
#include <format>
#include <utility>

#include <core/HostTable.h>
#include <nlohmann/json.hpp>

namespace ts4_mod::loader
{

namespace
{
[[nodiscard]] HostTable& TableOf(void* context)
{
	return *static_cast<HostTable*>(context);
}

[[nodiscard]] std::string Copy(const char* text)
{
	return text == nullptr ? std::string() : std::string(text);
}

/// Run `work` and turn an exception into a refusal. No exception may go back into the mod.
template <typename Work>
uint32_t Guard(HostTable& table, std::string_view what, Work&& work) noexcept
{
	try
	{
		return work();
	}
	catch (const std::exception& error)
	{
		return table.Refuse(what, ts4::Error {error.what()});
	}
	catch (...)
	{
		return table.Refuse(what, ts4::Error {"an unknown exception"});
	}
}

/// The registrations are open only during the init of the mod.
template <typename Work>
uint32_t Register(void* context, std::string_view what, Work&& work) noexcept
{
	if (context == nullptr)
	{
		return 0;
	}
	auto& table = TableOf(context);
	return Guard(table, what, [&]() -> uint32_t {
		if (!table.Open())
		{
			return table.Refuse(what, ts4::MakeError("the mod can register only in Ts4ModInit"));
		}
		const ts4::Result<void> result = work(table);
		return result ? 1u : table.Refuse(what, result.GetError());
	});
}

void Log(void* context, Ts4LogLevel level, const char* text)
{
	if (context == nullptr || text == nullptr)
	{
		return;
	}
	auto& table = TableOf(context);
	try
	{
		table.Services().Log(table.Owner(), level, text);
	}
	catch (...)
	{
		// The log has no other place for its own error.
	}
}

uint32_t AddFunctionCallback(void* context, const Ts4FunctionDesc* function, Ts4HookPhase phase, const char* name,
                             Ts4CallFn callback, void* user)
{
	return Register(context, std::format("the function callback '{}'", Copy(name)), [&](HostTable& table) -> ts4::Result<void> {
		if (function == nullptr)
		{
			return ts4::MakeError("no function");
		}
		auto target = MakeFunctionTarget(Copy(function->module), Copy(function->name), Copy(function->signature),
		                                 function->parameterCount);
		if (!target)
		{
			return target.GetError();
		}
		return table.Services().Registry().AddFunctionCallback(table.Owner(), std::move(*target), phase, Copy(name), callback,
		                                                       user);
	});
}

uint32_t PostTask(void* context, Ts4TaskFn task, void* user)
{
	if (context == nullptr)
	{
		return 0;
	}
	auto& table = TableOf(context);
	return Guard(table, "a task", [&]() -> uint32_t {
		if (task == nullptr)
		{
			return table.Refuse("a task", ts4::MakeError("no function"));
		}
		const auto result = table.Services().PostTask(table.Owner(), task, user);
		return result ? 1u : table.Refuse("a task", result.GetError());
	});
}

uint32_t AddSettings(void* context, const char* section, Ts4SettingsFn load, void* user)
{
	return Register(context, std::format("the settings section '{}'", Copy(section)), [&](HostTable& table) {
		return table.Services().Registry().AddSettings(table.Owner(), Copy(section), load, user);
	});
}

uint32_t SaveSettings(void* context, const char* patch)
{
	if (context == nullptr)
	{
		return 0;
	}
	auto& table = TableOf(context);
	return Guard(table, "a change of mods.json", [&]() -> uint32_t {
		const auto result = table.Services().SaveSettings(table.Owner(), patch == nullptr ? "" : patch);
		return result ? 1u : table.Refuse("a change of mods.json", result.GetError());
	});
}

uint32_t AddField(void* context, const Ts4FieldDesc* field, Ts4FieldGetFn get, Ts4FieldSetFn set, void* user)
{
	const auto what = field == nullptr ? std::string("a field") : std::format("the field '{}'", Copy(field->key));
	return Register(context, what, [&](HostTable& table) -> ts4::Result<void> {
		if (field == nullptr)
		{
			return ts4::MakeError("no description");
		}
		auto id = table.Services().Registry().AddField(table.Owner(), *field, get, set, user);
		if (!id)
		{
			return id.GetError();
		}
		return {};
	});
}

uint32_t AddAction(void* context, const Ts4ActionDesc* action, Ts4TaskFn run, Ts4EnabledFn enabled, void* user)
{
	const auto what = action == nullptr
	                      ? std::string("an action")
	                      : std::format("the action '{}'", Copy(action->hotkey != nullptr ? action->hotkey : action->label));
	return Register(context, what, [&](HostTable& table) -> ts4::Result<void> {
		if (action == nullptr)
		{
			return ts4::MakeError("no description");
		}
		auto id = table.Services().Registry().AddAction(table.Owner(), *action, run, enabled, user);
		if (!id)
		{
			return id.GetError();
		}
		return {};
	});
}

uint32_t AddReadout(void* context, const char* menu, const char* group, Ts4ReadFn read, void* user)
{
	return Register(context, std::format("the readout '{}'", Copy(group)), [&](HostTable& table) -> ts4::Result<void> {
		auto id = table.Services().Registry().AddReadout(table.Owner(), menu, group, read, user);
		if (!id)
		{
			return id.GetError();
		}
		return {};
	});
}

uint32_t SetStatusReader(void* context, Ts4ReadFn read, void* user)
{
	return Register(context, "the status reader",
	                [&](HostTable& table) { return table.Services().Registry().SetStatusReader(table.Owner(), read, user); });
}

uint32_t AddQuery(void* context, const char* name, const char* help, Ts4QueryFn query, void* user)
{
	return Register(context, std::format("the query '{}'", Copy(name)), [&](HostTable& table) {
		return table.Services().Registry().AddQuery(table.Owner(), Copy(name), Copy(help), query, user);
	});
}

void Give(Ts4Text* text, std::string_view value)
{
	if (text != nullptr && text->assign != nullptr)
	{
		text->assign(text->context, value.data(), value.size());
	}
}

void ListQueries(void* context, Ts4Text* list)
{
	if (context == nullptr)
	{
		return;
	}
	auto& table = TableOf(context);
	(void)Guard(table, "the list of the queries", [&]() -> uint32_t {
		const auto& registry = table.Services().Registry();
		auto items = nlohmann::json::array();
		for (const auto& query : registry.Queries())
		{
			items.push_back({{"name", query.name}, {"help", query.help}, {"mod", registry.OwnerName(query.owner)}});
		}
		Give(list, items.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
		return 1;
	});
}

uint32_t CallQuery(void* context, const char* name, const char* params, Ts4Text* result)
{
	if (context == nullptr)
	{
		return 0;
	}
	auto& table = TableOf(context);
	// An unknown query and an error of a query go to the caller, not to the log.
	try
	{
		const auto* query = table.Services().Registry().FindQuery(Copy(name));
		if (query == nullptr)
		{
			Give(result, std::format("the query '{}' is not known", Copy(name)));
			return 0;
		}
		const auto parameters = params == nullptr ? std::string("{}") : std::string(params);
		auto answer = table.Services().CallQuery(*query, parameters);
		if (!answer)
		{
			Give(result, answer.GetError().message);
			return 0;
		}
		Give(result, *answer);
		return 1;
	}
	catch (const std::exception& error)
	{
		Give(result, error.what());
	}
	catch (...)
	{
		Give(result, "an unknown exception in the extender");
	}
	return 0;
}

void ShowMessage(void* context, const char* text, double seconds)
{
	if (context == nullptr || text == nullptr)
	{
		return;
	}
	auto& table = TableOf(context);
	(void)Guard(table, "a message", [&]() -> uint32_t {
		table.Services().ShowMessage(table.Owner(), text, seconds);
		return 1;
	});
}
} // namespace

ts4::Result<std::string> RunQueryFunction(const QueryEntry& query, const std::string& params)
{
	std::string text;
	Ts4Text target {.context = &text, .assign = [](void* context, const char* utf8, size_t size) {
		                if (utf8 != nullptr)
		                {
			                static_cast<std::string*>(context)->assign(utf8, size);
		                }
	                }};
	if (query.query(query.user, params.c_str(), &target) == 0)
	{
		return ts4::MakeError("{}", text.empty() ? std::format("the query '{}' failed", query.name) : text);
	}
	return text;
}

HostTable::HostTable(HostServices& services, OwnerId owner, HostInfo info)
    : _services(services)
    , _owner(owner)
    , _info(std::move(info))
{
	_api.apiVersion = TS4_MOD_API_VERSION;
	_api.size = sizeof(Ts4HostApi);
	_api.context = this;
	_api.loaderVersion = _info.loaderVersion.c_str();
	_api.gameVersion = _info.gameVersion.c_str();
	_api.gameFolder = _info.gameFolder.c_str();
	_api.userFolder = _info.userFolder.c_str();
	_api.settingsFolder = _info.settingsFolder.c_str();
	_api.logFolder = _info.logFolder.c_str();
	_api.log = &Log;
	_api.addFunctionCallback = &AddFunctionCallback;
	_api.postTask = &PostTask;
	_api.addSettings = &AddSettings;
	_api.saveSettings = &SaveSettings;
	_api.addField = &AddField;
	_api.addAction = &AddAction;
	_api.addReadout = &AddReadout;
	_api.setStatusReader = &SetStatusReader;
	_api.showMessage = &ShowMessage;
	_api.addQuery = &AddQuery;
	_api.listQueries = &ListQueries;
	_api.callQuery = &CallQuery;
}

uint32_t HostTable::Refuse(std::string_view what, const ts4::Error& error)
{
	++_refusals;
	try
	{
		_services.Log(_owner, TS4_LOG_ERROR, std::format("the extender refuses {}: {}", what, error.message));
	}
	catch (...)
	{
		// The log has no other place for its own error.
	}
	return 0;
}

} // namespace ts4_mod::loader
