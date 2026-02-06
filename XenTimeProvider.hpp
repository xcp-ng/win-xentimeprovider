#pragma once

#include <optional>
#include <memory>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <TimeProv.h>

#include "Logging.hpp"
#include "XenIfaceWorker.hpp"

class XenTimeProvider {
public:
    XenTimeProvider(_In_ TimeProvSysCallbacks *callbacks);
    XenTimeProvider(const XenTimeProvider &) = delete;
    XenTimeProvider &operator=(const XenTimeProvider &) = delete;
    XenTimeProvider(XenTimeProvider &&) = default;
    XenTimeProvider &operator=(XenTimeProvider &&) = default;

    HRESULT TimeJumped(_In_ TpcTimeJumpedArgs *args);
    HRESULT GetSamples(_Out_ TpcGetSamplesArgs *args);
    HRESULT PollIntervalChanged();
    HRESULT UpdateConfig();
    HRESULT Shutdown();

    const TimeProvSysCallbacks &GetCallbacks() {
        return _callbacks;
    }

private:
    void OnResume();
    HRESULT Update();

    void Log(LogTimeProvEventType level, PCWSTR format, ...) {
        va_list args;

        va_start(args, format);
        TimeProvVLog(_callbacks.pfnLogTimeProvEvent, level, format, args);
        va_end(args);
    }

    TimeProvSysCallbacks _callbacks;
    std::unique_ptr<XenIfaceWorker> _worker;
    std::optional<TimeSample> _sample;
};
