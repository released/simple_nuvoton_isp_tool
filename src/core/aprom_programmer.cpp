#include "aprom_programmer.h"

#include <algorithm>

#include "firmware_image.h"

namespace mfc_tool::core {
namespace {

void AppendLe32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void CheckCancelled(const std::function<bool()>& cancel) {
    if (cancel && cancel()) {
        throw std::runtime_error("operation cancelled by user; acknowledged data may remain on target");
    }
}

} // namespace

ProgrammingOutcomeUnknown::ProgrammingOutcomeUnknown(std::uint32_t confirmed, std::uint32_t packet,
                                                     const std::string& reason)
    : std::runtime_error("Programming outcome unknown; no replay. Confirmed bytes=" +
                         std::to_string(confirmed) + ", attempted packet=" + std::to_string(packet) +
                         ". Reconcile target state before another update. Cause: " + reason),
      confirmed_bytes(confirmed), attempted_packet(packet) {
}

void ProgramApromImage(const std::vector<std::uint8_t>& image,
                       const std::function<void()>& sync,
                       const ApromExchange& exchange,
                       const std::function<void(const ApromProgress&)>& progress,
                       const std::function<bool()>& cancel,
                       const std::function<void()>& poll) {
    const auto total = CheckedImageLength(image.size(), kMaxIspImageBytes);
    // Address zero is the existing NuvoISP wire contract; mapping/protection is target-owned.
    CheckImageAddressRange(0u, total);
    CheckCancelled(cancel);
    sync();
    std::uint32_t offset = 0u;
    std::uint32_t packet_count = 0u;
    while (offset < total) {
        CheckCancelled(cancel);
        const bool first = offset == 0u;
        const auto length = (std::min)(total - offset, first ? 48u : 56u);
        std::vector<std::uint8_t> payload;
        if (first) {
            AppendLe32(payload, 0u);
            AppendLe32(payload, total);
        }
        payload.insert(payload.end(), image.begin() + offset, image.begin() + offset + length);
        try {
            exchange(first ? 0xA0u : 0u, payload, first);
        } catch (const std::exception& e) {
            throw ProgrammingOutcomeUnknown(offset, packet_count + 1u, e.what());
        }
        offset += length;
        ++packet_count;
        if (progress) {
            progress({offset, total, packet_count});
        }
        if (poll) {
            poll();
        }
    }
}

} // namespace mfc_tool::core
