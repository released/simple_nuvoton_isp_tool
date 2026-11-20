#include "nuvo_isp_i2c.h"

#include <algorithm>
#include "aprom_programmer.h"
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "text_utils.h"

namespace mfc_tool::core {
namespace {

constexpr std::uint32_t CMD_SYNC_PACKNO = 0x000000A4u;
constexpr std::uint32_t CMD_GET_FWVER = 0x000000A6u;
constexpr std::uint32_t CMD_RUN_APROM = 0x000000ABu;
constexpr std::uint32_t CMD_RESET = 0x000000ADu;
constexpr std::uint32_t CMD_CONNECT = 0x000000AEu;
constexpr std::uint32_t CMD_GET_DEVICEID = 0x000000B1u;
constexpr std::size_t kPacketSize = 64u;
constexpr std::size_t kHeaderSize = 8u;
constexpr std::size_t kPayloadSize = kPacketSize - kHeaderSize;

} // namespace

NuvoIspI2cClient::NuvoIspI2cClient(BridgeService* service)
    : service_(service) {
}

void NuvoIspI2cClient::SetOptions(const NuvoIspI2cOptions& options) {
    if (options.port < 0 || options.port > 1 || options.addr7 < 0 || options.addr7 > 0x7F) {
        throw std::invalid_argument("invalid I2C port or 7-bit target address");
    }
    options_ = options;
    options_.response_delay_ms = std::clamp(options_.response_delay_ms, 0, 1000);
    options_.response_poll_ms = std::clamp(options_.response_poll_ms, 1, 1000);
    options_.normal_response_timeout_ms = std::clamp(options_.normal_response_timeout_ms, 100, 60000);
    options_.long_response_timeout_ms = std::clamp(options_.long_response_timeout_ms, 1000, 120000);
    options_.program_retry_count = std::clamp(options_.program_retry_count, 1, 50);
}

void NuvoIspI2cClient::ConnectTarget(const std::function<bool()>& should_cancel,
                                     const std::function<void()>& on_poll) {
    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(options_.long_response_timeout_ms);
    const auto retry_sleep = std::chrono::milliseconds((std::max)(20, options_.response_poll_ms));
    const int attempt_timeout_ms = (std::min)(options_.normal_response_timeout_ms, 250);
    std::string last_error = "target did not respond";

    while (true) {
        ThrowIfCancelled(should_cancel);
        try {
            (void)Transact(CMD_CONNECT, {}, false, attempt_timeout_ms, true, should_cancel, on_poll);
            SyncPacketNumber(should_cancel, on_poll);
            return;
        } catch (const std::exception& e) {
            last_error = e.what();
            ThrowIfCancelled(should_cancel);
        }

        if (on_poll) {
            on_poll();
        }
        if (std::chrono::steady_clock::now() - start >= timeout) {
            break;
        }
        std::this_thread::sleep_for(retry_sleep);
    }

    throw std::runtime_error("ISP target handshake timeout: " + last_error);
}

void NuvoIspI2cClient::SyncPacketNumber(const std::function<bool()>& should_cancel,
                                        const std::function<void()>& on_poll) {
    std::vector<std::uint8_t> payload;
    packet_index_ = 1u;
    AppendLe32(&payload, packet_index_);
    (void)Transact(CMD_SYNC_PACKNO, payload, false, options_.normal_response_timeout_ms, true, should_cancel, on_poll);
}

std::uint8_t NuvoIspI2cClient::GetFirmwareVersion() {
    const auto response = Transact(CMD_GET_FWVER, {}, true, options_.normal_response_timeout_ms, true, {}, {});
    if (response.bytes.size() <= 8u) {
        throw std::runtime_error("invalid ISP firmware-version response");
    }
    return response.bytes[8];
}

std::uint32_t NuvoIspI2cClient::GetDeviceId() {
    const auto response = Transact(CMD_GET_DEVICEID, {}, true, options_.normal_response_timeout_ms, true, {}, {});
    if (response.bytes.size() < 12u) {
        throw std::runtime_error("invalid ISP device-id response");
    }
    return ReadLe32(response.bytes, 8u);
}

void NuvoIspI2cClient::ProgramAprom(
    const std::vector<std::uint8_t>& image,
    const std::function<void(const NuvoIspI2cProgress&)>& on_progress,
    const std::function<bool()>& should_cancel,
    const std::function<void()>& on_poll
) {
    if (service_ == nullptr || !service_->IsConnected()) {
        throw std::runtime_error("programming transport is not connected");
    }
    ProgramApromImage(image,
        [&]() { SyncPacketNumber(should_cancel, on_poll); },
        [&](std::uint32_t command, const std::vector<std::uint8_t>& payload, bool first) {
            (void)Transact(command, payload, true,
                first ? options_.long_response_timeout_ms : options_.normal_response_timeout_ms,
                true, should_cancel, on_poll);
        },
        [&](const ApromProgress& p) {
            if (on_progress) {
                on_progress({p.bytes_done, p.total_bytes, p.packet_index});
            }
        }, should_cancel, on_poll);
}

void NuvoIspI2cClient::RunAprom() {
    try {
        SendNoResponse(CMD_RUN_APROM);
    } catch (const std::exception& e) {
        throw ProgrammingOutcomeUnknown(0u, packet_index_, e.what());
    }
}

void NuvoIspI2cClient::ResetTarget() {
    try {
        SendNoResponse(CMD_RESET);
    } catch (const std::exception& e) {
        throw ProgrammingOutcomeUnknown(0u, packet_index_, e.what());
    }
}

NuvoIspI2cResponse NuvoIspI2cClient::Transact(
    std::uint32_t command,
    const std::vector<std::uint8_t>& payload,
    bool check_index,
    int response_timeout_ms,
    bool require_valid,
    const std::function<bool()>& should_cancel,
    const std::function<void()>& on_poll
) {
    if (service_ == nullptr || !service_->IsConnected()) {
        throw std::runtime_error("bridge is not connected");
    }
    if (payload.size() > kPayloadSize) {
        throw std::invalid_argument("ISP payload exceeds 56 bytes");
    }

    ThrowIfCancelled(should_cancel);
    const std::uint32_t sent_index = packet_index_;
    if (sent_index > (std::numeric_limits<std::uint32_t>::max)() - 2u) {
        throw std::overflow_error("ISP packet index exhausted; reconnect before another command");
    }
    const std::vector<std::uint8_t> packet = BuildPacket(command, sent_index, payload);
    const std::uint16_t expected_checksum = Checksum(packet);

    (void)service_->I2cMasterWriteAllowFinalNack(options_.port, options_.addr7, packet);
    packet_index_ += 2u;

    NuvoIspI2cResponse response = ReadResponse(expected_checksum, sent_index + 1u, check_index,
                                               response_timeout_ms, should_cancel, on_poll);
    if (require_valid && (!response.checksum_ok || !response.index_ok)) {
        throw std::runtime_error(ResponseErrorText(response));
    }
    return response;
}

void NuvoIspI2cClient::SendNoResponse(std::uint32_t command) {
    if (service_ == nullptr || !service_->IsConnected()) {
        throw std::runtime_error("bridge is not connected");
    }
    if (packet_index_ > (std::numeric_limits<std::uint32_t>::max)() - 2u) {
        throw std::overflow_error("ISP packet index exhausted; reconnect before another command");
    }
    const std::vector<std::uint8_t> packet = BuildPacket(command, packet_index_, {});
    (void)service_->I2cMasterWriteAllowFinalNack(options_.port, options_.addr7, packet);
    packet_index_ += 2u;
}

NuvoIspI2cResponse NuvoIspI2cClient::ReadResponse(
    std::uint16_t expected_checksum,
    std::uint32_t expected_index,
    bool check_index,
    int response_timeout_ms,
    const std::function<bool()>& should_cancel,
    const std::function<void()>& on_poll
) {
    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(response_timeout_ms);
    const auto poll = std::chrono::milliseconds(options_.response_poll_ms);

    if (options_.response_delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(options_.response_delay_ms));
    }

    while (true) {
        ThrowIfCancelled(should_cancel);
        try {
            std::vector<std::uint8_t> rx = service_->I2cMasterWriteThenReadRaw(options_.port, options_.addr7, false, {}, 64);
            if (rx.size() == kPacketSize && !IsBusyPattern(rx)) {
                NuvoIspI2cResponse response;
                response.bytes = std::move(rx);
                response.expected_checksum = expected_checksum;
                response.response_checksum = static_cast<std::uint16_t>(response.bytes[0] | (response.bytes[1] << 8));
                response.expected_index = expected_index;
                response.response_index = ReadLe32(response.bytes, 4u);
                response.checksum_ok = (response.response_checksum == expected_checksum);
                response.index_ok = !check_index || (response.response_index == expected_index);
                return response;
            }
        } catch (const std::exception&) {
            if (std::chrono::steady_clock::now() - start >= timeout) {
                throw;
            }
        }

        if (on_poll) {
            on_poll();
        }
        if (std::chrono::steady_clock::now() - start >= timeout) {
            throw std::runtime_error("ISP target response timeout");
        }
        std::this_thread::sleep_for(poll);
    }
}


std::vector<std::uint8_t> NuvoIspI2cClient::BuildPacket(
    std::uint32_t command,
    std::uint32_t packet_index,
    const std::vector<std::uint8_t>& payload
) {
    std::vector<std::uint8_t> packet(kPacketSize, 0u);
    packet[0] = static_cast<std::uint8_t>(command & 0xFFu);
    packet[1] = static_cast<std::uint8_t>((command >> 8u) & 0xFFu);
    packet[2] = static_cast<std::uint8_t>((command >> 16u) & 0xFFu);
    packet[3] = static_cast<std::uint8_t>((command >> 24u) & 0xFFu);
    packet[4] = static_cast<std::uint8_t>(packet_index & 0xFFu);
    packet[5] = static_cast<std::uint8_t>((packet_index >> 8u) & 0xFFu);
    packet[6] = static_cast<std::uint8_t>((packet_index >> 16u) & 0xFFu);
    packet[7] = static_cast<std::uint8_t>((packet_index >> 24u) & 0xFFu);
    std::copy(payload.begin(), payload.end(), packet.begin() + static_cast<std::ptrdiff_t>(kHeaderSize));
    return packet;
}

std::uint16_t NuvoIspI2cClient::Checksum(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t sum = 0u;
    for (std::uint8_t b : bytes) {
        sum += b;
    }
    return static_cast<std::uint16_t>(sum & 0xFFFFu);
}

std::uint32_t NuvoIspI2cClient::ReadLe32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4u) {
        throw std::runtime_error("short little-endian word");
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

void NuvoIspI2cClient::AppendLe32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
    out->push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
    out->push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

bool NuvoIspI2cClient::IsBusyPattern(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 2u || bytes[0] != 0xCCu || bytes[1] != 0xDDu) {
        return false;
    }
    const std::size_t check_len = (std::min)(bytes.size(), static_cast<std::size_t>(8u));
    for (std::size_t i = 1u; i < check_len; ++i) {
        if (bytes[i] != 0xDDu) {
            return false;
        }
    }
    return true;
}

std::string NuvoIspI2cClient::ResponseErrorText(const NuvoIspI2cResponse& response) {
    std::ostringstream ss;
    ss << "ISP response validation failed";
    if (!response.checksum_ok) {
        ss << ": checksum expected=0x" << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
           << response.expected_checksum << " actual=0x" << std::setw(4) << response.response_checksum;
    }
    if (!response.index_ok) {
        ss << ": packet index expected=" << std::dec << response.expected_index
           << " actual=" << response.response_index;
    }
    return ss.str();
}

void NuvoIspI2cClient::ThrowIfCancelled(const std::function<bool()>& should_cancel) {
    if (should_cancel && should_cancel()) {
        throw std::runtime_error("operation cancelled by user");
    }
}

} // namespace mfc_tool::core
