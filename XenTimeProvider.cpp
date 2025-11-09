#include "XenTimeProvider.hpp"

#include <winioctl.h>

#include "xeniface_ioctls.h"

#define TIME_US(_us) ((_us) * 10)
#define TIME_MS(_ms) (TIME_US((_ms) * 1000))
#define TIME_S(_s) (TIME_MS((_s) * 1000))

enum LogTimeProvEventType : WORD {
    LogTimeProvEventTypeError = 1,
    LogTimeProvEventTypeWarning = 2,
    LogTimeProvEventTypeInformation = 3,
};

_Use_decl_annotations_ XenTimeProvider::XenTimeProvider(TimeProvSysCallbacks *callbacks)
    : _callbacks(*callbacks), _worker() {}

_Use_decl_annotations_ HRESULT XenTimeProvider::TimeJumped(TpcTimeJumpedArgs *args) {
    UNREFERENCED_PARAMETER(args);

    _callbacks.pfnLogTimeProvEvent(
        LogTimeProvEventTypeInformation,
        const_cast<PWSTR>(XenTimeProviderName),
        const_cast<PWSTR>(L"XenTimeProvider::TimeJumped"));
    _sample = std::nullopt;
    return S_OK;
}

_Use_decl_annotations_ HRESULT XenTimeProvider::GetSamples(TpcGetSamplesArgs *args) {
    Update();

    args->dwSamplesAvailable = _sample.has_value() ? 1 : 0;

    args->dwSamplesReturned = 0;
    if (args->cbSampleBuf < sizeof(TimeSample))
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

    if (_sample.has_value()) {
        memcpy(args->pbSampleBuf, &_sample.value(), sizeof(TimeSample));
        args->dwSamplesReturned = 1;
    }
    return S_OK;
}

HRESULT XenTimeProvider::PollIntervalChanged() {
    _callbacks.pfnLogTimeProvEvent(
        LogTimeProvEventTypeInformation,
        const_cast<PWSTR>(XenTimeProviderName),
        const_cast<PWSTR>(L"XenTimeProvider::PollIntervalChanged"));
    return S_OK;
}

HRESULT XenTimeProvider::UpdateConfig() {
    _callbacks.pfnLogTimeProvEvent(
        LogTimeProvEventTypeInformation,
        const_cast<PWSTR>(XenTimeProviderName),
        const_cast<PWSTR>(L"XenTimeProvider::UpdateConfig"));
    return S_OK;
}

HRESULT XenTimeProvider::Shutdown() {
    _callbacks.pfnLogTimeProvEvent(
        LogTimeProvEventTypeInformation,
        const_cast<PWSTR>(XenTimeProviderName),
        const_cast<PWSTR>(L"XenTimeProvider::Shutdown"));
    return S_OK;
}

static HRESULT GetXenHostTime(_In_ HANDLE handle, _Out_ unsigned __int64 *xenTime, _Out_ unsigned __int64 *dispersion) {
    XENIFACE_SHAREDINFO_GET_HOST_TIME_OUT time;
    DWORD bytes;

    RETURN_IF_WIN32_BOOL_FALSE(DeviceIoControl(
        handle,
        IOCTL_XENIFACE_SHAREDINFO_GET_HOST_TIME,
        nullptr,
        0,
        &time,
        sizeof(time),
        &bytes,
        nullptr));

    auto value = static_cast<unsigned __int64>(time.Time.dwHighDateTime) << 32 |
        static_cast<unsigned __int64>(time.Time.dwLowDateTime);

    *xenTime = value;
    *dispersion = 0;

    return S_OK;
}

HRESULT XenTimeProvider::Update() {
    _sample = std::nullopt;
    auto [lock, handle] = _worker.GetDevice();
    if (!handle || handle == INVALID_HANDLE_VALUE)
        return E_PENDING;

    unsigned __int64 tickCount;
    RETURN_IF_FAILED(_callbacks.pfnGetTimeSysInfo(TSI_TickCount, &tickCount));

    signed __int64 phaseOffset;
    RETURN_IF_FAILED(_callbacks.pfnGetTimeSysInfo(TSI_PhaseOffset, &phaseOffset));

    unsigned __int64 begin;
    RETURN_IF_FAILED(_callbacks.pfnGetTimeSysInfo(TSI_CurrentTime, &begin));

    unsigned __int64 xenTime, dispersion;
    RETURN_IF_FAILED(GetXenHostTime(handle, &xenTime, &dispersion));

    unsigned __int64 end;
    RETURN_IF_FAILED(_callbacks.pfnGetTimeSysInfo(TSI_CurrentTime, &end));

    signed __int64 delay = end - begin;
    if (delay < 0)
        delay = 0;

    TimeSample sample{
        .dwSize = sizeof(TimeSample),
        .dwRefid = ' NEX',
        .toOffset = static_cast<signed __int64>(xenTime - begin + delay / 2),
        .toDelay = delay,
        .tpDispersion = dispersion,
        .nSysTickCount = tickCount,
        .nSysPhaseOffset = phaseOffset,
        .nLeapFlags = 3,
        .nStratum = 0,
        .dwTSFlags = TSF_Hardware,
    };
    auto devicePath = _worker.GetDevicePath();
    FAIL_FAST_IF(wcsncpy_s(sample.wszUniqueName, devicePath.c_str(), _TRUNCATE));
    _sample = sample;

    return S_OK;
}
