#pragma once

#include <functional>
#include <utility>

namespace mfc_tool::core {

// UI-thread single-flight ownership, including nested message pumps and error dialogs.
// There are no background jobs or posted heap results in the current programming UI.
class OperationSession {
public:
    void SetOnChange(std::function<void()> callback) { changed_ = std::move(callback); }
    bool Active() const noexcept { return active_; }
    bool Closing() const noexcept { return closing_; }
    bool Cancelled() const noexcept { return cancelled_; }
    bool OutcomeUnknown() const noexcept { return unknown_; }
    void MarkOutcomeUnknown() noexcept { unknown_ = true; }
    void RequestCancel() noexcept { cancelled_ = true; }
    void RequestClose() noexcept { closing_ = true; cancelled_ = true; }

    class Scope {
    public:
        explicit Scope(OperationSession* session, bool writes_target = false) : session_(session) {
            if (session_ == nullptr || session_->active_ || session_->closing_ ||
                (writes_target && session_->unknown_)) {
                session_ = nullptr;
                return;
            }
            session_->active_ = true;
            session_->cancelled_ = false;
            session_->Notify();
        }
        ~Scope() {
            if (session_ != nullptr) {
                session_->active_ = false;
                session_->Notify();
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        explicit operator bool() const noexcept { return session_ != nullptr; }
    private:
        OperationSession* session_;
    };

private:
    void Notify() noexcept {
        try { if (changed_) changed_(); } catch (...) {}
    }
    bool active_ = false;
    bool cancelled_ = false;
    bool closing_ = false;
    bool unknown_ = false;
    std::function<void()> changed_;
};

} // namespace mfc_tool::core
