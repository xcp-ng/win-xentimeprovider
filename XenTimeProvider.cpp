#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <wil/registry.h>

#include "Globals.hpp"
#include "XenTimeProvider.hpp"
#include "TimeConverter.hpp"

#include "xeniface_ioctls.h"

#define TIME_US(_us) ((_us) * 10)
#define TIME_MS(_ms) (TIME_US((_ms) * 1000))
#define TIME_S(_s) (TIME_MS((_s) * 1000))

XenTimeProvider::XenTimeProvider(_In_ TimeProvSysCallbacks *callbacks) : _callbacks(*callbacks), _worker() {
    UpdateConfig();
}

HRESULT XenTimeProvider::TimeJumped(_In_ TpcTimeJumpedArgs *args) {
    UNREFERENCED_PARAMETER(args);

    Log(LogTimeProvEventTypeInformation, L"TimeJumped");
    _sample = std::nullopt;
    return S_OK;
}

HRESULT XenTimeProvider::GetSamples(_Out_ TpcGetSamplesArgs *args) {
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
    HRESULT hr;

    Log(LogTimeProvEventTypeInformation, L"UpdateConfig");

    _allow_fallback = false;
    _need_fallback = false;
    DWORD value;
    hr = wil::reg::get_value_dword_nothrow(
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\W32Time\\TimeProviders\\" XenTimeProviderName L"\\Parameters",
        L"AllowFallback",
        &value);
    if (SUCCEEDED(hr))
        _allow_fallback = value;
    else if (hr != __HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        return hr;

    return S_OK;
}

HRESULT XenTimeProvider::Shutdown() {
    Log(LogTimeProvEventTypeInformation, L"Shutdown");
    return S_OK;
}

static HRESULT GetXenTime(_In_ HANDLE handle, _Out_ unsigned __int64 *xenTime, _Out_ unsigned __int64 *dispersion) {
    XENIFACE_SHAREDINFO_GET_TIME_OUT buffer;
    DWORD dummy;

    RETURN_IF_WIN32_BOOL_FALSE(DeviceIoControl(
        handle,
        IOCTL_XENIFACE_SHAREDINFO_GET_TIME,
        nullptr,
        0,
        &buffer,
        sizeof(buffer),
        &dummy,
        nullptr));

    if (buffer.Local) {
        FILETIME universalTime;

        RETURN_IF_WIN32_BOOL_FALSE(
            TimeConvertFileTime(&buffer.Time, &universalTime, TimeConvertLocalToUniversal, nullptr));
        auto value = static_cast<unsigned __int64>(universalTime.dwHighDateTime) << 32 |
            static_cast<unsigned __int64>(universalTime.dwLowDateTime);

        *xenTime = value;
        // inherent inaccuracy of TimeConvertFileTime
        *dispersion = TIME_MS(1);
    } else {
        auto value = static_cast<unsigned __int64>(buffer.Time.dwHighDateTime) << 32 |
            static_cast<unsigned __int64>(buffer.Time.dwLowDateTime);

        *xenTime = value;
        *dispersion = 0;
    }

    return S_OK;
}

static HRESULT GetXenHostTime(_In_ HANDLE handle, _Out_ unsigned __int64 *xenTime, _Out_ unsigned __int64 *dispersion) {
    XENIFACE_SHAREDINFO_GET_HOST_TIME_OUT buffer;
    DWORD dummy;

    RETURN_IF_WIN32_BOOL_FALSE(DeviceIoControl(
        handle,
        IOCTL_XENIFACE_SHAREDINFO_GET_HOST_TIME,
        nullptr,
        0,
        &buffer,
        sizeof(buffer),
        &dummy,
        nullptr));

    auto value = static_cast<unsigned __int64>(buffer.Time.dwHighDateTime) << 32 |
        static_cast<unsigned __int64>(buffer.Time.dwLowDateTime);

    *xenTime = value;
    *dispersion = 0;

    return S_OK;
}

HRESULT XenTimeProvider::GetTimeOrFallback(
    _In_ HANDLE handle,
    _Out_ unsigned __int64 *xenTime,
    _Out_ unsigned __int64 *dispersion) {
    if (!_allow_fallback) {
        return GetXenHostTime(handle, xenTime, dispersion);
    } else if (!_need_fallback) {
        auto hr = GetXenHostTime(handle, xenTime, dispersion);
        switch (hr) {
        case __HRESULT_FROM_WIN32(ERROR_INVALID_FUNCTION):
        case __HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED):
            _callbacks.pfnLogTimeProvEvent(
                LogTimeProvEventTypeError,
                const_cast<PWSTR>(XenTimeProviderName),
                const_cast<PWSTR>(L"The Xen PV interface driver has indicated that Xen host time is not supported. "
                                  L"Falling back to guest time; reliability issues are likely."));
            _need_fallback = true;
            // retry right here and not later, just to avoid a prefast warning
            return GetXenTime(handle, xenTime, dispersion);
        default:
            return hr;
        }
    } else {
        return GetXenTime(handle, xenTime, dispersion);
    }
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
    RETURN_IF_FAILED(GetTimeOrFallback(handle, &xenTime, &dispersion));

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
