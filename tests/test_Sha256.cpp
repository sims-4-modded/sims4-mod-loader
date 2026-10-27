/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <cstddef>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

#include <core/Sha256.h>
#include <gtest/gtest.h>

using namespace ts4_mod::loader;

namespace
{
[[nodiscard]] std::span<const std::byte> Bytes(std::string_view text)
{
	return std::as_bytes(std::span(text.data(), text.size()));
}
} // namespace

// The test vectors of FIPS 180-4 and of the NIST examples of SHA-256.
TEST(Sha256, GivesTheChecksumsOfTheTestVectors)
{
	EXPECT_EQ(Sha256Hex(Bytes("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	EXPECT_EQ(Sha256Hex(Bytes("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	EXPECT_EQ(Sha256Hex(Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
	          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	EXPECT_EQ(Sha256Hex(Bytes("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopq"
	                          "rsmnopqrstnopqrstu")),
	          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
	const std::string million(1'000'000, 'a');
	EXPECT_EQ(Sha256Hex(Bytes(million)), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, GivesTheSameChecksumForBytesInManyParts)
{
	// The lengths 55, 56 and 64 are at the edges of the padding.
	for (const std::size_t length : {0u, 1u, 55u, 56u, 63u, 64u, 65u, 119u, 120u, 1000u})
	{
		std::string text(length, '\0');
		for (std::size_t i = 0; i < length; ++i)
		{
			text[i] = static_cast<char>((i * 7) + 3);
		}
		Sha256 parts;
		for (std::size_t start = 0; start < length; start += 13)
		{
			parts.Update(Bytes(std::string_view(text).substr(start, 13)));
		}
		EXPECT_EQ(ToHex(parts.Finish()), Sha256Hex(Bytes(text))) << length;
	}
}

TEST(Sha256, StartsANewChecksumAfterFinish)
{
	Sha256 checksum;
	checksum.Update(Bytes("abc"));
	(void)checksum.Finish();
	checksum.Update(Bytes("abc"));
	EXPECT_EQ(ToHex(checksum.Finish()), Sha256Hex(Bytes("abc")));
}

TEST(Sha256, ReadsTheChecksumOfAFile)
{
	const auto path = std::filesystem::path(testing::TempDir()) / "ts4_sha256_test.bin";
	{
		std::ofstream file(path, std::ios::binary);
		const std::string block(70'000, 'a');
		file << block;
	}
	const auto checksum = Sha256OfFile(path);
	ASSERT_TRUE(checksum) << checksum.GetError().message;
	EXPECT_EQ(*checksum, Sha256Hex(Bytes(std::string(70'000, 'a'))));
	std::filesystem::remove(path);
	EXPECT_FALSE(Sha256OfFile(path));
}
