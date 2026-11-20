#include "nuvo_isp_uart.h"

#include <algorithm>
#include "aprom_programmer.h"
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

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

void NuvoIspUartClient::Connect(const std::wstring& com_port, std::uint32_t baudrate, int timeout_ms) {
    Disconnect();
    if (com_port.empty()) {
        throw std::runtime_error("no COM port selected");
    }

    std::wstring error;
    if (!port_->Open(com_port, baudrate, &error)) {
        throw std::runtime_error("open UART failed: " + WideToNarrow(error));
    }

    current_port_ = com_port;
    baudrate_ = baudrate;
    timeout_ms_ = (std::max)(100, timeout_ms);
    packet_index_ = 18u;
}

void NuvoIspUartClient::Disconnect() {
    port_->Close();
    current_port_.clear();
    packet_index_ = 18u;
}

bool NuvoIspUartClient::IsConnected() const noexcept {
    return port_->IsOpen();
}

void NuvoIspUartClient::SetOptions(const NuvoIspUartOptions& options) {
    options_ = options;
    options_.response_poll_ms = std::clamp(options_.response_poll_ms, 1, 1000);
    options_.normal_response_timeout_ms = std::clamp(options_.normal_response_timeout_ms, 100, 60000);
    options_.long_response_timeout_ms = std::clamp(options_.long_response_timeout_ms, 1000, 120000);
    options_.program_retry_count = std::clamp(options_.program_retry_count, 1, 50);
}

void NuvoIspUartClient::ConnectTarget(const std::function<bool()>& should_cancel,
                                      const std::function<void()>& on_poll) {
    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(options_.long_response_timeout_ms);
    const auto retry_sleep = std::chrono::milliseconds((std::max)(20, options_.response_poll_ms));
    const int attempt_timeout_ms = (std::min)(options_.normal_response_timeout_ms, 300);
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

    throw std::runtime_error("UART ISP target handshake timeout: " + last_error);
}

void NuvoIspUartClient::SyncPacketNumber(const std::function<bool()>& should_cancel,
                                         const std::function<void()>& on_poll) {
    std::vector<std::uint8_t> payload;
    packet_index_ = 1u;
    AppendLe32(&payload, packet_index_);
    (void)Transact(CMD_SYNC_PACKNO, payload, false, options_.normal_response_timeout_ms, true, should_cancel, on_poll);
}

std::uint8_t NuvoIspUartClient::GetFirmwareVersion() {
    const auto response = Transact(CMD_GET_FWVER, {}, true, options_.normal_response_timeout_ms, true, {}, {});
    if (response.bytes.size() <= 8u) {
        throw std::runtime_error("invalid ISP firmware-version response");
    }
    return response.bytes[8];
}

std::uint32_t NuvoIspUartClient::GetDeviceId() {
    const auto response = Transact(CMD_GET_DEVICEID, {}, true, options_.normal_response_timeout_ms, true, {}, {});
    if (response.bytes.size() < 12u) {
        throw std::runtime_error("invalid ISP device-id response");
    }
    return ReadLe32(response.bytes, 8u);
}

void NuvoIspUartClient::ProgramAprom(
    const std::vector<std::uint8_t>& image,
    const std::function<void(const NuvoIspUartProgress&)>& on_progress,
    const std::function<bool()>& should_cancel,
    const std::function<void()>& on_poll
) {
    if (!IsConnected()) {
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

void NuvoIspUartClient::RunAprom() {
    try {
        SendNoResponse(CMD_RUN_APROM);
    } catch (const std::exception& e) {
        throw ProgrammingOutcomeUnknown(0u, packet_index_, e.what());
    }
}

void NuvoIspUartClient::ResetTarget() {
    try {
        SendNoResponse(CMD_RESET);
    } catch (const std::exception& e) {
        throw ProgrammingOutcomeUnknown(0u, packet_index_, e.what());
    }
}

NuvoIspUartResponse NuvoIspUartClient::Transact(
    std::uint32_t command,
    const std::vector<std::uint8_t>& payload,
    bool check_index,
    int response_timeout_ms,
    bool require_valid,
    const std::function<bool()>& should_cancel,
    const std::function<void()>& on_poll
) {
    if (!IsConnected()) {
        throw std::runtime_error("UART is not connected");
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

    std::wstring error;
    if (!port_->WriteAll(packet.data(), packet.size(), static_cast<std::uint32_t>(timeout_ms_), &error)) {
        throw std::runtime_error("UART write failed: " + WideToNarrow(error));
    }
    packet_index_ += 2u;

    std::vector<std::uint8_t> rx = ReadExact(kPacketSize, response_timeout_ms, should_cancel, on_poll);
    NuvoIspUartResponse response;
    response.bytes = std::move(rx);
    response.expected_checksum = expected_checksum;
    response.response_checksum = static_cast<std::uint16_t>(response.bytes[0] | (response.bytes[1] << 8));
    response.expected_index = sent_index + 1u;
    response.response_index = ReadLe32(response.bytes, 4u);
    response.checksum_ok = (response.response_checksum == expected_checksum);
    response.index_ok = !check_index || (response.response_index == response.expected_index);
    if (require_valid && (!response.checksum_ok || !response.index_ok)) {
        throw std::runtime_error(ResponseErrorText(response));
    }
    return response;
}

void NuvoIspUartClient::SendNoResponse(std::uint32_t command) {
    if (!IsConnected()) {
        throw std::runtime_error("UART is not connected");
    }
    if (packet_index_ > (std::numeric_limits<std::uint32_t>::max)() - 2u) {
        throw std::overflow_error("ISP packet index exhausted; reconnect before another command");
    }
    const std::vector<std::uint8_t> packet = BuildPacket(command, packet_index_, {});
    std::wstring error;
    if (!port_->WriteAll(packet.data(), packet.size(), static_cast<std::uint32_t>(timeout_ms_), &error)) {
        throw std::runtime_error("UART write failed: " + WideToNarrow(error));
    }
    packet_index_ += 2u;
}

std::vector<std::uint8_t> NuvoIspUartClient::ReadExact(
    std::size_t length,
    int timeout_ms,
    const std::function<bool()>& should_cancel,
    const std::function<void()>& on_poll
) {
    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(timeout_ms);
    std::vector<std::uint8_t> out;
    out.reserve(length);

    while (out.size() < length) {
        ThrowIfCancelled(should_cancel);

        const auto now = std::chrono::steady_clock::now();
        if (now - start >= timeout) {
            throw std::runtime_error("UART ISP response timeout");
        }

        std::vector<std::uint8_t> chunk;
        std::wstring error;
        const std::size_t want = (std::min)(length - out.size(), static_cast<std::size_t>(64u));
        if (port_->ReadSome(&chunk, want, static_cast<std::uint32_t>(options_.response_poll_ms), &error)) {
            out.insert(out.end(), chunk.begin(), chunk.end());
        } else if (error != L"Read timeout.") {
            throw std::runtime_error("UART read failed: " + WideToNarrow(error));
        }

        if (on_poll) {
            on_poll();
        }
    }

    return out;
}


std::vector<std::uint8_t> NuvoIspUartClient::BuildPacket(
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

std::uint16_t NuvoIspUartClient::Checksum(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t sum = 0u;
    for (std::uint8_t b : bytes) {
        sum += b;
    }
    return static_cast<std::uint16_t>(sum & 0xFFFFu);
}

std::uint32_t NuvoIspUartClient::ReadLe32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4u) {
        throw std::runtime_error("short little-endian word");
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

void NuvoIspUartClient::AppendLe32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
    out->push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
    out->push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

std::string NuvoIspUartClient::ResponseErrorText(const NuvoIspUartResponse& response) {
    std::ostringstream ss;
    ss << "UART ISP response validation failed";
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

std::string NuvoIspUartClient::WideToNarrow(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        out.push_back(ch >= 0 && ch <= 0x7F ? static_cast<char>(ch) : '?');
    }
    return out;
}

void NuvoIspUartClient::ThrowIfCancelled(const std::function<bool()>& should_cancel) {
    if (should_cancel && should_cancel()) {
        throw std::runtime_error("operation cancelled by user");
    }
}

} // namespace mfc_tool::core
