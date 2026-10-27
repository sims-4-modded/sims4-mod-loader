/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <filesystem>
#include <random>
#include <string_view>
#include <vector>

#include <core/GameStart.h>
#include <gtest/gtest.h>

using namespace ts4_mod::loader;

namespace
{
template <typename T>
void Put(std::vector<std::byte>& bytes, std::size_t offset, T value)
{
	std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

struct FakeSection
{
	std::string_view name;
	std::uint32_t virtualAddress;
	std::uint32_t virtualSize;
};

/// Make the headers of a small 64-bit PE image with the sections.
[[nodiscard]] std::vector<std::byte> FakeHeaders(std::initializer_list<FakeSection> sections, std::uint16_t magic = 0x020b)
{
	constexpr std::size_t k_NtOffset = 0x80;
	constexpr std::uint16_t k_OptionalSize = 0xf0;
	std::vector<std::byte> bytes(0x400);
	Put<std::uint16_t>(bytes, 0, 0x5a4d);
	Put<std::uint32_t>(bytes, 0x3c, k_NtOffset);
	Put<std::uint32_t>(bytes, k_NtOffset, 0x00004550);
	Put<std::uint16_t>(bytes, k_NtOffset + 4 + 2, static_cast<std::uint16_t>(sections.size()));
	Put<std::uint16_t>(bytes, k_NtOffset + 4 + 16, k_OptionalSize);
	Put<std::uint16_t>(bytes, k_NtOffset + 24, magic);
	std::size_t entry = k_NtOffset + 24 + k_OptionalSize;
	for (const auto& section : sections)
	{
		std::memcpy(bytes.data() + entry, section.name.data(), section.name.size());
		Put<std::uint32_t>(bytes, entry + 8, section.virtualSize);
		Put<std::uint32_t>(bytes, entry + 12, section.virtualAddress);
		entry += 40;
	}
	return bytes;
}

[[nodiscard]] std::vector<std::byte> RandomBytes(std::size_t size)
{
	std::mt19937 engine(42);
	std::uniform_int_distribution<int> value(0, 255);
	std::vector<std::byte> bytes(size);
	for (auto& byte : bytes)
	{
		byte = static_cast<std::byte>(value(engine));
	}
	return bytes;
}
} // namespace

TEST(GameStart, FindsTheProgramOfTheGame)
{
	EXPECT_TRUE(IsGameExe(L"F:/Origin Games/The Sims 4/Game/Bin/TS4_x64.exe"));
	EXPECT_TRUE(IsGameExe(L"f:\\origin games\\the sims 4\\game\\bin\\ts4_X64.EXE"));
	EXPECT_FALSE(IsGameExe(L"F:/Origin Games/The Sims 4/Game/Bin/TS4_x64.exe.bak"));
	EXPECT_FALSE(IsGameExe(L"C:/Program Files/Electronic Arts/EA Desktop/EADesktop.exe"));
	EXPECT_FALSE(IsGameExe(L"F:/Origin Games/The Sims 4/Game/Bin/CrashSender.exe"));
	EXPECT_FALSE(IsGameExe(L""));
}

TEST(GameStart, GivesTheGameFolderOfTheProgram)
{
	EXPECT_EQ(GameFolderOfExe(L"F:/Origin Games/The Sims 4/Game/Bin/TS4_x64.exe"),
	          std::filesystem::path(L"F:/Origin Games/The Sims 4"));
	EXPECT_EQ(GameFolderOfExe(L"F:\\Origin Games\\The Sims 4\\game\\bin\\TS4_x64.exe"),
	          std::filesystem::path(L"F:\\Origin Games\\The Sims 4"));
	EXPECT_FALSE(GameFolderOfExe(L"F:/Origin Games/The Sims 4/Game/TS4_x64.exe"));
	EXPECT_FALSE(GameFolderOfExe(L"F:/Origin Games/The Sims 4/Game/Bin/other.exe"));
	EXPECT_FALSE(GameFolderOfExe(L"TS4_x64.exe"));
}

TEST(GameStart, ReadsTheSectionTable)
{
	const auto headers = FakeHeaders({{.name = ".text", .virtualAddress = 0x1000, .virtualSize = 0x2000},
	                                  {.name = ".rdata", .virtualAddress = 0x3000, .virtualSize = 0x500},
	                                  {.name = ".ooa", .virtualAddress = 0x4000, .virtualSize = 0x100}});
	const auto sections = ReadPeSections(headers);
	ASSERT_TRUE(sections) << sections.GetError().message;
	ASSERT_EQ(sections->size(), 3U);
	const auto text = FindPeSection(*sections, ".text").value_or(PeSection {});
	EXPECT_EQ(text.name, ".text");
	EXPECT_EQ(text.virtualAddress, 0x1000U);
	EXPECT_EQ(text.virtualSize, 0x2000U);
	EXPECT_EQ(FindPeSection(*sections, ".ooa").value_or(PeSection {}).virtualAddress, 0x4000U);
	EXPECT_FALSE(FindPeSection(*sections, ".data"));
}

TEST(GameStart, RefusesHeadersThatAreNotA64BitImage)
{
	EXPECT_FALSE(ReadPeSections({}));
	EXPECT_FALSE(ReadPeSections(FakeHeaders({{.name = ".text", .virtualAddress = 0x1000, .virtualSize = 0x2000}}, 0x010b)));
	auto noSignature = FakeHeaders({{.name = ".text", .virtualAddress = 0x1000, .virtualSize = 0x2000}});
	Put<std::uint32_t>(noSignature, 0x80, 0);
	EXPECT_FALSE(ReadPeSections(noSignature));
	// A section table that goes past the end of the bytes.
	auto shortTable = FakeHeaders({{.name = ".text", .virtualAddress = 0x1000, .virtualSize = 0x2000}});
	Put<std::uint16_t>(shortTable, 0x80 + 4 + 2, 100);
	EXPECT_FALSE(ReadPeSections(shortTable));
	// An offset of the NT headers that is past the end of the bytes.
	auto farOffset = FakeHeaders({{.name = ".text", .virtualAddress = 0x1000, .virtualSize = 0x2000}});
	Put<std::uint32_t>(farOffset, 0x3c, 0xfffffff0);
	EXPECT_FALSE(ReadPeSections(farOffset));
}

TEST(GameStart, MeasuresTheEntropy)
{
	EXPECT_DOUBLE_EQ(ShannonEntropy({}), 0.0);
	EXPECT_DOUBLE_EQ(ShannonEntropy(std::vector<std::byte>(1000, std::byte {7})), 0.0);
	std::vector<std::byte> twoValues(1000, std::byte {0});
	std::fill(twoValues.begin(), twoValues.begin() + 500, std::byte {1});
	EXPECT_DOUBLE_EQ(ShannonEntropy(twoValues), 1.0);
	std::vector<std::byte> allValues(256 * 4);
	for (std::size_t i = 0; i < allValues.size(); ++i)
	{
		allValues[i] = static_cast<std::byte>(i % 256);
	}
	EXPECT_DOUBLE_EQ(ShannonEntropy(allValues), 8.0);
	// Random bytes are as encrypted bytes: above the limit.
	EXPECT_GT(ShannonEntropy(RandomBytes(64 * 1024)), k_DecryptedEntropyLimit);
}

TEST(GameStart, DividesASectionIntoSampleBlocks)
{
	EXPECT_TRUE(SampleBlocks(0, 4096, 8).empty());
	EXPECT_TRUE(SampleBlocks(1000, 0, 8).empty());
	EXPECT_TRUE(SampleBlocks(1000, 4096, 0).empty());

	const auto small = SampleBlocks(1000, 4096, 8);
	ASSERT_EQ(small.size(), 1U);
	EXPECT_EQ(small[0].offset, 0U);
	EXPECT_EQ(small[0].size, 1000U);

	const auto blocks = SampleBlocks(1'000'000, 4096, 8);
	ASSERT_EQ(blocks.size(), 8U);
	EXPECT_EQ(blocks.front().offset, 0U);
	EXPECT_EQ(blocks.back().offset + blocks.back().size, 1'000'000U);
	for (std::size_t i = 1; i < blocks.size(); ++i)
	{
		EXPECT_GE(blocks[i].offset, blocks[i - 1].offset + blocks[i - 1].size);
	}

	// A section with space for only three blocks gives three blocks.
	EXPECT_EQ(SampleBlocks((3 * 4096) + 10, 4096, 8).size(), 3U);
}

TEST(GameStart, WaitsForAStableDecryptedSection)
{
	DecryptionWatch watch;
	EXPECT_EQ(watch.State(), ImageState::Encrypted);
	EXPECT_FALSE(watch.First());
	EXPECT_EQ(watch.Add(7.999), ImageState::Encrypted);
	EXPECT_EQ(watch.Add(7.998), ImageState::Encrypted);
	// The decryption is in progress: the entropy falls.
	EXPECT_EQ(watch.Add(7.2), ImageState::Changing);
	EXPECT_EQ(watch.Add(6.6), ImageState::Changing);
	EXPECT_EQ(watch.Add(6.483), ImageState::Changing);
	EXPECT_EQ(watch.Add(6.481), ImageState::Ready);
	EXPECT_DOUBLE_EQ(*watch.First(), 7.999);
	EXPECT_DOUBLE_EQ(*watch.Last(), 6.481);
	EXPECT_EQ(watch.Samples(), 6U);
	EXPECT_EQ(ImageStateName(watch.State()), "decrypted");
}

TEST(GameStart, NeedsTwoSamplesForADecryptedSection)
{
	// The proxy can load after the decryption: the first sample is low, and the second sample makes it ready.
	DecryptionWatch watch;
	EXPECT_EQ(watch.Add(6.48), ImageState::Changing);
	EXPECT_EQ(watch.Add(6.48), ImageState::Ready);

	// An encrypted sample between two low samples starts the wait again.
	DecryptionWatch back;
	EXPECT_EQ(back.Add(6.48), ImageState::Changing);
	EXPECT_EQ(back.Add(7.99), ImageState::Encrypted);
	EXPECT_EQ(back.Add(6.48), ImageState::Changing);
	EXPECT_EQ(back.Add(6.48), ImageState::Ready);
}
