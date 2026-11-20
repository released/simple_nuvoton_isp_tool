#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <stdexcept>

namespace mfc_tool::hid {

// The caller owns handle, event and buffer until this function returns or throws.
inline DWORD CompleteOverlappedIo(HANDLE handle, OVERLAPPED& ov, int timeout_ms,
                                  const std::function<bool()>& cancel = {},
                                  const std::function<void()>& poll = {}) {
    DWORD transferred = 0u;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds((std::max)(1, timeout_ms));
    try {
        while (true) {
            const DWORD wait = WaitForSingleObject(ov.hEvent, 20u);
            if (wait == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(handle, &ov, &transferred, FALSE)) {
                    throw std::runtime_error("HID overlapped operation failed");
                }
                return transferred;
            }
            if (wait != WAIT_TIMEOUT) throw std::runtime_error("HID I/O wait failed");
            if (poll) poll();
            if (cancel && cancel()) throw std::runtime_error("operation cancelled by user");
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("HID I/O timeout");
            }
        }
    } catch (...) {
        // CancelIoEx is asynchronous; drain before OVERLAPPED/event/buffer may be released.
        CancelIoEx(handle, &ov);
        (void)GetOverlappedResult(handle, &ov, &transferred, TRUE);
        throw;
    }
}

} // namespace mfc_tool::hid
