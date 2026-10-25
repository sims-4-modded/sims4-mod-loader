/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include "game/loader/ExeImage.h"

#include <array>
#include <utility>

#include <windows.h>

namespace ts4_mod::loader
{

namespace
{
/// The size of the headers that the extender reads. The section table of the game is in the first page.
constexpr std::size_t k_HeaderSize = 4096;
/// The size of one block of a sample of the code section.
constexpr std::size_t k_SampleBlockSize = 64 * 1024;
/// The number of blocks in one sample: 4 MB of the code section.
constexpr std::size_t k_SampleBlockCount = 64;

[[nodiscard]] bool ReadOwnMemory(std::uintptr_t address, std::span<std::byte> target)
{
	SIZE_T copied = 0;
	return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), target.data(), target.size(),
	                         &copied) != FALSE &&
	       copied == target.size();
}
} // namespace

ExeImage::ExeImage(std::uintptr_t base, PeSection text)
    : _base(base)
    , _text(std::move(text))
{
}

ts4::Result<ExeImage> ExeImage::Open()
{
	const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
	std::array<std::byte, k_HeaderSize> headers {};
	if (base == 0 || !ReadOwnMemory(base, headers))
	{
		return ts4::MakeError("the extender cannot read the headers of the program");
	}
	auto sections = ReadPeSections(headers);
	if (!sections)
	{
		return sections.GetError();
	}
	auto text = FindPeSection(*sections, ".text");
	if (!text || text->virtualSize == 0)
	{
		return ts4::MakeError("the program has no .text section");
	}
	return ExeImage(base, std::move(*text));
}

ts4::Result<double> ExeImage::TextEntropy()
{
	const auto blocks = SampleBlocks(_text.virtualSize, k_SampleBlockSize, k_SampleBlockCount);
	std::size_t total = 0;
	for (const auto& block : blocks)
	{
		total += block.size;
	}
	_buffer.resize(total);
	std::size_t position = 0;
	for (const auto& block : blocks)
	{
		const auto target = std::span(_buffer).subspan(position, block.size);
		if (!ReadOwnMemory(_base + _text.virtualAddress + block.offset, target))
		{
			return ts4::MakeError("the extender cannot read the .text section now");
		}
		position += block.size;
	}
	return ShannonEntropy(_buffer);
}

} // namespace ts4_mod::loader
