/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

#include <vector>

#include <common/Result.h>
#include <core/GameStart.h>

namespace ts4_mod::loader
{

/// The image of the program of this process in memory, and the entropy of its code section.
/// The activation wrapper of EA decrypts the code section in memory before the game starts. The entropy of the
/// section tells if the code is decrypted.
class ExeImage
{
public:
	/// Read the section table of the program of this process. The headers are not encrypted.
	[[nodiscard]] static ts4::Result<ExeImage> Open();

	[[nodiscard]] std::uintptr_t Base() const { return _base; }
	[[nodiscard]] const PeSection& Text() const { return _text; }

	/// Measure the entropy of the code section from blocks at equal spaces in it. The function copies the bytes with
	/// `ReadProcessMemory`, so a page that the wrapper protects gives an error and not an access violation.
	[[nodiscard]] ts4::Result<double> TextEntropy();

private:
	ExeImage(std::uintptr_t base, PeSection text);

	std::uintptr_t _base = 0;
	PeSection _text;
	std::vector<std::byte> _buffer;
};

} // namespace ts4_mod::loader
