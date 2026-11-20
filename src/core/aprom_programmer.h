#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mfc_tool::core {

class ProgrammingOutcomeUnknown : public std::runtime_error {
public:
    ProgrammingOutcomeUnknown(std::uint32_t confirmed, std::uint32_t packet, const std::string& reason);
    const std::uint32_t confirmed_bytes;
    const std::uint32_t attempted_packet;
};

struct ApromProgress {
    std::uint32_t bytes_done = 0;
    std::uint32_t total_bytes = 0;
    std::uint32_t packet_index = 0;
};

// The adapter must validate the matching checksum/index or throw. No data replay.
using ApromExchange = std::function<void(std::uint32_t, const std::vector<std::uint8_t>&, bool)>;

void ProgramApromImage(const std::vector<std::uint8_t>& image,
                       const std::function<void()>& sync,
                       const ApromExchange& exchange,
                       const std::function<void(const ApromProgress&)>& progress,
                       const std::function<bool()>& cancel,
                       const std::function<void()>& poll);

} // namespace mfc_tool::core
