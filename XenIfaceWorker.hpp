#pragma once

#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <list>
#include <string>
#include <tuple>
#include <functional>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cfgmgr32.h>

#include <wil/resource.h>

#include "ResumeNotifier.hpp"

class XenIfaceWorker {
public:
    XenIfaceWorker();
    ~XenIfaceWorker();
    XenIfaceWorker(const XenIfaceWorker &) = delete;
    XenIfaceWorker &operator=(const XenIfaceWorker &) = delete;

    std::tuple<std::unique_lock<std::mutex>, HANDLE, PCWSTR> GetDevice();
    void RegisterResume(std::function<void()> &&callback);

private:
    class XenIfaceDevice : public std::enable_shared_from_this<XenIfaceDevice> {
    private:
        struct Private {
            explicit Private() = default;
        };

    public:
        XenIfaceDevice(
            _In_ Private pvt,
            _In_ wil::unique_hfile &&handle,
            _In_ const std::wstring &path,
            _In_ XenIfaceWorker *worker);

        static HRESULT make(
            _Out_ std::shared_ptr<XenIfaceDevice> &device,
            _In_ wil::unique_hfile &&handle,
            _In_ const std::wstring &path,
            _In_ XenIfaceWorker *worker);

        XenIfaceDevice(const XenIfaceDevice &) = delete;
        XenIfaceDevice &operator=(const XenIfaceDevice &) = delete;
        XenIfaceDevice(XenIfaceDevice &&) = default;
        XenIfaceDevice &operator=(XenIfaceDevice &&) = default;

        wil::unique_hfile &GetHandle() {
            return _handle;
        }
        const std::wstring &GetPath() const {
            return _path;
        }

        _Pre_satisfies_(eventDataSize >= sizeof(CM_NOTIFY_EVENT_DATA)) static DWORD CALLBACK DeviceHandleCallback(
            _In_ HCMNOTIFICATION notifyHandle,
            _In_opt_ PVOID context,
            _In_ CM_NOTIFY_ACTION action,
            _In_reads_bytes_(eventDataSize) PCM_NOTIFY_EVENT_DATA eventData,
            _In_ DWORD eventDataSize);

    private:
        wil::unique_hcmnotification _listener;
        wil::unique_hfile _handle;
        std::wstring _path;
        XenIfaceWorker *_worker;
        ResumeNotifier _suspend;
    };

    struct XenIfaceWorkerRequest {
        std::shared_ptr<XenIfaceDevice> Target;
        CM_NOTIFY_ACTION Action;
    };

    void WorkerFunc(std::stop_token stop);
    HRESULT RefreshDevices(std::list<std::shared_ptr<XenIfaceDevice>> &tombstones);
    void
    QueueRequest(std::unique_lock<std::mutex> &&lock, std::shared_ptr<XenIfaceDevice> target, CM_NOTIFY_ACTION action);
    void OnResume(XenIfaceDevice *device);

    _Pre_satisfies_(eventDataSize >= sizeof(CM_NOTIFY_EVENT_DATA)) static DWORD CALLBACK CmListenerCallback(
        _In_ HCMNOTIFICATION notifyHandle,
        _In_opt_ PVOID context,
        _In_ CM_NOTIFY_ACTION action,
        _In_reads_bytes_(eventDataSize) PCM_NOTIFY_EVENT_DATA eventData,
        _In_ DWORD eventDataSize);

    struct {
        std::mutex _mutex;
        std::condition_variable _signal;
        _Guarded_by_(_mutex) std::list<XenIfaceWorkerRequest> _requests;
        _Guarded_by_(_mutex) std::shared_ptr<XenIfaceDevice> _active;
        _Guarded_by_(_mutex) std::vector<std::function<void()>> _callbacks;
    };
    std::jthread _worker;
};
