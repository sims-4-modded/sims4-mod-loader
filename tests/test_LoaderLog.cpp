/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "game/loader/LoaderLog.h"

using namespace ts4_mod::loader;

namespace
{
[[nodiscard]] std::filesystem::path TestFolder(const char* name)
{
	auto folder = std::filesystem::temp_directory_path() / "ts4_loader_tests" / name;
	std::filesystem::remove_all(folder);
	return folder;
}

[[nodiscard]] std::vector<std::string> Lines(const std::filesystem::path& path)
{
	std::ifstream file(path, std::ios::binary);
	std::vector<std::string> lines;
	for (std::string line; std::getline(file, line);)
	{
		lines.push_back(line);
	}
	return lines;
}
} // namespace

TEST(LoaderLog, GivesEachLineTheTimeAndTheProcess)
{
	std::array<char, 64> buffer {};
	const auto prefix = LinePrefix(buffer);
	// For example "2026-10-25 01:12:43.170 [48040] ".
	ASSERT_GE(prefix.size(), 28U);
	EXPECT_EQ(prefix[4], '-');
	EXPECT_EQ(prefix[10], ' ');
	EXPECT_EQ(prefix[19], '.');
	EXPECT_NE(prefix.find(std::format("[{}] ", GetCurrentProcessId())), std::string_view::npos);
	EXPECT_EQ(prefix.back(), ' ');

	// A buffer that is too small gives the start of the prefix only.
	std::array<char, 8> small {};
	EXPECT_EQ(LinePrefix(small).size(), small.size());
}

TEST(LoaderLog, AddsLinesAtTheEndOfTheFile)
{
	const auto folder = TestFolder("append");
	const auto path = folder / "logs" / "loader.log";
	{
		LoaderLog log(path);
		log.Info("first line {}", 1);
	}
	LoaderLog log(path);
	log.Write("second line");
	WriteLineAtExit(path.c_str(), "stub process: the last line");
	const auto lines = Lines(path);
	ASSERT_EQ(lines.size(), 3U);
	EXPECT_TRUE(lines[0].ends_with("] first line 1"));
	EXPECT_TRUE(lines[1].ends_with("] second line"));
	EXPECT_TRUE(lines[2].ends_with("] stub process: the last line"));
}

TEST(LoaderLog, CutsALongLineAtExit)
{
	const auto folder = TestFolder("long");
	std::filesystem::create_directories(folder);
	const auto path = folder / "loader.log";
	WriteLineAtExit(path.c_str(), std::string(5000, 'x'));
	const auto lines = Lines(path);
	ASSERT_EQ(lines.size(), 1U);
	EXPECT_LT(lines[0].size(), 1024U);
	EXPECT_TRUE(lines[0].ends_with("xxx"));
}

TEST(LoaderLog, MovesALargeLogToTheOldLog)
{
	const auto folder = TestFolder("rotate");
	const auto path = folder / "loader.log";
	LoaderLog log(path);
	log.Write(std::string(LoaderLog::k_MaxSize + 10, 'a'));
	log.RotateIfLarge();
	EXPECT_FALSE(std::filesystem::exists(path));
	EXPECT_TRUE(std::filesystem::exists(folder / "loader.old.log"));
	log.Write("after the move");
	ASSERT_EQ(Lines(path).size(), 1U);
	// A small log stays.
	log.RotateIfLarge();
	EXPECT_TRUE(std::filesystem::exists(path));
}
