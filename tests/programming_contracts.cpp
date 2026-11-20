#include "../src/core/aprom_programmer.h"
#include "../src/core/firmware_image.h"
#include "../src/core/nuvo_isp_uart.h"
#include "../src/core/operation_session.h"
#include "../src/core/text_utils.h"
#include "../src/core/xmodem_sender.h"
#include "../src/hid/overlapped_io.h"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

using namespace mfc_tool::core;
using Bytes = std::vector<std::uint8_t>;

namespace {

int checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

template<class Error = std::exception, class Action>
void Reject(Action action, const char* message) {
    bool caught = false;
    try { action(); } catch (const Error&) { caught = true; }
    Check(caught, message);
}

std::uint32_t Word(const Bytes& bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes.at(at)) |
        (static_cast<std::uint32_t>(bytes.at(at + 1u)) << 8u) |
        (static_cast<std::uint32_t>(bytes.at(at + 2u)) << 16u) |
        (static_cast<std::uint32_t>(bytes.at(at + 3u)) << 24u);
}

class SyntheticSerial final : public mfc_tool::vcom::SerialTransport {
public:
    bool Open(const std::wstring&, std::uint32_t, std::wstring*) override { open = true; return true; }
    void Close() override { open = false; }
    bool IsOpen() const override { return open; }
    bool Purge(std::wstring*) override { return true; }
    bool WriteAll(const std::uint8_t* data, std::size_t count, std::uint32_t, std::wstring* error) override {
        sent.emplace_back(data, data + count);
        if (fail_write == sent.size()) {
            if (error) *error = L"synthetic partial write";
            return false;
        }
        if (isp && count == 64u) {
            const auto& packet = sent.back();
            const auto cmd = Word(packet, 0u);
            if (cmd == 0xABu || cmd == 0xADu) return true;
            Bytes reply(64u, 0u);
            auto sum = std::accumulate(packet.begin(), packet.end(), 0u);
            if (corrupt_checksum == sent.size()) ++sum;
            reply[0] = static_cast<std::uint8_t>(sum);
            reply[1] = static_cast<std::uint8_t>(sum >> 8u);
            auto index = Word(packet, 4u) + 1u;
            if (corrupt_index == sent.size()) index += 2u;
            for (unsigned i = 0u; i < 4u; ++i) reply[4u + i] = static_cast<std::uint8_t>(index >> (8u * i));
            if (drop_reply == sent.size()) {
                throw std::runtime_error("synthetic accepted write, reply lost");
            }
            incoming.insert(incoming.end(), reply.begin(), reply.end());
        }
        return true;
    }
    bool ReadSome(Bytes* bytes, std::size_t count, std::uint32_t, std::wstring* error) override {
        if (incoming.empty()) {
            if (error) *error = empty_is_timeout ? L"Read timeout." : L"synthetic disconnected";
            return false;
        }
        bytes->clear();
        // Fragment all ISP replies; test actual UART reassembly rather than prebuilt responses.
        count = (std::min)(count, isp ? std::size_t{7} : std::size_t{1});
        while (count-- && !incoming.empty()) {
            bytes->push_back(incoming.front()); incoming.pop_front();
        }
        return true;
    }
    bool open = false;
    bool isp = true;
    bool empty_is_timeout = false;
    std::size_t fail_write = 0u, drop_reply = 0u, corrupt_checksum = 0u, corrupt_index = 0u;
    std::deque<std::uint8_t> incoming;
    std::vector<Bytes> sent;
};

void NumericAndImageTests(const std::filesystem::path& work) {
    Check(ParseInt(L"2147483647") == INT_MAX, "INT_MAX");
    Check(ParseInt(L"-2147483648") == INT_MIN, "INT_MIN");
    Check(ParseInt(L"0xFFFF") == 65535, "hex parsing");
    for (const auto* input : {L"2147483648", L"-2147483649", L"0xFFFFFFFF", L"999999999999999999999999", L"-", L"0x"}) {
        Reject([&] { (void)ParseInt(input); }, "invalid/overflow integer accepted");
    }
    Check(CheckedImageLength(kMaxIspImageBytes, kMaxIspImageBytes) == kMaxIspImageBytes, "ISP exact limit");
    Reject([&] { (void)CheckedImageLength(0, kMaxIspImageBytes); }, "empty image");
    Reject([&] { (void)CheckedImageLength(kMaxIspImageBytes + 1u, kMaxIspImageBytes); }, "ISP oversize");
    Reject([&] { (void)CheckedImageLength(std::uint64_t{1} << 32u, UINT64_MAX); }, "32-bit length wrap");
    CheckImageAddressRange(UINT32_MAX, 1u);
    Reject([&] { CheckImageAddressRange(UINT32_MAX, 2u); }, "address wrap");
    const auto file = work / "synthetic-image.bin";
    { std::ofstream out(file, std::ios::binary); out.write("TEST_ONLY", 9); }
    Check(ReadFirmwareImage(file) == Bytes({'T','E','S','T','_','O','N','L','Y'}), "image bytes");
    std::filesystem::resize_file(file, 0u);
    Reject([&] { (void)ReadFirmwareImage(file); }, "empty file");
    std::filesystem::resize_file(file, kMaxLoadedImageBytes + 1u);
    Reject([&] { (void)ReadFirmwareImage(file); }, "load budget");
    std::filesystem::remove(file);
    Reject([&] { (void)ReadFirmwareImage(file); }, "missing file");
}

void ApromTests() {
    for (const std::size_t size : {1u, 47u, 48u, 49u, 103u, 104u, 105u, 4097u, 1048576u}) {
        SyntheticSerial wire;
        NuvoIspUartClient client(wire);
        client.Connect(L"SYNTHETIC_ONLY", 115200u, 100);
        Bytes image(size);
        for (std::size_t i = 0; i < size; ++i) image[i] = static_cast<std::uint8_t>(i);
        std::uint32_t confirmed = 0u;
        client.ProgramAprom(image, [&](const NuvoIspUartProgress& p) { confirmed = p.bytes_done; }, {}, {});
        Check(confirmed == size, "progress exact length");
        Check(Word(wire.sent.at(0), 0) == 0xA4u, "sync first");
        Check(Word(wire.sent.at(1), 0) == 0xA0u, "UPDATE_APROM command");
        Check(Word(wire.sent.at(1), 8) == 0u, "bootloader-managed address zero");
        Check(Word(wire.sent.at(1), 12) == size, "image length on wire");
        Bytes recovered;
        for (std::size_t i = 1u; i < wire.sent.size(); ++i) {
            Check(Word(wire.sent[i], 4) == 1u + 2u * static_cast<std::uint32_t>(i), "packet identity");
            if (i > 1u) Check(Word(wire.sent[i], 0) == 0u, "continuation command");
            const std::size_t header = i == 1u ? 16u : 8u;
            const auto take = (std::min)(64u - header, image.size() - recovered.size());
            recovered.insert(recovered.end(), wire.sent[i].begin() + header, wire.sent[i].begin() + header + take);
        }
        Check(recovered == image, "transport reconstructs exact image");
    }
    for (int fault = 0; fault < 4; ++fault) {
        SyntheticSerial wire;
        if (fault == 0) wire.drop_reply = 3u;
        if (fault == 1) wire.corrupt_checksum = 3u;
        if (fault == 2) wire.corrupt_index = 3u;
        if (fault == 3) wire.fail_write = 3u;
        NuvoIspUartClient client(wire);
        client.Connect(L"SYNTHETIC_ONLY", 115200u, 100);
        bool unknown = false;
        try { client.ProgramAprom(Bytes(200u, 0x5Au), {}, {}, {}); }
        catch (const ProgrammingOutcomeUnknown& e) {
            unknown = true;
            Check(e.confirmed_bytes == 48u && e.attempted_packet == 2u, "unknown retains confirmed phase");
        }
        Check(unknown, "ambiguous write must be unknown");
        Check(wire.sent.size() == 3u, "no replay or RESEND_PACKET after ambiguous write");
    }
    SyntheticSerial wire;
    NuvoIspUartClient client(wire);
    client.Connect(L"SYNTHETIC_ONLY", 115200u, 100);
    Reject([&] { client.ProgramAprom({}, {}, {}, {}); }, "empty sends nothing");
    Check(wire.sent.empty(), "validation before sync/write");
    Reject([&] { client.ProgramAprom(Bytes(1u), {}, [] { return true; }, {}); }, "cancel before write");
    Check(wire.sent.empty(), "cancel sends nothing");
    bool cancel = false;
    Reject([&] {
        client.ProgramAprom(Bytes(200u), [&](const NuvoIspUartProgress&) { cancel = true; }, [&] { return cancel; }, {});
    }, "cancel after acknowledged phase");
    Check(wire.sent.size() == 2u, "cancel prevents following block");
    client.RunAprom(); client.ResetTarget();
    Check(Word(wire.sent[2], 0) == 0xABu && Word(wire.sent[3], 0) == 0xADu, "run/reset IDs retained");
    cancel = false;
    client.ProgramAprom(Bytes(1u), [&](const NuvoIspUartProgress&) { cancel = true; }, [&] { return cancel; }, {});
    Check(cancel, "late cancellation does not undo fully acknowledged success");
}

void XmodemTests() {
    SyntheticSerial wire;
    wire.isp = false;
    XmodemSender sender(wire);
    sender.Connect(L"SYNTHETIC_ONLY", 115200u);
    wire.incoming = {'C', 'C', 0x15u, 0x06u, 0x06u};
    sender.Send(Bytes(128u, 0u), {}, {}, {});
    Check(wire.sent.size() == 3u, "XMODEM retry then EOT");
    Check(wire.sent[0] == wire.sent[1], "XMODEM duplicate uses identical sequence/payload");
    Check(wire.sent[0].size() == 133u && wire.sent[0][1] == 1u && wire.sent[0][2] == 0xFEu, "SOH CRC framing");
    Check(wire.sent[0][131] == 0u && wire.sent[0][132] == 0u, "independent all-zero CRC vector");
    Check(wire.sent[2] == Bytes{4u}, "EOT retained");
    wire.sent.clear(); wire.incoming = {0x15u, 0x15u, 0x06u, 0x06u};
    sender.Send(Bytes{1u}, {}, {}, {});
    Check(wire.sent[0].size() == 132u && wire.sent[0][131] == 0x1Bu, "checksum and CTRL-Z padding");
    wire.sent.clear(); wire.incoming = {'C','C'};
    Reject<ProgrammingOutcomeUnknown>([&] { sender.Send(Bytes(128u), {}, {}, {}); }, "lost XMODEM response unknown");
    Check(wire.sent.size() == 1u, "disconnect never restarts transfer");
    wire.sent.clear(); wire.incoming = {'C','C'};
    wire.incoming.insert(wire.incoming.end(), 258u, 0x06u);
    sender.Send(Bytes(257u * 128u), {}, {}, {});
    Check(wire.sent[255][1] == 0u && wire.sent[256][1] == 1u, "XMODEM sequence wraps by protocol");
    wire.sent.clear(); wire.incoming = {'C','C',0x06u};
    Reject<ProgrammingOutcomeUnknown>([&] { sender.Send(Bytes(128u), {}, {}, {}); }, "lost EOT response unknown");
    Check(wire.sent.size() == 2u && wire.sent[1] == Bytes{4u}, "unconfirmed EOT does not replay image");
    wire.sent.clear(); wire.incoming = {'C','C'}; wire.empty_is_timeout = true;
    bool cancel = false;
    Reject<ProgrammingOutcomeUnknown>([&] {
        sender.Send(Bytes(128u), {}, [&] { return cancel; }, [&] { cancel = true; });
    }, "cancel of unacknowledged block stays unknown, not cancelled-success");
    Check(wire.sent.size() == 1u, "pending block cancellation cannot restart transfer");
}

struct OwnedHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~OwnedHandle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
};

void OverlappedLifetimeTests() {
    for (int mode = 0; mode < 4; ++mode) {
        const auto name = L"\\\\.\\pipe\\isp-contract-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(mode);
        OwnedHandle server{CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1u, 64u, 64u, 0u, nullptr)};
        Check(server.value != INVALID_HANDLE_VALUE, "synthetic local pipe");
        OwnedHandle client{CreateFileW(name.c_str(), GENERIC_WRITE, 0u, nullptr, OPEN_EXISTING, 0u, nullptr)};
        Check(client.value != INVALID_HANDLE_VALUE, "synthetic writer");
        OwnedHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        Check(event.value != nullptr, "owned I/O event");
        OVERLAPPED ov = {};
        ov.hEvent = event.value;
        std::uint8_t byte = 0u;
        Check(!ReadFile(server.value, &byte, 1u, nullptr, &ov) && GetLastError() == ERROR_IO_PENDING, "read is genuinely pending");
        if (mode == 0) {
            const std::uint8_t expected = 0x5Au;
            DWORD written = 0u;
            Check(WriteFile(client.value, &expected, 1u, &written, nullptr) && written == 1u, "synthetic completion");
            Check(mfc_tool::hid::CompleteOverlappedIo(server.value, ov, 1000) == 1u && byte == expected, "I/O success retained");
        } else {
            Reject([&] {
                (void)mfc_tool::hid::CompleteOverlappedIo(server.value, ov, 30,
                    [&] { return mode == 2; }, [&] { if (mode == 3) throw std::runtime_error("synthetic poll failure"); });
            }, "timeout/cancel/poll failure");
            DWORD transferred = 0u;
            Check(!GetOverlappedResult(server.value, &ov, &transferred, FALSE) && GetLastError() == ERROR_OPERATION_ABORTED,
                  "failure drains OS request before event/buffer lifetime ends");
        }
    }
}

void LifetimeTests() {
    OperationSession session;
    int notifications = 0;
    session.SetOnChange([&] { ++notifications; });
    {
        OperationSession::Scope first(&session);
        Check(static_cast<bool>(first) && session.Active(), "single-flight acquired");
        OperationSession::Scope duplicate(&session);
        Check(!duplicate, "double trigger rejected");
        session.RequestClose();
        Check(session.Active() && session.Cancelled(), "close does not release live operation");
    }
    Check(!session.Active() && session.Closing() && notifications == 2, "completion includes close boundary");
    OperationSession::Scope late(&session);
    Check(!late, "closing cannot start new work");
    OperationSession unknown;
    unknown.MarkOutcomeUnknown();
    { OperationSession::Scope repeat(&unknown, true); Check(!repeat, "unknown prohibits subsequent writes"); }
    { OperationSession::Scope reconnect(&unknown); Check(static_cast<bool>(reconnect), "read/connect still available"); }
    { OperationSession::Scope repeat(&unknown, true); Check(!repeat, "reconnect does not grant replay authority"); }
    OperationSession failed_start;
    failed_start.SetOnChange([] { throw std::runtime_error("notification unavailable"); });
    { OperationSession::Scope job(&failed_start); Check(failed_start.Active(), "notification failure owned"); }
    Check(!failed_start.Active(), "failed notification cannot leak operation lifetime");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected isolated synthetic work directory");
        NumericAndImageTests(argv[1]);
        ApromTests(); XmodemTests(); LifetimeTests(); OverlappedLifetimeTests();
        std::cout << "PASS: " << checks << " synthetic contract assertions; no hardware or private fixtures\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
