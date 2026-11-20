#include "firmware_image.h"

#include <fstream>
#include <limits>
#include <stdexcept>

namespace mfc_tool::core {

std::uint32_t CheckedImageLength(std::uint64_t length, std::uint64_t limit) {
    if (length == 0u) {
        throw std::invalid_argument("binary image is empty");
    }
    if (length > limit || length > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::invalid_argument("binary image exceeds the transfer size limit");
    }
    return static_cast<std::uint32_t>(length);
}

void CheckImageAddressRange(std::uint32_t address, std::uint64_t length) {
    constexpr std::uint64_t address_space = std::uint64_t{1} << 32u;
    if (length == 0u || length > address_space - address) {
        throw std::invalid_argument("binary image address range overflows 32 bits");
    }
}

std::vector<std::uint8_t> ReadFirmwareImage(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("failed to open binary image");
    }
    const auto end = in.tellg();
    if (end < std::streampos{0}) {
        throw std::runtime_error("failed to determine binary image size");
    }
    const auto length = CheckedImageLength(static_cast<std::uint64_t>(end), kMaxLoadedImageBytes);
    std::vector<std::uint8_t> image(length);
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(image.size())) ||
        in.peek() != std::ifstream::traits_type::eof() || in.bad()) {
        throw std::runtime_error("binary image changed or could not be read completely");
    }
    return image;
}

} // namespace mfc_tool::core
