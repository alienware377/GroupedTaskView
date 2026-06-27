#include "pch.h"
#include "DesktopMover.h"

#include <servprov.h>
#include <objbase.h>

namespace
{
    // Stable shell CLSIDs/IIDs (unchanged across builds).
    const CLSID CLSID_ImmersiveShell =
        { 0xC2F03A33, 0x21F5, 0x47FA, { 0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39 } };
    const CLSID CLSID_VirtualDesktopManagerInternal =
        { 0xC5E0CDCA, 0x7B6E, 0x41B2, { 0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B } };
    // IApplicationViewCollection: IID and the slot of GetViewForHwnd have been
    // stable across all builds (GetViewForHwnd is the 4th method => vtable 6).
    const IID IID_IApplicationViewCollection =
        { 0x1841C6D7, 0x4F9D, 0x42C0, { 0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5 } };
    constexpr int kGetViewForHwndSlot = 6;

    // Known IVirtualDesktopManagerInternal IIDs (newest first). MoveViewToDesktop
    // has been the 2nd method (vtable slot 4) across these; FindDesktop's slot
    // shifts a little between builds, so we trial a few candidates.
    const IID kManagerIIDs[] = {
        { 0x53F5CA0B, 0x158F, 0x4124, { 0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27 } }, // Win11 22H2-24H2
        { 0xB2F925B9, 0x5A0F, 0x4D2E, { 0x9F, 0x4D, 0x2B, 0x15, 0x07, 0x59, 0x3C, 0x10 } }, // Win11 21H2
        { 0xF31574D6, 0xB682, 0x4CDC, { 0xBD, 0x56, 0x18, 0x27, 0x86, 0x0A, 0xBE, 0xC6 } }, // Win10
    };
    constexpr int kMoveViewSlot = 4;
    const int kFindSlots[] = { 14, 13, 12 };

    using FnFindDesktop = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, const GUID*, IUnknown**);
    using FnMoveView = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, IUnknown*, IUnknown*);
    using FnGetView = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, HWND, IUnknown**);

    inline void** Vtbl(IUnknown* p) { return *reinterpret_cast<void***>(p); }

    // Raw, SEH-guarded call. No C++ objects so __try is valid here.
    IUnknown* GetViewForHwndSafe(IUnknown* avc, HWND hwnd)
    {
        __try
        {
            FnGetView fn = reinterpret_cast<FnGetView>(Vtbl(avc)[kGetViewForHwndSlot]);
            IUnknown* view = nullptr;
            if (SUCCEEDED(fn(avc, hwnd, &view)))
            {
                return view;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return nullptr;
    }

    HRESULT TryMove(IUnknown* mgr, IUnknown* view, const GUID* id, int findSlot)
    {
        __try
        {
            FnFindDesktop find = reinterpret_cast<FnFindDesktop>(Vtbl(mgr)[findSlot]);
            IUnknown* desk = nullptr;
            HRESULT hr = find(mgr, id, &desk);
            if (FAILED(hr) || !desk)
            {
                return E_FAIL;
            }
            FnMoveView move = reinterpret_cast<FnMoveView>(Vtbl(mgr)[kMoveViewSlot]);
            hr = move(mgr, view, desk);
            desk->Release();
            return hr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return E_FAIL;
        }
    }

    bool MoveOne(IServiceProvider* sp, IUnknown* avc, HWND hwnd, const GUID& id)
    {
        IUnknown* view = GetViewForHwndSafe(avc, hwnd);
        if (!view)
        {
            return false;
        }
        bool ok = false;
        for (const IID& iid : kManagerIIDs)
        {
            IUnknown* mgr = nullptr;
            if (SUCCEEDED(sp->QueryService(CLSID_VirtualDesktopManagerInternal, iid, reinterpret_cast<void**>(&mgr))) && mgr)
            {
                for (int fs : kFindSlots)
                {
                    if (SUCCEEDED(TryMove(mgr, view, &id, fs)))
                    {
                        ok = true;
                        break;
                    }
                }
                mgr->Release();
            }
            if (ok)
            {
                break;
            }
        }
        view->Release();
        return ok;
    }

    struct MoveJob
    {
        const std::vector<HWND>* windows;
        GUID desktopId;
        int moved;
    };

    // The shell's immersive services expect an STA; run the work on a dedicated
    // single-threaded-apartment thread regardless of the app's main apartment.
    DWORD WINAPI MoveThread(LPVOID arg)
    {
        auto* job = static_cast<MoveJob*>(arg);
        job->moved = 0;
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        {
            return 0;
        }
        IServiceProvider* sp = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sp))) && sp)
        {
            IUnknown* avc = nullptr;
            sp->QueryService(IID_IApplicationViewCollection, IID_IApplicationViewCollection, reinterpret_cast<void**>(&avc));
            if (avc)
            {
                for (HWND h : *job->windows)
                {
                    if (h && IsWindow(h) && MoveOne(sp, avc, h, job->desktopId))
                    {
                        ++job->moved;
                    }
                }
                avc->Release();
            }
            sp->Release();
        }
        CoUninitialize();
        return 0;
    }
}

int MoveWindowsToVirtualDesktop(const std::vector<HWND>& windows, const GUID& desktopId)
{
    if (windows.empty())
    {
        return 0;
    }
    MoveJob job{ &windows, desktopId, 0 };
    HANDLE th = CreateThread(nullptr, 0, MoveThread, &job, 0, nullptr);
    if (!th)
    {
        return 0;
    }
    WaitForSingleObject(th, 6000);
    CloseHandle(th);
    return job.moved;
}
