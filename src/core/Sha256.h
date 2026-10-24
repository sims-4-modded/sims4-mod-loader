/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

#include <array>
#include <filesystem>
#include <span>
#include <string>

#include <common/Result.h>

namespace ts4_mod::loader
{

/// The SHA-256 checksum (FIPS 180-4) of a sequence of bytes. The extender gives the checksum of each DLL mod in the
/// list of the mods and in the log, so that the user can see which file loaded.
class Sha256
{
public:
	using Digest = std::array<std::uint8_t, 32>;

	Sha256() = default;

	/// Add bytes to the checksum. The checksum of the bytes of two calls is the same as the checksum of one call with
	/// all the bytes.
	void Update(std::span<const std::byte> bytes);

	/// Complete the checksum and return it. After this call, the object starts a new checksum.
	[[nodiscard]] Digest Finish();

private:
	void Compress(std::span<const std::uint8_t, 64> block);

	std::array<std::uint32_t, 8> _state {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
	                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	std::array<std::uint8_t, 64> _block {};
	std::size_t _blockSize = 0;
	std::uint64_t _length = 0;
};

/// The text of a checksum: two lower-case hex digits for each byte.
[[nodiscard]] std::string ToHex(std::span<const std::uint8_t> bytes);

/// The SHA-256 checksum of the bytes as text of 64 hex digits.
[[nodiscard]] std::string Sha256Hex(std::span<const std::byte> bytes);

/// The SHA-256 checksum of the bytes of a file as text of 64 hex digits. Return an error if the file cannot be read.
[[nodiscard]] ts4::Result<std::string> Sha256OfFile(const std::filesystem::path& path);

} // namespace ts4_mod::loader
