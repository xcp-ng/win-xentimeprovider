#include <stdexcept>
#include <system_error>

#include "XenTimeProvider.hpp"

HRESULT CALLBACK
TimeProvOpen(_In_ PWSTR wszName, _In_ TimeProvSysCallbacks *pSysCallbacks, _Out_ TimeProvHandle *phTimeProv) {
    if (wcscmp(XenTimeProviderName, wszName) == 0) {
        try {
            *phTimeProv = new (std::nothrow) XenTimeProvider(pSysCallbacks);
            RETURN_IF_NULL_ALLOC(*phTimeProv);
            return S_OK;
        } catch (...) {
            *phTimeProv = nullptr;
            return E_FAIL;
        }
    } else {
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
}

HRESULT CALLBACK TimeProvCommand(_In_ TimeProvHandle hTimeProv, _In_ TimeProvCmd eCmd, _In_ TimeProvArgs pvArgs) {
    auto provider = static_cast<XenTimeProvider *>(hTimeProv);
    FAIL_FAST_IF_NULL(provider);

    try {
        switch (eCmd) {
        case TPC_TimeJumped:
            return provider->TimeJumped(static_cast<TpcTimeJumpedArgs *>(pvArgs));
        case TPC_GetSamples:
            return provider->GetSamples(static_cast<TpcGetSamplesArgs *>(pvArgs));
        case TPC_PollIntervalChanged:
            return provider->PollIntervalChanged();
        case TPC_UpdateConfig:
            return provider->UpdateConfig();
        case TPC_Shutdown:
            return provider->Shutdown();
        default:
            return S_OK;
        }
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT CALLBACK TimeProvClose(_In_ TimeProvHandle hTimeProv) {
    try {
        delete static_cast<XenTimeProvider *>(hTimeProv);
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReasonForCall, LPVOID lpReserved) {
    UNREFERENCED_PARAMETER(hModule);
    UNREFERENCED_PARAMETER(ulReasonForCall);
    UNREFERENCED_PARAMETER(lpReserved);
    return TRUE;
}
