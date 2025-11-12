#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include "Globals.hpp"
#include "XenTimeProvider.hpp"
#include "TimeConverter.hpp"

#include "xeniface_ioctls.h"

#define TIME_US(_us) ((_us) * 10)
#define TIME_MS(_ms) (TIME_US((_ms) * 1000))
#define TIME_S(_s) (TIME_MS((_s) * 1000))

_Use_decl_annotations_ XenTimeProvider::XenTimeProvider(TimeProvSysCallbacks *callbacks)
    : _callbacks(*callbacks), _worker() {}

_Use_decl_annotations_ HRESULT XenTimeProvider::TimeJumped(TpcTimeJumpedArgs *args) {
    UNREFERENCED_PARAMETER(args);

    Log(LogTimeProvEventTypeInformation, L"TimeJumped");
    _sample = std::nullopt;
    return S_OK;
}

_Use_decl_annotations_ HRESULT XenTimeProvider::GetSamples(TpcGetSamplesArgs *args) {
    HRESULT hr = Update();

    if (FAILED(hr))
        Log(LogTimeProvEventTypeError, L"Update failed: %x", hr);

    if (_sample) {
        args->dwSamplesAvailable = 1;
        if (args->cbSampleBuf < sizeof(TimeSample))
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

        memcpy(args->pbSampleBuf, &*_sample, sizeof(TimeSample));
        args->dwSamplesReturned = 1;
    } else {
        args->dwSamplesAvailable = args->dwSamplesReturned = 0;
    }
    return S_OK;
}

HRESULT XenTimeProvider::PollIntervalChanged() {
    Log(LogTimeProvEventTypeInformation, L"PollIntervalChanged");
    return S_OK;
}

HRESULT XenTimeProvider::UpdateConfig() {
    Log(LogTimeProvEventTypeInformation, L"UpdateConfig");
    return S_OK;
}

HRESULT XenTimeProvider::Shutdown() {
    Log(LogTimeProvEventTypeInformation, L"Shutdown");
    return S_OK;
}

static HRESULT GetXenTime(
    _In_ HANDLE handle,
    _In_ bool getHostTime,
    _Out_ unsigned __int64 *xenTime,
    _Out_ unsigned __int64 *dispersion) {
    XENIFACE_SHAREDINFO_GET_HOST_TIME_OUT time;
    DWORD bytes;

    RETURN_IF_WIN32_BOOL_FALSE(DeviceIoControl(
        handle,
        getHostTime ? IOCTL_XENIFACE_SHAREDINFO_GET_HOST_TIME : IOCTL_XENIFACE_SHAREDINFO_GET_TIME,
        nullptr,
        0,
        &time,
        sizeof(time),
        &bytes,
        nullptr));

    if (!getHostTime) {
        auto value = static_cast<unsigned __int64>(time.Time.dwHighDateTime) << 32 |
            static_cast<unsigned __int64>(time.Time.dwLowDateTime);

        *xenTime = value;
        *dispersion = 0;
    } else {
        FILETIME universalTime;
        RETURN_IF_WIN32_BOOL_FALSE(
            TimeConvertFileTime(&time.Time, &universalTime, TimeConvertLocalToUniversal, nullptr));
        auto value = static_cast<unsigned __int64>(time.Time.dwHighDateTime) << 32 |
            static_cast<unsigned __int64>(time.Time.dwLowDateTime);

        *xenTime = value;
        // inherent inaccuracy of TimeConvertFileTime
        *dispersion = TIME_MS(1);
    }

    return S_OK;
}

HRESULT XenTimeProvider::Update() {
    _sample = std::nullopt;
    auto [lock, handle, path] = _worker.GetDevice();
    if (!handle || handle == INVALID_HANDLE_VALUE)
        return E_PENDING;

    unsigned __int64 tickCount;
    RETURN_IF_FAILED(_callbacks.pfnGetTimeSysInfo(TSI_TickCount, &tickCount));

    signed __int64 phaseOffset;
    RETURN_IF_FAILED(_callbacks.pfnGetTimeSysInfo(TSI_PhaseOffset, &phaseOffset));

    unsigned __int64 begin;
    RETURN_IF_FAILED(_callbacks.pfnGetTimeSysInfo(TSI_CurrentTime, &begin));

    unsigned __int64 xenTime, dispersion;
    if (!_host_time_not_supported) {
        auto hr = GetXenTime(handle, true, &xenTime, &dispersion);
        if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_FUNCTION) || hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) {
            _callbacks.pfnLogTimeProvEvent(
                LogTimeProvEventTypeError,
                const_cast<PWSTR>(XenTimeProviderName),
                const_cast<PWSTR>(L"The Xen PV interface driver has indicated that Xen host time is not supported. "
                                  L"Falling back to guest time; reliability issues are likely."));
            _host_time_not_supported = true;
            // retry right here and not later, otherwise we get a prefast warning
            RETURN_IF_FAILED(GetXenTime(handle, false, &xenTime, &dispersion));
        } else {
            RETURN_IF_FAILED(hr);
        }
    } else {
        RETURN_IF_FAILED(GetXenTime(handle, false, &xenTime, &dispersion));
    }

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
    wcsncpy_s(sample.wszUniqueName, path, _TRUNCATE);
    _sample = sample;

    return S_OK;
}
