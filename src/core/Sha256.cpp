/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <algorithm>
#include <bit>
#include <format>
#include <fstream>
#include <vector>

#include <core/Sha256.h>

namespace ts4_mod::loader
{

namespace
{
/// The round constants of SHA-256.
constexpr std::array<std::uint32_t, 64> k_Rounds {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

/// The size of the blocks that `Sha256OfFile` reads.
constexpr std::size_t k_FileBlock = 64 * 1024;

[[nodiscard]] std::string Utf8(const std::filesystem::path& path)
{
	const auto text = path.u8string();
	return {text.begin(), text.end()};
}
} // namespace

void Sha256::Compress(std::span<const std::uint8_t, 64> block)
{
	std::array<std::uint32_t, 64> words {};
	for (std::size_t i = 0; i < 16; ++i)
	{
		words[i] = static_cast<std::uint32_t>(block[i * 4]) << 24 | static_cast<std::uint32_t>(block[i * 4 + 1]) << 16 |
		           static_cast<std::uint32_t>(block[i * 4 + 2]) << 8 | static_cast<std::uint32_t>(block[i * 4 + 3]);
	}
	for (std::size_t i = 16; i < 64; ++i)
	{
		const auto s0 = std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
		const auto s1 = std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
		words[i] = words[i - 16] + s0 + words[i - 7] + s1;
	}
	auto [a, b, c, d, e, f, g, h] = _state;
	for (std::size_t i = 0; i < 64; ++i)
	{
		const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
		const auto choice = (e & f) ^ (~e & g);
		const auto first = h + s1 + choice + k_Rounds[i] + words[i];
		const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
		const auto majority = (a & b) ^ (a & c) ^ (b & c);
		const auto second = s0 + majority;
		h = g;
		g = f;
		f = e;
		e = d + first;
		d = c;
		c = b;
		b = a;
		a = first + second;
	}
	const std::array<std::uint32_t, 8> added {a, b, c, d, e, f, g, h};
	for (std::size_t i = 0; i < 8; ++i)
	{
		_state[i] += added[i];
	}
}

void Sha256::Update(std::span<const std::byte> bytes)
{
	_length += bytes.size();
	for (const auto byte : bytes)
	{
		_block[_blockSize++] = static_cast<std::uint8_t>(byte);
		if (_blockSize == _block.size())
		{
			Compress(_block);
			_blockSize = 0;
		}
	}
}

Sha256::Digest Sha256::Finish()
{
	const auto bits = _length * 8;
	// The padding: one bit 1, then bits 0 until 8 bytes stay in the block, then the length in bits (big endian).
	_block[_blockSize++] = 0x80;
	if (_blockSize > 56)
	{
		std::fill(_block.begin() + static_cast<std::ptrdiff_t>(_blockSize), _block.end(), std::uint8_t {0});
		Compress(_block);
		_blockSize = 0;
	}
	std::fill(_block.begin() + static_cast<std::ptrdiff_t>(_blockSize), _block.begin() + 56, std::uint8_t {0});
	for (std::size_t i = 0; i < 8; ++i)
	{
		_block[56 + i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
	}
	Compress(_block);
	Digest digest {};
	for (std::size_t i = 0; i < 8; ++i)
	{
		digest[i * 4] = static_cast<std::uint8_t>(_state[i] >> 24);
		digest[i * 4 + 1] = static_cast<std::uint8_t>(_state[i] >> 16);
		digest[i * 4 + 2] = static_cast<std::uint8_t>(_state[i] >> 8);
		digest[i * 4 + 3] = static_cast<std::uint8_t>(_state[i]);
	}
	*this = Sha256();
	return digest;
}

std::string ToHex(std::span<const std::uint8_t> bytes)
{
	std::string text;
	text.reserve(bytes.size() * 2);
	for (const auto byte : bytes)
	{
		text += std::format("{:02x}", byte);
	}
	return text;
}

std::string Sha256Hex(std::span<const std::byte> bytes)
{
	Sha256 checksum;
	checksum.Update(bytes);
	return ToHex(checksum.Finish());
}

ts4::Result<std::string> Sha256OfFile(const std::filesystem::path& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
	{
		return ts4::MakeError("the file {} cannot be read", Utf8(path));
	}
	Sha256 checksum;
	std::vector<char> block(k_FileBlock);
	while (file)
	{
		file.read(block.data(), static_cast<std::streamsize>(block.size()));
		const auto count = static_cast<std::size_t>(file.gcount());
		checksum.Update(std::as_bytes(std::span(block.data(), count)));
	}
	if (file.bad())
	{
		return ts4::MakeError("the file {} cannot be read to its end", Utf8(path));
	}
	return ToHex(checksum.Finish());
}

} // namespace ts4_mod::loader
