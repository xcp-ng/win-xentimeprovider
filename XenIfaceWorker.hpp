#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <list>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cfgmgr32.h>

#include <wil/resource.h>

class XenIfaceWorker {
public:
    XenIfaceWorker();
    XenIfaceWorker(const XenIfaceWorker &) = delete;
    XenIfaceWorker &operator=(const XenIfaceWorker &) = delete;

    std::tuple<std::unique_lock<std::mutex>, HANDLE> GetDevice();

    ~XenIfaceWorker();

private:
    void WorkerFunc(std::stop_token stop);
    _Requires_lock_held_(_mutex) HRESULT RefreshDevices();

    _Pre_satisfies_(eventDataSize >= sizeof(CM_NOTIFY_EVENT_DATA)) static DWORD CALLBACK CmNotifyCallback(
        _In_ HCMNOTIFICATION notifyHandle,
        _In_opt_ PVOID context,
        _In_ CM_NOTIFY_ACTION action,
        _In_reads_bytes_(eventDataSize) PCM_NOTIFY_EVENT_DATA eventData,
        _In_ DWORD eventDataSize);

    _Pre_satisfies_(eventDataSize >= sizeof(CM_NOTIFY_EVENT_DATA)) DWORD OnCmNotification(
        _In_ HCMNOTIFICATION notifyHandle,
        _In_ CM_NOTIFY_ACTION action,
        _In_reads_bytes_(eventDataSize) PCM_NOTIFY_EVENT_DATA eventData,
        _In_ DWORD eventDataSize);

    struct {
        std::mutex _mutex;
        std::condition_variable _signal;
        _Guarded_by_(_mutex) std::list<CM_NOTIFY_ACTION> _requests;
        _Guarded_by_(_mutex) wil::unique_hcmnotification _deviceListener;
        _Guarded_by_(_mutex) wil::unique_hfile _device;
    };
    std::jthread _worker;
};
