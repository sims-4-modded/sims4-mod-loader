/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include <cmath>
#include <cstring>
#include <cwctype>

#include <algorithm>
#include <array>

#include <core/GameStart.h>

namespace ts4_mod::loader
{

namespace
{
/// The values of the PE format that the reader of the section table uses.
constexpr std::uint16_t k_DosMagic = 0x5a4d;         // "MZ"
constexpr std::uint32_t k_NtSignature = 0x00004550;  // "PE\0\0"
constexpr std::uint16_t k_Pe64Magic = 0x020b;        // PE32+
constexpr std::size_t k_DosNtOffsetField = 0x3c;     // The field of the DOS header that gives the NT headers.
constexpr std::size_t k_FileHeaderSize = 20;         // The COFF file header, after the signature.
constexpr std::size_t k_SectionHeaderSize = 40;      // One entry of the section table.
constexpr std::size_t k_SectionNameSize = 8;         // The name of a section, padded with zeros.
constexpr std::size_t k_SectionVirtualSizeField = 8; // The virtual size in a section header.
constexpr std::size_t k_SectionVirtualAddressField = 12;

template <typename T>
[[nodiscard]] std::optional<T> ReadAt(std::span<const std::byte> bytes, std::size_t offset)
{
	if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
	{
		return std::nullopt;
	}
	T value {};
	std::memcpy(&value, bytes.data() + offset, sizeof(T));
	return value;
}

[[nodiscard]] bool SameTextNoCase(std::wstring_view a, std::wstring_view b)
{
	return std::ranges::equal(a, b, [](wchar_t x, wchar_t y) {
		return std::towlower(static_cast<wint_t>(x)) == std::towlower(static_cast<wint_t>(y));
	});
}
} // namespace

bool IsGameExe(const std::filesystem::path& exePath)
{
	return SameTextNoCase(exePath.filename().wstring(), k_GameExeName);
}

std::optional<std::filesystem::path> GameFolderOfExe(const std::filesystem::path& exePath)
{
	const auto bin = exePath.parent_path();
	const auto game = bin.parent_path();
	if (!IsGameExe(exePath) || !SameTextNoCase(bin.filename().wstring(), L"Bin") ||
	    !SameTextNoCase(game.filename().wstring(), L"Game") || !game.has_parent_path() || game.parent_path() == game)
	{
		return std::nullopt;
	}
	return game.parent_path();
}

ts4::Result<std::vector<PeSection>> ReadPeSections(std::span<const std::byte> headers)
{
	if (ReadAt<std::uint16_t>(headers, 0) != k_DosMagic)
	{
		return ts4::MakeError("the image has no DOS header");
	}
	const auto ntOffset = ReadAt<std::uint32_t>(headers, k_DosNtOffsetField);
	if (!ntOffset || ReadAt<std::uint32_t>(headers, *ntOffset) != k_NtSignature)
	{
		return ts4::MakeError("the image has no NT headers");
	}
	const std::size_t fileHeader = static_cast<std::size_t>(*ntOffset) + 4;
	const auto sectionCount = ReadAt<std::uint16_t>(headers, fileHeader + 2);
	const auto optionalSize = ReadAt<std::uint16_t>(headers, fileHeader + 16);
	const std::size_t optionalHeader = fileHeader + k_FileHeaderSize;
	if (!sectionCount || !optionalSize || ReadAt<std::uint16_t>(headers, optionalHeader) != k_Pe64Magic)
	{
		return ts4::MakeError("the image is not a 64-bit PE image");
	}
	const std::size_t table = optionalHeader + *optionalSize;
	std::vector<PeSection> sections;
	for (std::size_t i = 0; i < *sectionCount; ++i)
	{
		const std::size_t entry = table + (i * k_SectionHeaderSize);
		const auto virtualSize = ReadAt<std::uint32_t>(headers, entry + k_SectionVirtualSizeField);
		const auto virtualAddress = ReadAt<std::uint32_t>(headers, entry + k_SectionVirtualAddressField);
		if (!virtualSize || !virtualAddress)
		{
			return ts4::MakeError("the section table of the image is not complete");
		}
		std::array<char, k_SectionNameSize> name {};
		std::memcpy(name.data(), headers.data() + entry, name.size());
		const auto length = std::ranges::find(name, '\0') - name.begin();
		sections.push_back(PeSection {.name = std::string(name.data(), static_cast<std::size_t>(length)),
		                              .virtualAddress = *virtualAddress,
		                              .virtualSize = *virtualSize});
	}
	return sections;
}

std::optional<PeSection> FindPeSection(std::span<const PeSection> sections, std::string_view name)
{
	const auto found = std::ranges::find(sections, name, &PeSection::name);
	return found == sections.end() ? std::nullopt : std::optional(*found);
}

double ShannonEntropy(std::span<const std::byte> bytes)
{
	if (bytes.empty())
	{
		return 0.0;
	}
	std::array<std::size_t, 256> counts {};
	for (const auto value : bytes)
	{
		++counts[std::to_integer<std::size_t>(value)];
	}
	const auto total = static_cast<double>(bytes.size());
	double entropy = 0.0;
	for (const auto count : counts)
	{
		if (count != 0)
		{
			const double p = static_cast<double>(count) / total;
			entropy -= p * std::log2(p);
		}
	}
	return entropy;
}

std::vector<SampleBlock> SampleBlocks(std::size_t sectionSize, std::size_t blockSize, std::size_t count)
{
	if (sectionSize == 0 || blockSize == 0 || count == 0)
	{
		return {};
	}
	if (sectionSize <= blockSize || count == 1)
	{
		return {SampleBlock {.offset = 0, .size = std::min(sectionSize, blockSize)}};
	}
	const std::size_t blocks = std::min(count, sectionSize / blockSize);
	const std::size_t last = sectionSize - blockSize;
	std::vector<SampleBlock> result;
	result.reserve(blocks);
	for (std::size_t i = 0; i < blocks; ++i)
	{
		const std::size_t offset = blocks == 1 ? 0 : last * i / (blocks - 1);
		result.push_back(SampleBlock {.offset = offset, .size = blockSize});
	}
	return result;
}

std::string_view ImageStateName(ImageState state)
{
	switch (state)
	{
	case ImageState::Encrypted:
		return "encrypted";
	case ImageState::Changing:
		return "changing";
	case ImageState::Ready:
		return "decrypted";
	}
	return "unknown";
}

ImageState DecryptionWatch::Add(double entropy)
{
	const auto previous = _last;
	if (!_first)
	{
		_first = entropy;
	}
	_last = entropy;
	++_samples;
	if (entropy >= k_DecryptedEntropyLimit)
	{
		_state = ImageState::Encrypted;
	}
	else if (previous && *previous < k_DecryptedEntropyLimit && std::abs(entropy - *previous) <= k_StableEntropyChange)
	{
		_state = ImageState::Ready;
	}
	else
	{
		_state = ImageState::Changing;
	}
	return _state;
}

} // namespace ts4_mod::loader
