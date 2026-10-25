/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <common/Result.h>

namespace ts4_mod::loader
{

/// The file name of the program of the game. The proxy starts the extender only in this program.
inline constexpr std::wstring_view k_GameExeName = L"TS4_x64.exe";

/// The entropy limit of a decrypted code section, in bits for each byte. Encrypted bytes have an entropy of almost 8.
/// The x64 code of the game has an entropy of approximately 6.5. The limit is between the two, with a large margin.
inline constexpr double k_DecryptedEntropyLimit = 7.5;

/// The largest change of the entropy between two samples of a code section that is stable.
inline constexpr double k_StableEntropyChange = 0.01;

/// True if `exePath` is the program of the game (`TS4_x64.exe`). The test ignores the case of letters, as Windows
/// does. Other programs, for example the crash reporter of EA, can also load the proxy.
[[nodiscard]] bool IsGameExe(const std::filesystem::path& exePath);

/// The game folder (`The Sims 4`) of the program of the game (`<game folder>\Game\Bin\TS4_x64.exe`).
/// Return nothing if the path does not have this form.
[[nodiscard]] std::optional<std::filesystem::path> GameFolderOfExe(const std::filesystem::path& exePath);

/// One section of a PE image, from the section table in the headers.
struct PeSection
{
	std::string name;
	std::uint32_t virtualAddress = 0;
	std::uint32_t virtualSize = 0;
};

/// Read the section table from the first bytes of a PE image (the DOS header, the NT headers and the section
/// headers). Return an error if the bytes are not the headers of a 64-bit PE image.
[[nodiscard]] ts4::Result<std::vector<PeSection>> ReadPeSections(std::span<const std::byte> headers);

/// Find a section by its name. Return nothing if the image has no section with this name.
[[nodiscard]] std::optional<PeSection> FindPeSection(std::span<const PeSection> sections, std::string_view name);

/// The Shannon entropy of the bytes, in bits for each byte (0 to 8). Return 0 for no bytes.
[[nodiscard]] double ShannonEntropy(std::span<const std::byte> bytes);

/// One block of a section that the extender reads to measure the entropy.
struct SampleBlock
{
	std::size_t offset = 0;
	std::size_t size = 0;
};

/// Divide a section of `sectionSize` bytes into at most `count` blocks of `blockSize` bytes, with equal spaces between
/// them. The blocks start at the start of the section and stay in the section. A small section gives one block.
[[nodiscard]] std::vector<SampleBlock> SampleBlocks(std::size_t sectionSize, std::size_t blockSize, std::size_t count);

/// The state of the code section of the program of the game.
enum class ImageState
{
	/// The entropy is at or above the limit: the activation wrapper did not decrypt the code.
	Encrypted,
	/// The entropy is below the limit but it changed since the last sample: the decryption can be in progress.
	Changing,
	/// The entropy is below the limit and stable: the code is decrypted.
	Ready,
};

[[nodiscard]] std::string_view ImageStateName(ImageState state);

/// Decides from samples of the entropy of the code section when the activation wrapper decrypted the program.
/// A decryption in place can stop at a point where part of the section is decrypted. Thus one sample below the limit
/// is not sufficient: the next sample must also be below the limit and almost the same.
class DecryptionWatch
{
public:
	/// Add one sample of the entropy. Return the state after this sample.
	ImageState Add(double entropy);

	[[nodiscard]] ImageState State() const { return _state; }
	/// The first sample, or nothing before the first sample.
	[[nodiscard]] std::optional<double> First() const { return _first; }
	/// The last sample, or nothing before the first sample.
	[[nodiscard]] std::optional<double> Last() const { return _last; }
	[[nodiscard]] std::size_t Samples() const { return _samples; }

private:
	ImageState _state = ImageState::Encrypted;
	std::optional<double> _first;
	std::optional<double> _last;
	std::size_t _samples = 0;
};

} // namespace ts4_mod::loader
