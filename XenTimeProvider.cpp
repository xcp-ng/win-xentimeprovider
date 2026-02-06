#include <string>
#include <vector>
#include <charconv>

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

#define XENSTORE_PAYLOAD_MAX 4096

XenTimeProvider::XenTimeProvider(_In_ TimeProvSysCallbacks *callbacks) : _callbacks(*callbacks) {
    UpdateConfig();
    _worker = std::make_unique<XenIfaceWorker>();
    _worker->RegisterResume([this] {});
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
    return S_OK;
}

HRESULT XenTimeProvider::UpdateConfig() {
    return S_OK;
}

HRESULT XenTimeProvider::Shutdown() {
    _worker.reset();
    _sample = std::nullopt;
    return S_OK;
}

void XenTimeProvider::OnResume() {
    _callbacks.pfnAlertSamplesAvail();
}

static HRESULT StoreRead(_In_ HANDLE handle, _In_ PCSTR path, _Out_ std::string &out) {
    std::vector<char> buffer(XENSTORE_PAYLOAD_MAX);
    auto pathlen = strlen(path) + 1;
    DWORD size;

    RETURN_IF_WIN32_BOOL_FALSE(DeviceIoControl(
        handle,
        IOCTL_XENIFACE_STORE_READ,
        const_cast<LPVOID>(static_cast<PCVOID>(path)),
        static_cast<DWORD>(pathlen),
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &size,
        nullptr));
    buffer.back() = 0;
    out = std::string(buffer.data());
    return S_OK;
}

static HRESULT StringToInt64(const std::string &str, _Out_ int64_t &val) {
    auto [ptr, ec] = std::from_chars(&*str.begin(), &*str.end(), val);
    if (ptr != &*str.end() || ec != std::errc())
        return E_FAIL;
    return S_OK;
}

static HRESULT
GetXenOffsetTime(_In_ HANDLE handle, _Out_ unsigned __int64 *xenTime, _Out_ unsigned __int64 *dispersion) {
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

    auto value = static_cast<unsigned __int64>(buffer.Time.dwHighDateTime) << 32 |
        static_cast<unsigned __int64>(buffer.Time.dwLowDateTime);

    *xenTime = value;
    *dispersion = 0;

    return S_OK;
}

HRESULT
XenTimeProvider::GetTime(_In_ HANDLE handle, _Out_ unsigned __int64 *xenTime, _Out_ unsigned __int64 *dispersion) {
    unsigned __int64 offsetTime, totalDispersion;
    HRESULT hr;

    std::string vmPath;
    RETURN_IF_FAILED(StoreRead(handle, "vm", vmPath));
    if (vmPath.empty())
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (vmPath.back() != '/')
        vmPath += '/';
    vmPath += "rtc/timeoffset";

    std::string tmp;
    int64_t timeOffsetPre, timeOffsetPost;

    RETURN_IF_FAILED(StoreRead(handle, vmPath.c_str(), tmp));
    RETURN_IF_FAILED(StringToInt64(tmp, timeOffsetPre));

    hr = GetXenOffsetTime(handle, &offsetTime, &totalDispersion);
    RETURN_IF_FAILED(hr);

    RETURN_IF_FAILED(StoreRead(handle, vmPath.c_str(), tmp));
    RETURN_IF_FAILED(StringToInt64(tmp, timeOffsetPost));

    if (timeOffsetPre != timeOffsetPost)
        return E_PENDING;

    *xenTime = offsetTime - TIME_S(timeOffsetPost);
    *dispersion = totalDispersion;
    return S_OK;
}

HRESULT XenTimeProvider::Update() {
    _sample = std::nullopt;

    if (!_worker)
        return E_PENDING;

    auto [lock, handle, path] = _worker->GetDevice();
    if (!handle || handle == INVALID_HANDLE_VALUE)
        return E_PENDING;

    unsigned __int64 tickCount;
    RETURN_IF_FAILED(_callbacks.pfnGetTimeSysInfo(TSI_TickCount, &tickCount));

    signed __int64 phaseOffset;
    RETURN_IF_FAILED(_callbacks.pfnGetTimeSysInfo(TSI_PhaseOffset, &phaseOffset));

    unsigned __int64 begin;
    RETURN_IF_FAILED(_callbacks.pfnGetTimeSysInfo(TSI_CurrentTime, &begin));

    unsigned __int64 xenTime, dispersion;
    RETURN_IF_FAILED(GetTime(handle, &xenTime, &dispersion));

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
