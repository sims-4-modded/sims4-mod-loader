/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include "game/loader/LoaderLog.h"

#include <cstring>

#include <algorithm>
#include <array>
#include <memory>
#include <system_error>

#include <windows.h>

namespace ts4_mod::loader
{

namespace
{
struct HandleCloser
{
	void operator()(HANDLE handle) const
	{
		if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(handle);
		}
	}
};
using FileHandle = std::unique_ptr<void, HandleCloser>;

/// Open the log file to add lines at its end. Other processes can read and write the file at the same time.
[[nodiscard]] FileHandle OpenForAppend(const wchar_t* path)
{
	const auto handle = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
	                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	return FileHandle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

void WriteAll(HANDLE file, std::string_view text)
{
	DWORD written = 0;
	WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}
} // namespace

std::string_view LinePrefix(std::span<char> buffer)
{
	SYSTEMTIME time {};
	GetLocalTime(&time);
	const auto result = std::format_to_n(buffer.data(), static_cast<std::ptrdiff_t>(buffer.size()),
	                                     "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{}] ", time.wYear, time.wMonth, time.wDay,
	                                     time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetCurrentProcessId());
	const auto size = std::min(static_cast<std::size_t>(result.size), buffer.size());
	return {buffer.data(), size};
}

LoaderLog::LoaderLog(std::filesystem::path path)
    : _path(std::move(path))
{
	std::error_code error;
	std::filesystem::create_directories(_path.parent_path(), error);
}

void LoaderLog::RotateIfLarge()
{
	const std::lock_guard lock(_mutex);
	std::error_code error;
	if (std::filesystem::file_size(_path, error) > k_MaxSize && !error)
	{
		auto old = _path;
		old.replace_extension(".old.log");
		std::filesystem::rename(_path, old, error);
	}
}

void LoaderLog::Write(std::string_view text)
{
	std::array<char, 64> prefix {};
	std::string line(LinePrefix(prefix));
	line.append(text);
	line.push_back('\n');
	const std::lock_guard lock(_mutex);
	if (const auto file = OpenForAppend(_path.c_str()))
	{
		WriteAll(file.get(), line);
	}
}

void WriteLineAtExit(const wchar_t* path, std::string_view text)
{
	std::array<char, 1024> line {};
	const auto prefix = LinePrefix(line);
	const auto room = line.size() - prefix.size() - 1;
	const auto size = std::min(text.size(), room);
	std::memcpy(line.data() + prefix.size(), text.data(), size);
	line[prefix.size() + size] = '\n';
	if (const auto file = OpenForAppend(path))
	{
		WriteAll(file.get(), std::string_view(line.data(), prefix.size() + size + 1));
	}
}

} // namespace ts4_mod::loader
