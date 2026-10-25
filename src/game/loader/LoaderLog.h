/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <cstdint>

#include <filesystem>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace ts4_mod::loader
{

/// The log file of the extender (`loader.log`). Each process of the game adds its lines at the end of the same file.
/// Each line has the local time and the process id, so that the log tells the lines of the two processes apart.
/// Each line is one write to the file in append mode, so the lines of two processes do not mix.
class LoaderLog
{
public:
	/// The largest size of the log. At the start of the game process, a larger log becomes `loader.old.log`.
	static constexpr std::uintmax_t k_MaxSize = 1024 * 1024;

	explicit LoaderLog(std::filesystem::path path);

	/// Move a log that is larger than `k_MaxSize` to `loader.old.log`. Only the game process calls it.
	void RotateIfLarge();

	/// Add one line to the log. A failed write does nothing: the extender never stops the game for its log.
	void Write(std::string_view text);

	template <typename... Args>
	void Info(std::format_string<Args...> format, Args&&... args)
	{
		Write(std::format(format, std::forward<Args>(args)...));
	}

	[[nodiscard]] const std::filesystem::path& Path() const { return _path; }

private:
	std::filesystem::path _path;
	std::mutex _mutex;
};

/// Add one line to a log file with no lock, no allocation and no C runtime call that can take a lock.
/// `DllMain` calls it when the process ends, while other threads can hold locks.
void WriteLineAtExit(const wchar_t* path, std::string_view text);

/// The prefix of a log line: the local time and the process id. It fits in a small buffer with no allocation.
[[nodiscard]] std::string_view LinePrefix(std::span<char> buffer);

} // namespace ts4_mod::loader
