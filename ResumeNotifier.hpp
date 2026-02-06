#pragma once

#include <cassert>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <wil/resource.h>

#include "xeniface_ioctls.h"
#include "Borrowed.hpp"

class ResumeNotifier {
public:
    ResumeNotifier() = default;
    ResumeNotifier(HANDLE borrowed, wistd::function<void()> &&callback) : _borrowed(borrowed) {
        _watcher.create(
            wil::unique_event(wil::EventOptions::ManualReset),
            std::forward<wistd::function<void()>>(callback));

        XENIFACE_SUSPEND_REGISTER_IN in{_watcher.get_event().get()};

        DWORD dummy;
        THROW_IF_WIN32_BOOL_FALSE(DeviceIoControl(
            _borrowed.Get(),
            IOCTL_XENIFACE_SUSPEND_REGISTER,
            &in,
            sizeof(in),
            &_out,
            sizeof(_out),
            &dummy,
            nullptr));
    }

    ResumeNotifier(const ResumeNotifier &) = delete;
    ResumeNotifier &operator=(const ResumeNotifier &) = delete;
    ResumeNotifier(ResumeNotifier &&other) noexcept {
        swap(*this, other);
    }
    ResumeNotifier &operator=(ResumeNotifier &&other) noexcept {
        if (this != std::addressof(other)) {
            Dispose();
            swap(*this, other);
        }
        return *this;
    }
    ~ResumeNotifier() {
        Dispose();
    }
    void Reset() noexcept {
        Dispose();
    }

    friend void swap(ResumeNotifier &self, ResumeNotifier &other) noexcept {
        using std::swap;
        swap(self._borrowed, other._borrowed);
        swap(self._watcher, other._watcher);
        swap(self._out, other._out);
    }

private:
    void Dispose() noexcept {
        if (_borrowed) {
            DWORD dummy;
            DeviceIoControl(
                _borrowed.Get(),
                IOCTL_XENIFACE_SUSPEND_DEREGISTER,
                &_out,
                sizeof(_out),
                nullptr,
                0,
                &dummy,
                nullptr);
        }
        _out = XENIFACE_SUSPEND_REGISTER_OUT{};
        _watcher.reset();
        _borrowed.Reset();
    }

    Borrowed<HANDLE> _borrowed;
    wil::unique_event_watcher _watcher;
    XENIFACE_SUSPEND_REGISTER_OUT _out{};
};
