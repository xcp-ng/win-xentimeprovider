#include "XenIfaceWorker.hpp"

#include <vector>
#include <format>

#include <wil/result.h>
#include <wil/filesystem.h>

#include "xeniface_ioctls.h"

#define RETURN_IF_CR_FAILED(cr) \
    do { \
        CONFIGRET _cr = (cr); \
        if (_cr != CR_SUCCESS) { \
            return HRESULT_FROM_WIN32(CM_MapCrToWin32Err(_cr, ERROR_GEN_FAILURE)); \
        } \
    } while (0)

static std::vector<std::wstring> ParseMultiStrings(_In_reads_(count) const WCHAR *buf, size_t count) {
    std::vector<std::wstring> strings;
    size_t first = 0;
    for (size_t i = 0; i < count; i++) {
        if (buf[i] == 0) {
            strings.emplace_back(buf + first, i - first);
            first = i + 1;
        }
    }
    if (strings.back().empty()) {
        strings.pop_back();
    }
    return strings;
}

static HRESULT GetDeviceInterfaceList(
    _Out_ std::vector<WCHAR> &list,
    _In_ LPCGUID interfaceClassGuid,
    _In_opt_ DEVINSTID_W deviceID,
    _In_ ULONG flags) {
    ULONG devListLen = 0;
    CONFIGRET cr;

    list.clear();

    do {
        cr = CM_Get_Device_Interface_List_Size(&devListLen, const_cast<LPGUID>(interfaceClassGuid), nullptr, flags);
        if (cr != CR_SUCCESS) {
            auto msg = std::format(L"CM_Get_Device_Interface_List_Size failed {}", cr);
            OutputDebugStringW(msg.c_str());
        }
        RETURN_IF_CR_FAILED(cr);

        list.resize(devListLen);

        cr = CM_Get_Device_Interface_List(
            const_cast<LPGUID>(interfaceClassGuid),
            deviceID,
            list.data(),
            static_cast<ULONG>(list.size()),
            flags);
    } while (cr == CR_BUFFER_SMALL);
    if (cr != CR_SUCCESS) {
        auto msg = std::format(L"CM_Get_Device_Interface_List failed {}", cr);
        OutputDebugStringW(msg.c_str());
    }
    RETURN_IF_CR_FAILED(cr);

    return S_OK;
}

XenIfaceWorker::XenIfaceWorker() : _worker([this](std::stop_token stop) { WorkerFunc(stop); }) {}

XenIfaceWorker::~XenIfaceWorker() {
    _worker.request_stop();
    std::lock_guard lock(_mutex);
    _signal.notify_one();
}

std::tuple<std::unique_lock<std::mutex>, HANDLE> XenIfaceWorker::GetDevice() {
    std::unique_lock lock(_mutex);
    return std::make_tuple(std::move(lock), _device.get());
}

_Use_decl_annotations_ DWORD CALLBACK XenIfaceWorker::CmNotifyCallback(
    HCMNOTIFICATION notifyHandle,
    PVOID context,
    CM_NOTIFY_ACTION action,
    PCM_NOTIFY_EVENT_DATA eventData,
    DWORD eventDataSize) {
    auto self = static_cast<XenIfaceWorker *>(context);
    FAIL_FAST_IF_NULL(self);
    return self->OnCmNotification(notifyHandle, action, eventData, eventDataSize);
}

_Use_decl_annotations_ DWORD XenIfaceWorker::OnCmNotification(
    HCMNOTIFICATION notifyHandle,
    CM_NOTIFY_ACTION action,
    PCM_NOTIFY_EVENT_DATA eventData,
    DWORD eventDataSize) {
    UNREFERENCED_PARAMETER(notifyHandle);
    UNREFERENCED_PARAMETER(eventData);
    UNREFERENCED_PARAMETER(eventDataSize);

    {
        std::lock_guard lock(_mutex);
        switch (action) {
        case CM_NOTIFY_ACTION_DEVICEQUERYREMOVE:
        case CM_NOTIFY_ACTION_DEVICEQUERYREMOVEFAILED: {
            OutputDebugStringW(L"CM_NOTIFY_ACTION_DEVICEQUERYREMOVE");
            _device.reset();
            break;
        }
        }
        _requests.emplace_back(action);
    }
    _signal.notify_one();

    switch (action) {
    case CM_NOTIFY_ACTION_DEVICEREMOVEPENDING:
    case CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE: {
        OutputDebugStringW(L"CM_NOTIFY_ACTION_DEVICEREMOVEPENDING");
        // unregistering _deviceListener can only be done from worker thread
        std::thread([this]() {
            OutputDebugStringW(L"Unregistering listener");
            std::unique_lock lock(_mutex);
            _device.reset();
            _deviceListener.reset();
        }).detach();
        break;
    }
    }

    return ERROR_SUCCESS;
}

_Use_decl_annotations_ HRESULT XenIfaceWorker::RefreshDevices() {
    OutputDebugStringW(L"XenIfaceWorker::RefreshDevices");

    std::vector<WCHAR> buffer;
    auto hr = GetDeviceInterfaceList(buffer, &GUID_INTERFACE_XENIFACE, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (FAILED(hr)) {
        auto msg = std::format(L"GetDeviceInterfaceList failed {}", hr);
        OutputDebugStringW(msg.c_str());
    }
    RETURN_IF_FAILED(hr);

    auto interfaces = ParseMultiStrings(buffer.data(), buffer.size());
    if (interfaces.size() == 0) {
        OutputDebugStringW(L"Interface list empty");
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    OutputDebugStringW(L"Interface list:");
    for (const auto &iface : interfaces) {
        OutputDebugStringW(iface.c_str());
    }

    if (_device.is_valid()) {
        OutputDebugStringW(L"Device valid, skipping refresh");
        return S_FALSE;
    }
    _deviceListener.reset();

    auto [newDevice, err] = wil::try_open_file(interfaces[0].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (!newDevice.is_valid()) {
        auto msg = std::format(L"open({}) failed {}", interfaces[0], err);
        OutputDebugStringW(msg.c_str());
    }
    RETURN_HR_IF(HRESULT_FROM_WIN32(err), !newDevice.is_valid());

    CM_NOTIFY_FILTER filter{
        .cbSize = sizeof(CM_NOTIFY_FILTER),
        .Flags = 0,
        .FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE,
        .Reserved = 0,
        .u = {.DeviceHandle = {.hTarget = newDevice.get()}},
    };
    auto cr = CM_Register_Notification(&filter, this, &XenIfaceWorker::CmNotifyCallback, &_deviceListener);
    if (cr != CR_SUCCESS) {
        auto msg = std::format(L"CM_Register_Notification failed {}", cr);
        OutputDebugStringW(msg.c_str());
    }
    RETURN_IF_CR_FAILED(cr);

    _device = std::move(newDevice);

    return S_OK;
}

void XenIfaceWorker::WorkerFunc(std::stop_token stop) {
    HRESULT hr;

    CM_NOTIFY_FILTER filter{
        .cbSize = sizeof(CM_NOTIFY_FILTER),
        .Flags = 0,
        .FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE,
        .Reserved = 0,
        .u = {.DeviceInterface = {.ClassGuid = GUID_INTERFACE_XENIFACE}},
    };

    wil::unique_hcmnotification cmListener;
    auto cr = CM_Register_Notification(&filter, this, &XenIfaceWorker::CmNotifyCallback, &cmListener);
    if (cr != CR_SUCCESS)
        return;

    {
        std::lock_guard lock(_mutex);
        hr = RefreshDevices();
        if (FAILED(hr)) {
            auto msg = std::format(L"RefreshDevices failed {}", hr);
            OutputDebugStringW(msg.c_str());
        }
    }

    while (1) {
        std::unique_lock lock(_mutex);
        _signal.wait(lock);
        if (stop.stop_requested())
            break;

        while (!_requests.empty()) {
            auto request = _requests.front();
            _requests.pop_front();
            switch (request) {
            case CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL:
                OutputDebugStringW(L"CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL");
                hr = RefreshDevices();
                if (FAILED(hr)) {
                    auto msg = std::format(L"RefreshDevices failed {}", hr);
                    OutputDebugStringW(msg.c_str());
                }
                break;
            }
        }
    }
}
