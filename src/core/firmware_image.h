#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace mfc_tool::core {

// Host memory budget, not a target flash-capacity or MCU profile.
constexpr std::uint64_t kMaxLoadedImageBytes = 64u * 1024u * 1024u;
constexpr std::uint64_t kMaxIspImageBytes = 0x00100000u;

std::uint32_t CheckedImageLength(std::uint64_t length, std::uint64_t limit);
void CheckImageAddressRange(std::uint32_t address, std::uint64_t length);
std::vector<std::uint8_t> ReadFirmwareImage(const std::filesystem::path& path);

} // namespace mfc_tool::core
