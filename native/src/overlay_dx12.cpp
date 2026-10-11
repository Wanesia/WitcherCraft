// Minecraft HUD overlay for The Witcher 3 (DX12).
//
// Threads:
//   installer  - started from DllMain; hooks the DXGI factory at once, falls back to a dummy
//                device/swapchain after OverlayFallbackDelayMs if no swapchain creation was seen.
//   game       - our Present/ExecuteCommandLists/... hooks run on whatever thread calls them.
//   worker     - takes new frames out of the SkyCraft triple buffer and memcpy's them into
//                upload buffers, so the 8-33 MB copy never runs on the game's present thread.
//
// Safety rules: every D3D call is checked, every hook body runs under __try, and any failure
// sets g_disabled; from then on the hooks only forward to the originals. GPU objects are
// leaked (never released) on that path so nothing in flight can be freed under the GPU.
#include "overlay_dx12.hpp"

#include "camsync.hpp"
#include "config.hpp"
#include "control.hpp" // M6a: OverlayWanted() (Witcher mode is pure Witcher)
#include "log.hpp"
#include "skycraft_host.hpp"
#include "skycraft_protocol.h"

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace proto = skycraft::proto;

namespace wc::overlay
{
namespace
{
// ---- vtable slots (COM interface order, see dxgi*.h / d3d12.h) -------------------------------
constexpr int kVtFactoryCreateSwapChain = 10;        // IDXGIFactory
constexpr int kVtFactoryCreateSwapChainForHwnd = 15; // IDXGIFactory2
constexpr int kVtScPresent = 8;                      // IDXGISwapChain
constexpr int kVtScResizeBuffers = 13;               // IDXGISwapChain
constexpr int kVtScPresent1 = 22;                    // IDXGISwapChain1
constexpr int kVtScSetColorSpace1 = 38;              // IDXGISwapChain3
constexpr int kVtScResizeBuffers1 = 39;              // IDXGISwapChain3
constexpr int kVtQueueExecuteCommandLists = 10;      // ID3D12CommandQueue

using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_ResizeBuffers1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                                       IUnknown* const*);
using PFN_SetColorSpace1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
using PFN_ExecuteCommandLists = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);
using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);

PFN_Present g_oPresent = nullptr;
PFN_Present1 g_oPresent1 = nullptr;
PFN_ResizeBuffers g_oResizeBuffers = nullptr;
PFN_ResizeBuffers1 g_oResizeBuffers1 = nullptr;
PFN_SetColorSpace1 g_oSetColorSpace1 = nullptr;
PFN_ExecuteCommandLists g_oECL = nullptr;
PFN_CreateSwapChain g_oCreateSwapChain = nullptr;
PFN_CreateSwapChainForHwnd g_oCreateSwapChainForHwnd = nullptr;

void** g_factoryVtable = nullptr;
void** g_scVtable = nullptr;
void** g_queueVtable = nullptr;

std::atomic<bool> g_disabled{false};
// Occlusion: the ONE switch the depth hooks look at (occlusion on, not failed,
// overlay alive). Off = the hooks are pure pass-through.
std::atomic<bool> g_occHooks{false};
std::atomic<bool> g_factoryHooked{false};
std::atomic<bool> g_swapHooked{false};
std::atomic<bool> g_queueHooked{false};
SRWLOCK g_hookLock = SRWLOCK_INIT;

thread_local int t_presentDepth = 0;
thread_local ID3D12CommandQueue* t_lastDirect = nullptr; // last DIRECT queue this thread submitted to

struct AtomicStats
{
    std::atomic<uint64_t> presents{0}, draws{0}, uploads{0}, acquired{0}, skippedBusy{0};
    std::atomic<uint32_t> texW{0}, texH{0}, bbW{0}, bbH{0};
    std::atomic<int> colorMode{0};
    std::atomic<bool> visible{false};
} g_stats;

// ---- small helpers ---------------------------------------------------------------------------
void ModuleOf(const void* addr, char* out, size_t n)
{
    HMODULE m = nullptr;
    if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(addr), &m) &&
        m)
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        _snprintf_s(out, n, _TRUNCATE, "%ls+0x%llX", name,
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(m)));
    }
    else
    {
        _snprintf_s(out, n, _TRUNCATE, "%p(no module)", addr);
    }
}

// True when the code/vtable at addr lives in a module whose file name starts with prefix.
bool InModule(const void* addr, const wchar_t* prefix)
{
    HMODULE m = nullptr;
    if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     static_cast<LPCWSTR>(addr), &m) ||
        !m)
        return false;
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsnicmp(name, prefix, wcslen(prefix)) == 0;
}

void* VtableOf(const void* obj)
{
    return obj ? *static_cast<void* const*>(obj) : nullptr;
}

void Disable(const char* why, long hr = 0)
{
    g_occHooks = false;
    if (!g_disabled.exchange(true))
        WLOG("overlay: DISABLED for this session: %s (hr=0x%08lX). The game continues without the HUD.", why,
             static_cast<unsigned long>(hr));
}

int SehFilter(unsigned code, const char* where)
{
    g_occHooks = false;
    g_disabled = true;
    WLOG("overlay: SEH exception 0x%08X in %s -> overlay DISABLED for this session", code, where);
    return EXCEPTION_EXECUTE_HANDLER;
}

const char* FormatName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    default: return "other";
    }
}

const char* ColorSpaceName(int cs)
{
    switch (cs)
    {
    case -1: return "unknown(not set while hooked)";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return "G22_NONE_P709 (sRGB)";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return "G10_NONE_P709 (scRGB)";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "G2084_NONE_P2020 (HDR10/PQ)";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020: return "G22_NONE_P2020";
    default: return "other";
    }
}

const char* ModeName(int mode)
{
    return mode == 1 ? "scRGB" : mode == 2 ? "HDR10/PQ" : "SDR";
}

// ---- vtable entry swap ------------------------------------------------------------------------
bool SwapEntry(void** vtable, int index, void* hook, void** original, const char* name)
{
    void** slot = &vtable[index];
    void* current = *slot;
    if (current == hook)
        return true;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(slot, &mbi, sizeof(mbi)))
    {
        WLOG("overlay: VirtualQuery(%s slot) failed (%lu)", name, GetLastError());
        return false;
    }
    const bool exec = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old))
    {
        WLOG("overlay: VirtualProtect(%s slot) failed (%lu)", name, GetLastError());
        return false;
    }
    *original = current; // publish the original before the hook can run
    InterlockedExchangePointer(slot, hook);
    VirtualProtect(slot, sizeof(void*), old, &old);
    char mod[160];
    ModuleOf(current, mod, sizeof(mod));
    WLOG("overlay: hooked %-28s vtable %p[%2d], original %s", name, static_cast<void*>(vtable), index, mod);
    return true;
}

// ---- known command queues (filled by the ExecuteCommandLists hook) ---------------------------
struct QueueRec
{
    std::atomic<ID3D12CommandQueue*> q{nullptr};
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12Device* device = nullptr;
    std::atomic<uint64_t> ecl{0};
};
constexpr int kMaxQueues = 64;
QueueRec g_queues[kMaxQueues];
std::atomic<int> g_queueCount{0};
SRWLOCK g_queueLock = SRWLOCK_INIT;

QueueRec* FindQueue(const void* q)
{
    const int n = g_queueCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_queues[i].q.load(std::memory_order_relaxed) == q)
            return &g_queues[i];
    return nullptr;
}

const char* QueueTypeName(D3D12_COMMAND_LIST_TYPE t)
{
    switch (t)
    {
    case D3D12_COMMAND_LIST_TYPE_DIRECT: return "DIRECT";
    case D3D12_COMMAND_LIST_TYPE_COMPUTE: return "COMPUTE";
    case D3D12_COMMAND_LIST_TYPE_COPY: return "COPY";
    default: return "other";
    }
}

void NoteQueue(ID3D12CommandQueue* q, void* ret)
{
    QueueRec* r = FindQueue(q);
    if (!r)
    {
        // Never block a game thread here: if someone else is registering, try again next call.
        if (!TryAcquireSRWLockExclusive(&g_queueLock))
            return;
        r = FindQueue(q);
        const int n = g_queueCount.load(std::memory_order_relaxed);
        if (!r && n < kMaxQueues)
        {
            const D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&dev))) && dev)
                dev->Release();
            r = &g_queues[n];
            r->type = d.Type;
            r->device = dev;
            r->q.store(q, std::memory_order_relaxed);
            g_queueCount.store(n + 1, std::memory_order_release);
            char caller[160];
            ModuleOf(ret, caller, sizeof(caller));
            WLOG("overlay: queue #%d %p %s prio %d device %p - first ExecuteCommandLists from %s (thread %lu)", n,
                 static_cast<void*>(q), QueueTypeName(d.Type), d.Priority, static_cast<void*>(dev), caller,
                 GetCurrentThreadId());
        }
        ReleaseSRWLockExclusive(&g_queueLock);
    }
    if (r)
    {
        r->ecl.fetch_add(1, std::memory_order_relaxed);
        if (r->type == D3D12_COMMAND_LIST_TYPE_DIRECT)
            t_lastDirect = q;
    }
}

// ---- known swapchains -------------------------------------------------------------------------
struct ScRec
{
    IDXGISwapChain* sc = nullptr; // identity only; never dereferenced from here
    ID3D12CommandQueue* queue = nullptr;
    char queueSource[48]{};
    int colorSpace = -1;
};
constexpr int kMaxSc = 16;
ScRec g_scs[kMaxSc];
int g_scNext = 0;
SRWLOCK g_scLock = SRWLOCK_INIT;

ScRec* ScFindLocked(const void* sc)
{
    for (auto& r : g_scs)
        if (r.sc == sc)
            return &r;
    return nullptr;
}

ScRec* ScFindOrAddLocked(IDXGISwapChain* sc)
{
    if (ScRec* r = ScFindLocked(sc))
        return r;
    ScRec& r = g_scs[g_scNext++ % kMaxSc];
    r = ScRec{};
    r.sc = sc;
    return &r;
}

void ScSetQueue(IDXGISwapChain* sc, ID3D12CommandQueue* q, const char* source)
{
    AcquireSRWLockExclusive(&g_scLock);
    ScRec* r = ScFindOrAddLocked(sc);
    r->queue = q;
    strncpy_s(r->queueSource, source, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_scLock);
}

void ScSetColorSpace(IDXGISwapChain* sc, int cs)
{
    AcquireSRWLockExclusive(&g_scLock);
    ScFindOrAddLocked(sc)->colorSpace = cs;
    ReleaseSRWLockExclusive(&g_scLock);
}

void ScForget(const void* sc)
{
    AcquireSRWLockExclusive(&g_scLock);
    if (ScRec* r = ScFindLocked(sc))
        *r = ScRec{};
    ReleaseSRWLockExclusive(&g_scLock);
}

ScRec ScGet(IDXGISwapChain* sc)
{
    AcquireSRWLockShared(&g_scLock);
    const ScRec* r = ScFindLocked(sc);
    ScRec copy = r ? *r : ScRec{};
    ReleaseSRWLockShared(&g_scLock);
    copy.sc = sc;
    return copy;
}

// ---- hook installation from live objects ------------------------------------------------------
HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags);
HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params);
HRESULT STDMETHODCALLTYPE HkResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags);
HRESULT STDMETHODCALLTYPE HkResizeBuffers1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags,
                                           const UINT* nodeMasks, IUnknown* const* queues);
HRESULT STDMETHODCALLTYPE HkSetColorSpace1(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE cs);
void STDMETHODCALLTYPE HkExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists);
HRESULT STDMETHODCALLTYPE HkCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out);
HRESULT STDMETHODCALLTYPE HkCreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out,
                                                   IDXGISwapChain1** sc);

bool HookSwapChainVtable(IDXGISwapChain* sc, const char* how)
{
    void** vt = static_cast<void**>(VtableOf(sc));
    AcquireSRWLockExclusive(&g_hookLock);
    bool ok = true;
    if (!g_swapHooked)
    {
        char mod[160];
        ModuleOf(vt, mod, sizeof(mod));
        WLOG("overlay: swapchain %p vtable %p (%s) via %s", static_cast<void*>(sc), static_cast<void*>(vt), mod, how);
        if (!InModule(vt, L"dxgi"))
            WLOG("overlay: NOTE the swapchain vtable is not in dxgi.dll: this is a wrapper (Streamline/FG/another overlay)");
        ok = SwapEntry(vt, kVtScPresent, reinterpret_cast<void*>(&HkPresent), reinterpret_cast<void**>(&g_oPresent),
                       "IDXGISwapChain::Present") &&
             SwapEntry(vt, kVtScPresent1, reinterpret_cast<void*>(&HkPresent1), reinterpret_cast<void**>(&g_oPresent1),
                       "IDXGISwapChain1::Present1") &&
             SwapEntry(vt, kVtScResizeBuffers, reinterpret_cast<void*>(&HkResizeBuffers),
                       reinterpret_cast<void**>(&g_oResizeBuffers), "IDXGISwapChain::ResizeBuffers");
        // IDXGISwapChain3 slots only exist if the object implements it.
        IDXGISwapChain3* sc3 = nullptr;
        if (ok && SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3)
        {
            if (VtableOf(sc3) == vt)
            {
                ok = SwapEntry(vt, kVtScResizeBuffers1, reinterpret_cast<void*>(&HkResizeBuffers1),
                               reinterpret_cast<void**>(&g_oResizeBuffers1), "IDXGISwapChain3::ResizeBuffers1") &&
                     SwapEntry(vt, kVtScSetColorSpace1, reinterpret_cast<void*>(&HkSetColorSpace1),
                               reinterpret_cast<void**>(&g_oSetColorSpace1), "IDXGISwapChain3::SetColorSpace1");
            }
            else
            {
                WLOG("overlay: IDXGISwapChain3 has its own vtable %p; ResizeBuffers1/SetColorSpace1 not hooked", VtableOf(sc3));
            }
            sc3->Release();
        }
        if (ok)
        {
            g_scVtable = vt;
            g_swapHooked = true;
        }
    }
    else if (vt != g_scVtable)
    {
        char mod[160];
        ModuleOf(vt, mod, sizeof(mod));
        WLOG("overlay: WARNING swapchain %p uses a different vtable %p (%s) than the hooked one %p; not hooked",
             static_cast<void*>(sc), static_cast<void*>(vt), mod, static_cast<void*>(g_scVtable));
    }
    ReleaseSRWLockExclusive(&g_hookLock);
    return ok;
}

bool HookQueueVtable(ID3D12CommandQueue* q, const char* how)
{
    void** vt = static_cast<void**>(VtableOf(q));
    AcquireSRWLockExclusive(&g_hookLock);
    bool ok = true;
    if (!g_queueHooked)
    {
        char mod[160];
        ModuleOf(vt, mod, sizeof(mod));
        WLOG("overlay: command queue %p vtable %p (%s) via %s", static_cast<void*>(q), static_cast<void*>(vt), mod, how);
        ok = SwapEntry(vt, kVtQueueExecuteCommandLists, reinterpret_cast<void*>(&HkExecuteCommandLists),
                       reinterpret_cast<void**>(&g_oECL), "ID3D12CommandQueue::ExecuteCommandLists");
        if (ok)
        {
            g_queueVtable = vt;
            g_queueHooked = true;
        }
    }
    else if (vt != g_queueVtable)
    {
        char mod[160];
        ModuleOf(vt, mod, sizeof(mod));
        WLOG("overlay: NOTE queue %p has another vtable %p (%s); its submissions are not observed", static_cast<void*>(q),
             static_cast<void*>(vt), mod);
    }
    ReleaseSRWLockExclusive(&g_hookLock);
    return ok;
}

// ---- W3 depth discovery ---------------------------------------------------
// The scene depth is found by watching the game's own D3D12 calls (vtable hooks on the shared
// native command-list and device vtables). W3 5.0 uses DYNAMIC RESOLUTION: a pool of D32S8 depth
// buffers, one per resolution step, and a different one may be used every frame. So the depth used
// is picked anew EVERY Present: the candidate the game used most since the previous Present
// (binds as DSV + clears + depth barriers), among those with the back buffer's aspect ratio.
//
// Hook rules (the game's render threads run these):
//   - pure pass-through unless occlusion is active (g_occHooks), and never for our own lists;
//   - NO locks, NO logging, NO allocation: lock-free tables with atomics only (new candidates are
//     logged later by the present thread);
//   - GetDesc only on resources the game passes in a barrier (alive by definition), under SEH.
constexpr int kVtListRSSetViewports = 21;        // ID3D12GraphicsCommandList
constexpr int kVtListResourceBarrier = 26;
constexpr int kVtListOMSetRenderTargets = 46;
constexpr int kVtListClearDepthStencilView = 47;
constexpr int kVtDeviceCreateDepthStencilView = 21; // ID3D12Device

using PFN_ResourceBarrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using PFN_OMSetRenderTargets = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                                        const D3D12_CPU_DESCRIPTOR_HANDLE*);
using PFN_ClearDepthStencilView = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS,
                                                           FLOAT, UINT8, UINT, const D3D12_RECT*);
using PFN_RSSetViewports = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using PFN_CreateDepthStencilView = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE);
PFN_ResourceBarrier g_oResourceBarrier = nullptr;
PFN_OMSetRenderTargets g_oOMSetRenderTargets = nullptr;
PFN_ClearDepthStencilView g_oClearDsv = nullptr;
PFN_RSSetViewports g_oRSSetViewports = nullptr;
PFN_CreateDepthStencilView g_oCreateDsv = nullptr;
std::atomic<bool> g_listHooked{false}, g_deviceHooked{false};

thread_local bool t_ours = false; // recording our own overlay lists

struct DepthCand
{
    std::atomic<ID3D12Resource*> res{nullptr};
    std::atomic<uint32_t> w{0}, h{0}, fmt{0}; // desc when first seen (re-read with GetDesc before every use)
    std::atomic<uint64_t> uses{0};            // lifetime: barriers + binds + clears
    std::atomic<uint64_t> frameStamp{0};      // present number frameUses belongs to
    std::atomic<uint32_t> frameUses{0};       // uses since that Present
    std::atomic<uint64_t> lastPresent{0};
    std::atomic<uint32_t> lastState{0};       // subresource 0 (the depth plane), from the barriers
    std::atomic<bool> stateKnown{false};
    std::atomic<uint32_t> clearBits{0x7FC00000u}; // last full ClearDepthStencilView depth (NaN = never)
    std::atomic<uint32_t> vpW{0}, vpH{0};        // largest viewport set while it was the bound DSV
    std::atomic<int> logged{0};
};
constexpr int kMaxCand = 64;
DepthCand g_cand[kMaxCand];
std::atomic<int> g_candCount{0};
std::atomic<uint64_t> g_presentNo{0};
std::atomic<uint32_t> g_bbW{0}, g_bbH{0}; // for the candidate filter

// Resources seen in depth barriers that are NOT candidates (shadow maps, arrays...): skip GetDesc.
constexpr int kRejectSlots = 128;
std::atomic<ID3D12Resource*> g_reject[kRejectSlots];
std::atomic<uint64_t> g_rejectStamp[kRejectSlots];

// DSV descriptor -> resource (CreateDepthStencilView hook), lock-free open addressing.
constexpr int kDsvSlots = 2048;
std::atomic<SIZE_T> g_dsvHandle[kDsvSlots];
std::atomic<ID3D12Resource*> g_dsvRes[kDsvSlots];

ID3D12Resource* DsvLookup(SIZE_T h)
{
    if (!h)
        return nullptr;
    const int s = static_cast<int>((h >> 3) % kDsvSlots);
    for (int i = 0; i < 16; ++i)
    {
        const int k = (s + i) % kDsvSlots;
        const SIZE_T v = g_dsvHandle[k].load(std::memory_order_acquire);
        if (v == h)
            return g_dsvRes[k].load(std::memory_order_acquire);
        if (v == 0)
            return nullptr;
    }
    return nullptr;
}

void DsvInsert(SIZE_T h, ID3D12Resource* res)
{
    if (!h)
        return;
    const int s = static_cast<int>((h >> 3) % kDsvSlots);
    for (int i = 0; i < 16; ++i)
    {
        const int k = (s + i) % kDsvSlots;
        SIZE_T v = g_dsvHandle[k].load(std::memory_order_acquire);
        if (v == 0)
        {
            g_dsvRes[k].store(res, std::memory_order_release); // claim below; a racing reader sees 0 = miss
            if (g_dsvHandle[k].compare_exchange_strong(v, h, std::memory_order_acq_rel))
                return;
        }
        if (v == h)
        {
            g_dsvRes[k].store(res, std::memory_order_release);
            return;
        }
    }
    // Probe window full: leave it (that DSV's binds are simply not counted).
}

const char* DepthFormatName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
    case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
    default: return "other";
    }
}

// Typeless family (for our copy) and the SRV format that reads the depth PLANE (plane 0) as a float.
bool DepthFormats(DXGI_FORMAT f, DXGI_FORMAT& typeless, DXGI_FORMAT& srv)
{
    switch (f)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT: typeless = DXGI_FORMAT_R32_TYPELESS, srv = DXGI_FORMAT_R32_FLOAT; return true;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS: typeless = DXGI_FORMAT_R24G8_TYPELESS, srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; return true;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS: typeless = DXGI_FORMAT_R32G8X24_TYPELESS, srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; return true;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS: typeless = DXGI_FORMAT_R16_TYPELESS, srv = DXGI_FORMAT_R16_UNORM; return true;
    default: return false;
    }
}

DepthCand* FindCand(const ID3D12Resource* r)
{
    if (!r)
        return nullptr;
    const int n = std::min(g_candCount.load(std::memory_order_acquire), kMaxCand);
    for (int i = 0; i < n; ++i)
        if (g_cand[i].res.load(std::memory_order_relaxed) == r)
            return &g_cand[i];
    return nullptr;
}

bool SafeGetDesc(ID3D12Resource* r, D3D12_RESOURCE_DESC& d)
{
    __try
    {
        d = r->GetDesc();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Scene-depth shape: 2D, ALLOW_DEPTH_STENCIL, 1 slice, no MSAA, a known depth format, the back
// buffer's aspect ratio (3%) and at least a quarter of its width (dynamic resolution goes down to 50%).
bool CandShape(const D3D12_RESOURCE_DESC& d)
{
    DXGI_FORMAT tl, sv;
    const uint32_t bw = g_bbW.load(std::memory_order_relaxed), bh = g_bbH.load(std::memory_order_relaxed);
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ||
        d.DepthOrArraySize != 1 || d.SampleDesc.Count != 1 || !DepthFormats(d.Format, tl, sv) || d.Height == 0)
        return false;
    if (!bw || !bh)
        return true; // back buffer not known yet: decide later
    const double a = double(d.Width) / d.Height, ab = double(bw) / bh;
    return std::fabs(a / ab - 1.0) <= 0.03 && d.Width * 4 >= bw;
}

bool IsRejected(ID3D12Resource* r, uint64_t P)
{
    const int s = static_cast<int>((reinterpret_cast<uintptr_t>(r) >> 4) % kRejectSlots);
    return g_reject[s].load(std::memory_order_relaxed) == r && g_rejectStamp[s].load(std::memory_order_relaxed) + 600 > P;
}

void Reject(ID3D12Resource* r, uint64_t P)
{
    const int s = static_cast<int>((reinterpret_cast<uintptr_t>(r) >> 4) % kRejectSlots);
    g_reject[s].store(r, std::memory_order_relaxed);
    g_rejectStamp[s].store(P, std::memory_order_relaxed);
}

void CountUse(DepthCand* c, uint64_t P)
{
    if (c->frameStamp.load(std::memory_order_relaxed) != P)
    {
        c->frameStamp.store(P, std::memory_order_relaxed);
        c->frameUses.store(0, std::memory_order_relaxed);
    }
    c->frameUses.fetch_add(1, std::memory_order_relaxed);
    c->uses.fetch_add(1, std::memory_order_relaxed);
    c->lastPresent.store(P, std::memory_order_relaxed);
}

// A resource the game put through a depth barrier (so it is alive): find or register it.
DepthCand* NoteBarrierResource(ID3D12Resource* r)
{
    const uint64_t P = g_presentNo.load(std::memory_order_relaxed);
    if (DepthCand* c = FindCand(r))
        return c;
    if (IsRejected(r, P))
        return nullptr;
    D3D12_RESOURCE_DESC d{};
    if (!SafeGetDesc(r, d) || !CandShape(d))
    {
        Reject(r, P);
        return nullptr;
    }
    // A slot: a new one, else one unused for 600 presents (resolution changes recreate the pool).
    int slot = -1;
    const int n = g_candCount.load(std::memory_order_acquire);
    if (n < kMaxCand)
    {
        const int k = g_candCount.fetch_add(1, std::memory_order_acq_rel);
        if (k < kMaxCand)
            slot = k;
    }
    for (int i = 0; slot < 0 && i < kMaxCand; ++i)
    {
        ID3D12Resource* old = g_cand[i].res.load(std::memory_order_relaxed);
        if (old && g_cand[i].lastPresent.load(std::memory_order_relaxed) + 600 < P &&
            g_cand[i].res.compare_exchange_strong(old, nullptr, std::memory_order_acq_rel))
            slot = i;
    }
    if (slot < 0)
        return nullptr;
    DepthCand& c = g_cand[slot];
    c.w = static_cast<uint32_t>(d.Width);
    c.h = d.Height;
    c.fmt = static_cast<uint32_t>(d.Format);
    c.uses = 0;
    c.frameStamp = 0;
    c.frameUses = 0;
    c.stateKnown = false;
    c.clearBits = 0x7FC00000u;
    c.vpW = 0;
    c.vpH = 0;
    c.lastPresent = P;
    c.logged = 0;
    c.res.store(r, std::memory_order_release);
    return &c;
}

void NoteBarriers(UINT n, const D3D12_RESOURCE_BARRIER* b)
{
    constexpr uint32_t kDepthStates = D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ;
    const uint64_t P = g_presentNo.load(std::memory_order_relaxed);
    for (UINT i = 0; i < n; ++i)
    {
        const D3D12_RESOURCE_BARRIER& x = b[i];
        if (x.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !x.Transition.pResource ||
            !((x.Transition.StateBefore | x.Transition.StateAfter) & kDepthStates))
            continue;
        if (DepthCand* c = NoteBarrierResource(x.Transition.pResource))
        {
            CountUse(c, P);
            // The depth plane's state: whole-resource barriers and subresource 0.
            if (x.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || x.Transition.Subresource == 0)
            {
                c->lastState.store(x.Transition.StateAfter, std::memory_order_relaxed);
                c->stateKnown.store(true, std::memory_order_relaxed);
            }
        }
    }
}

void SafeNoteBarriers(UINT n, const D3D12_RESOURCE_BARRIER* b)
{
    __try
    {
        NoteBarriers(n, b);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

thread_local ID3D12GraphicsCommandList* t_boundList = nullptr;
thread_local DepthCand* t_boundCand = nullptr;

void STDMETHODCALLTYPE HkResourceBarrier(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RESOURCE_BARRIER* b)
{
    if (g_occHooks.load(std::memory_order_relaxed) && !t_ours && b && n)
        SafeNoteBarriers(n, b);
    g_oResourceBarrier(l, n, b);
}

void STDMETHODCALLTYPE HkOMSetRenderTargets(ID3D12GraphicsCommandList* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{
    if (g_occHooks.load(std::memory_order_relaxed) && !t_ours)
    {
        // Only resources already known from a barrier are counted (no GetDesc on a stale pointer).
        DepthCand* c = dsv ? FindCand(DsvLookup(dsv->ptr)) : nullptr;
        if (c)
            CountUse(c, g_presentNo.load(std::memory_order_relaxed));
        t_boundList = c ? l : nullptr;
        t_boundCand = c;
    }
    g_oOMSetRenderTargets(l, n, rtvs, single, dsv);
}

void STDMETHODCALLTYPE HkClearDsv(ID3D12GraphicsCommandList* l, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth,
                                  UINT8 stencil, UINT nr, const D3D12_RECT* rects)
{
    if (g_occHooks.load(std::memory_order_relaxed) && !t_ours && (flags & D3D12_CLEAR_FLAG_DEPTH))
        if (DepthCand* c = FindCand(DsvLookup(dsv.ptr)))
        {
            CountUse(c, g_presentNo.load(std::memory_order_relaxed));
            if (nr == 0)
            {
                uint32_t bits;
                std::memcpy(&bits, &depth, 4);
                c->clearBits.store(bits, std::memory_order_relaxed);
            }
        }
    g_oClearDsv(l, dsv, flags, depth, stencil, nr, rects);
}

void STDMETHODCALLTYPE HkRSSetViewports(ID3D12GraphicsCommandList* l, UINT n, const D3D12_VIEWPORT* vp)
{
    if (n && vp && l == t_boundList && t_boundCand && g_occHooks.load(std::memory_order_relaxed) && !t_ours)
    {
        DepthCand* c = t_boundCand;
        const uint32_t w = static_cast<uint32_t>(vp[0].Width + 0.5f), h = static_cast<uint32_t>(vp[0].Height + 0.5f);
        if (w > c->vpW.load(std::memory_order_relaxed))
            c->vpW.store(w, std::memory_order_relaxed);
        if (h > c->vpH.load(std::memory_order_relaxed))
            c->vpH.store(h, std::memory_order_relaxed);
    }
    g_oRSSetViewports(l, n, vp);
}

void STDMETHODCALLTYPE HkCreateDsv(ID3D12Device* d, ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                   D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    g_oCreateDsv(d, r, desc, h);
    if (r && g_occHooks.load(std::memory_order_relaxed))
        DsvInsert(h.ptr, r); // always the newest resource for that descriptor
}

// Hooks the (shared, native) command-list and device vtables. Called with the game's native
// device as soon as we know it (swapchain creation, or the renderer's first frame).
void InstallDepthHooks(ID3D12Device* dev, const char* how)
{
    if (!dev || (g_listHooked && g_deviceHooked))
        return;
    AcquireSRWLockExclusive(&g_hookLock); // install time only (never taken by the hooks themselves)
    if (!g_deviceHooked)
    {
        void** vt = static_cast<void**>(VtableOf(dev));
        if (SwapEntry(vt, kVtDeviceCreateDepthStencilView, reinterpret_cast<void*>(&HkCreateDsv), reinterpret_cast<void**>(&g_oCreateDsv),
                      "ID3D12Device::CreateDepthStencilView"))
            g_deviceHooked = true;
    }
    if (!g_listHooked)
    {
        ID3D12CommandAllocator* a = nullptr;
        ID3D12GraphicsCommandList* l = nullptr;
        if (SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) &&
            SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l))))
        {
            l->Close();
            void** vt = static_cast<void**>(VtableOf(l));
            char mod[160];
            ModuleOf(vt, mod, sizeof(mod));
            WLOG("occlusion: command list vtable %p (%s) via %s", static_cast<void*>(vt), mod, how);
            g_listHooked =
                SwapEntry(vt, kVtListResourceBarrier, reinterpret_cast<void*>(&HkResourceBarrier), reinterpret_cast<void**>(&g_oResourceBarrier),
                          "ID3D12GraphicsCommandList::ResourceBarrier") &&
                SwapEntry(vt, kVtListOMSetRenderTargets, reinterpret_cast<void*>(&HkOMSetRenderTargets),
                          reinterpret_cast<void**>(&g_oOMSetRenderTargets), "ID3D12GraphicsCommandList::OMSetRenderTargets") &&
                SwapEntry(vt, kVtListClearDepthStencilView, reinterpret_cast<void*>(&HkClearDsv), reinterpret_cast<void**>(&g_oClearDsv),
                          "ID3D12GraphicsCommandList::ClearDepthStencilView") &&
                SwapEntry(vt, kVtListRSSetViewports, reinterpret_cast<void*>(&HkRSSetViewports), reinterpret_cast<void**>(&g_oRSSetViewports),
                          "ID3D12GraphicsCommandList::RSSetViewports");
        }
        if (l)
            l->Release();
        if (a)
            a->Release();
    }
    ReleaseSRWLockExclusive(&g_hookLock);
    g_occHooks = g_listHooked && g_config.overlayOcclusion;
}

void SafeInstallDepthHooks(ID3D12Device* dev, const char* how)
{
    if (!g_config.overlayOcclusion)
    {
        static bool s_logged = false;
        if (!s_logged)
            WLOG("occlusion: OverlayOcclusion=0 at startup: no depth hooks (Minecraft always drawn on top)");
        s_logged = true;
        return;
    }
    __try
    {
        InstallDepthHooks(dev, how);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        WLOG("occlusion: SEH exception while hooking the depth discovery; occlusion unavailable");
    }
}

void OnSwapChainCreated(IDXGISwapChain* sc, IUnknown* dev, const char* via, void* ret)
{
    char caller[160];
    ModuleOf(ret, caller, sizeof(caller));
    ID3D12CommandQueue* q = nullptr;
    if (!dev || FAILED(dev->QueryInterface(IID_PPV_ARGS(&q))) || !q)
    {
        WLOG("overlay: %s -> swapchain %p is not on a D3D12 queue (device %p, caller %s); ignored", via,
             static_cast<void*>(sc), static_cast<void*>(dev), caller);
        return;
    }
    const D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
    q->Release(); // the swapchain holds its own reference
    char qmod[160];
    ModuleOf(VtableOf(q), qmod, sizeof(qmod));
    WLOG("overlay: %s -> swapchain %p presents on queue %p (%s, vtable in %s), called from %s", via, static_cast<void*>(sc),
         static_cast<void*>(q), QueueTypeName(d.Type), qmod, caller);
    ScSetQueue(sc, q, via);
    HookQueueVtable(q, via);
    HookSwapChainVtable(sc, via);
    ID3D12Device* d12 = nullptr;
    if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&d12))) && d12)
    {
        SafeInstallDepthHooks(d12, via); // early: the game creates its depth buffers/DSVs after the swapchain
        d12->Release();
    }
}

void SafeOnSwapChainCreated(IDXGISwapChain* sc, IUnknown* dev, const char* via, void* ret)
{
    __try
    {
        OnSwapChainCreated(sc, dev, via, ret);
    }
    __except (SehFilter(GetExceptionCode(), "CreateSwapChain hook"))
    {
    }
}

HRESULT STDMETHODCALLTYPE HkCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
{
    const HRESULT hr = g_oCreateSwapChain(f, dev, desc, out);
    if (SUCCEEDED(hr) && out && *out && !g_disabled.load(std::memory_order_relaxed))
        SafeOnSwapChainCreated(*out, dev, "IDXGIFactory::CreateSwapChain", _ReturnAddress());
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out,
                                                   IDXGISwapChain1** sc)
{
    const HRESULT hr = g_oCreateSwapChainForHwnd(f, dev, hwnd, desc, fs, out, sc);
    if (SUCCEEDED(hr) && sc && *sc && !g_disabled.load(std::memory_order_relaxed))
        SafeOnSwapChainCreated(*sc, dev, "IDXGIFactory2::CreateSwapChainForHwnd", _ReturnAddress());
    return hr;
}

// ---- dynamic system DLL entry points (keeps dinput8.dll's import table unchanged) ------------
FARPROC SysProc(const wchar_t* dll, const char* name)
{
    HMODULE m = GetModuleHandleW(dll);
    if (!m)
        m = LoadLibraryW(dll);
    return m ? GetProcAddress(m, name) : nullptr;
}

bool InstallFactoryHooks()
{
    if (g_factoryHooked)
        return true;
    auto create = reinterpret_cast<PFN_CreateDXGIFactory2>(SysProc(L"dxgi.dll", "CreateDXGIFactory2"));
    if (!create)
    {
        WLOG("overlay: dxgi.dll CreateDXGIFactory2 not found");
        return false;
    }
    IDXGIFactory2* factory = nullptr;
    HRESULT hr = create(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory)
    {
        WLOG("overlay: CreateDXGIFactory2 failed 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    void** vt = static_cast<void**>(VtableOf(factory));
    AcquireSRWLockExclusive(&g_hookLock);
    const bool ok = SwapEntry(vt, kVtFactoryCreateSwapChain, reinterpret_cast<void*>(&HkCreateSwapChain),
                              reinterpret_cast<void**>(&g_oCreateSwapChain), "IDXGIFactory::CreateSwapChain") &&
                    SwapEntry(vt, kVtFactoryCreateSwapChainForHwnd, reinterpret_cast<void*>(&HkCreateSwapChainForHwnd),
                              reinterpret_cast<void**>(&g_oCreateSwapChainForHwnd), "IDXGIFactory2::CreateSwapChainForHwnd");
    if (ok)
    {
        g_factoryVtable = vt;
        g_factoryHooked = true;
    }
    ReleaseSRWLockExclusive(&g_hookLock);
    factory->Release();
    return ok;
}

LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, m, w, l);
}

// Fallback: a throwaway device, queue and swapchain on a hidden window, just to reach the
// (shared, per-class) vtables. Only used when the game's swapchain creation wasn't observed.
bool InstallViaDummyDevice()
{
    auto createDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(SysProc(L"d3d12.dll", "D3D12CreateDevice"));
    auto createFactory = reinterpret_cast<PFN_CreateDXGIFactory2>(SysProc(L"dxgi.dll", "CreateDXGIFactory2"));
    if (!createDevice || !createFactory)
    {
        WLOG("overlay: dummy path: d3d12/dxgi entry points missing");
        return false;
    }
    bool ok = false;
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain1* sc = nullptr;
    HWND hwnd = nullptr;
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"WitcherCraftOverlayDummy";
    RegisterClassExW(&wc);
    do
    {
        HRESULT hr = createFactory(0, IID_PPV_ARGS(&factory));
        if (FAILED(hr))
        {
            WLOG("overlay: dummy factory failed 0x%08lX", static_cast<unsigned long>(hr));
            break;
        }
        IDXGIFactory6* f6 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f6))) && f6)
        {
            f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
            f6->Release();
        }
        hr = createDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(hr))
        {
            WLOG("overlay: dummy D3D12CreateDevice failed 0x%08lX", static_cast<unsigned long>(hr));
            break;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        if (FAILED(hr))
        {
            WLOG("overlay: dummy queue failed 0x%08lX", static_cast<unsigned long>(hr));
            break;
        }
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, inst, nullptr);
        if (!hwnd)
        {
            WLOG("overlay: dummy window failed (%lu)", GetLastError());
            break;
        }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 64;
        sd.Height = 64;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        // Goes through our CreateSwapChainForHwnd hook if the factory vtable is the hooked one.
        hr = factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc);
        if (FAILED(hr) || !sc)
        {
            WLOG("overlay: dummy swapchain failed 0x%08lX", static_cast<unsigned long>(hr));
            break;
        }
        ok = HookQueueVtable(queue, "dummy device") && HookSwapChainVtable(sc, "dummy device");
        ScForget(sc);
    } while (false);
    if (sc)
        sc->Release();
    if (queue)
        queue->Release();
    if (device)
        device->Release();
    if (adapter)
        adapter->Release();
    if (factory)
        factory->Release();
    if (hwnd)
        DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    WLOG("overlay: dummy-device hook path %s", ok ? "OK" : "FAILED");
    return ok;
}

// ---- link state (read straight from the mapping; host-owned code untouched) -------------------
struct LinkView
{
    bool mapped = false, mcAlive = false, inWorld = false, screenOpen = false;
    bool inGame = false, menu = false, loading = false;
    uint32_t guiScale = 0, cameraMode = 0;
};

constexpr uint64_t kMcTimeoutMs = 3000; // as SkyCraft's Link::McAlive

template <class T> T AtomicLoad(T& v)
{
    return std::atomic_ref<T>(v).load(std::memory_order_acquire);
}

bool ReadMcState(uint8_t* base, proto::McState& out)
{
    auto* src = reinterpret_cast<proto::McState*>(base + proto::kOffMcState);
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const uint32_t s1 = AtomicLoad(src->seq);
        if (s1 & 1)
        {
            _mm_pause();
            continue;
        }
        std::memcpy(&out, src, sizeof(out));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (AtomicLoad(src->seq) == s1)
            return true;
    }
    return false;
}

LinkView ReadLink()
{
    LinkView v;
    uint8_t* base = wc::host::MappingBase();
    if (!base)
        return v;
    v.mapped = true;
    auto* hdr = reinterpret_cast<proto::Header*>(base + proto::kOffHeader);
    const uint64_t beat = AtomicLoad(hdr->mcHeartbeatMs);
    v.mcAlive = beat != 0 && GetTickCount64() - beat < kMcTimeoutMs;
    auto* sky = reinterpret_cast<proto::SkyState*>(base + proto::kOffSkyState);
    const uint32_t sf = AtomicLoad(sky->flags);
    v.inGame = (sf & proto::kSkyInGame) != 0;
    v.menu = (sf & proto::kSkyMenuOpen) != 0;
    v.loading = (sf & proto::kSkyLoading) != 0;
    static proto::McState last{};
    proto::McState mc{};
    if (v.mcAlive && ReadMcState(base, mc))
        last = mc; // on a torn read keep the previous snapshot (render thread only)
    if (v.mcAlive)
    {
        v.inWorld = (last.flags & proto::kMcInWorld) != 0;
        v.screenOpen = (last.flags & proto::kMcScreenOpen) != 0;
        v.guiScale = last.guiScale;
        v.cameraMode = last.cameraMode;
    }
    return v;
}

// ---- upload buffers (worker fills, render thread consumes) ------------------------------------
enum : int
{
    kUpFree = 0,
    kUpFilling,
    kUpReady,
    kUpInFlight
};
struct UploadBuf
{
    ID3D12Resource* res = nullptr;
    uint8_t* ptr = nullptr;
    uint64_t capacity = 0;
    uint32_t w = 0, h = 0, pitch = 0, flags = 0;
    uint64_t frameId = 0, seq = 0, fence = 0;
    int state = kUpFree;
    camsync::CamPose pose; // kOverlayHasPose frames: the camera MC rendered it with
    // kOverlayHasDepth: Minecraft's depth plane after the colour in this buffer.
    bool hasDepth = false;
    uint64_t depthOffset = 0;
    uint32_t depthPitch = 0;
    float depthA = 0, depthB = 0, depthClear = 0;
};
constexpr int kUploads = 4;
std::atomic<uint64_t> g_depthFrames{0}, g_depthBad{0};
UploadBuf g_up[kUploads];
SRWLOCK g_upLock = SRWLOCK_INIT;
std::atomic<ID3D12Device*> g_workerDevice{nullptr};
std::atomic<uint64_t> g_wantFramesTick{0};

constexpr uint32_t Align(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE t)
{
    D3D12_HEAP_PROPERTIES p{};
    p.Type = t;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

// M3a pose getter (LastDrawnPose): the camera of the newest frame taken for drawing.
SRWLOCK g_poseLock = SRWLOCK_INIT;
wc::overlay::Pose g_pose;
bool g_poseValid = false;
void StorePose(const proto::OverlaySlotHdr& sh)
{
    AcquireSRWLockExclusive(&g_poseLock);
    g_pose.frameId = sh.frameId;
    g_pose.takenMs = GetTickCount64();
    g_pose.eye[0] = sh.camEyeX, g_pose.eye[1] = sh.camEyeY, g_pose.eye[2] = sh.camEyeZ;
    g_pose.yaw = sh.camYaw, g_pose.pitch = sh.camPitch, g_pose.fovDeg = sh.camFovDeg, g_pose.rollDeg = sh.camRollDeg;
    g_poseValid = true;
    ReleaseSRWLockExclusive(&g_poseLock);
}

// One step of the worker. Returns true if it did work (poll again at once).
bool WorkerStep(uint32_t& front, uint32_t& lastPid, uint64_t& seq)
{
    uint8_t* base = wc::host::MappingBase();
    ID3D12Device* device = g_workerDevice.load();
    if (!base || !device)
        return false;
    auto* hdr = reinterpret_cast<proto::Header*>(base + proto::kOffHeader);
    auto& state = reinterpret_cast<proto::OverlayCtl*>(base + proto::kOffOverlayCtl)->state;
    const uint64_t now = GetTickCount64();
    const uint64_t beat = AtomicLoad(hdr->mcHeartbeatMs);
    const bool alive = beat != 0 && now - beat < kMcTimeoutMs;
    const uint32_t pid = AtomicLoad(hdr->mcPid);
    if (alive && pid != 0 && pid != lastPid)
    {
        // A (new) Minecraft process: its writer starts at back slot 1 with middle 0, so restart
        // our side as reader with front 2 (SkyCraft's Link::ResetOverlay on "connected").
        lastPid = pid;
        std::atomic_ref<uint32_t>(state).store(0, std::memory_order_release);
        front = 2;
        WLOG("overlay: Minecraft pid %u -> overlay triple buffer reset (reader front = 2)", pid);
    }
    if (!alive || now - g_wantFramesTick.load(std::memory_order_relaxed) > 500)
        return false;
    if (!(std::atomic_ref<uint32_t>(state).load(std::memory_order_acquire) & proto::kOverlayDirty))
        return false;

    // A buffer to fill: a free one, else the oldest unconsumed Ready one.
    AcquireSRWLockExclusive(&g_upLock);
    UploadBuf* buf = nullptr;
    for (auto& b : g_up)
        if (b.state == kUpFree)
        {
            buf = &b;
            break;
        }
    if (!buf)
        for (auto& b : g_up)
            if (b.state == kUpReady && (!buf || b.seq < buf->seq))
                buf = &b;
    if (buf)
        buf->state = kUpFilling;
    ReleaseSRWLockExclusive(&g_upLock);
    if (!buf)
        return false;

    // Exactly SkyCraft's Link::AcquireOverlayFrame: swap only when the dirty bit is set.
    const uint32_t old = std::atomic_ref<uint32_t>(state).exchange(front, std::memory_order_acq_rel);
    front = old & 3;
    g_stats.acquired.fetch_add(1, std::memory_order_relaxed);
    const auto* sh = reinterpret_cast<const proto::OverlaySlotHdr*>(base + proto::kOffOverlaySlotHdr) + front;
    const uint32_t w = sh->width, h = sh->height, flags = sh->flags;
    const uint64_t frameId = sh->frameId;
    if (flags & proto::kOverlayHasPose)
    {
        StorePose(*sh); // M3a: control.cpp places the W3 camera with it (LastDrawnPose)
        static uint64_t s_poseLogs = 0;
        const uint64_t n = s_poseLogs++;
        if (n == 0 || n == 300 || n % 3600 == 0) // first one, ~5 s later, then about once a minute
            WLOG("overlay: world-layer frame %llu camera MC (%.3f, %.3f, %.3f) yaw %.2f pitch %.2f roll %.2f vfov %.2f",
                 static_cast<unsigned long long>(frameId), sh->camEyeX, sh->camEyeY, sh->camEyeZ, sh->camYaw, sh->camPitch,
                 sh->camRollDeg, sh->camFovDeg);
    }
    const uint8_t* src = base + proto::kOffOverlayPixels + proto::kOverlaySlotBytes * front;

    bool ready = false;
    if (front <= 2 && w && h && w <= proto::kMaxOverlayW && h <= proto::kMaxOverlayH)
    {
        const uint32_t pitch = Align(w * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        // Minecraft's depth plane (occlusion): same slot, after the colour.
        const auto* dh = reinterpret_cast<const proto::OverlayDepthHdr*>(src + proto::OverlayDepthOffset(w, h));
        const bool depth = (flags & proto::kOverlayHasDepth) && proto::OverlayDepthFits(w, h) && dh->magic == proto::kOverlayDepthMagic &&
                           dh->width == w && dh->height == h && dh->format == 1 && dh->b != 0.0f;
        if ((flags & proto::kOverlayHasDepth) && !depth)
            g_depthBad.fetch_add(1, std::memory_order_relaxed);
        const uint64_t depthOffset = (uint64_t(pitch) * h + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                                     ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
        const uint64_t need = depth ? depthOffset + uint64_t(pitch) * h : uint64_t(pitch) * h;
        if (buf->capacity < need)
        {
            if (buf->res)
                buf->res->Release(); // Free/Ready buffers are never referenced by in-flight GPU work
            buf->res = nullptr;
            buf->ptr = nullptr;
            buf->capacity = 0;
            const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
            const D3D12_RESOURCE_DESC rd = BufferDesc(need);
            HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                         nullptr, IID_PPV_ARGS(&buf->res));
            void* p = nullptr;
            const D3D12_RANGE noRead{0, 0};
            if (SUCCEEDED(hr))
                hr = buf->res->Map(0, &noRead, &p);
            if (FAILED(hr) || !p)
            {
                Disable("upload buffer creation/map failed", hr);
                return false;
            }
            buf->ptr = static_cast<uint8_t*>(p);
            buf->capacity = need;
            WLOG("overlay: upload buffer %d: %ux%u (%.1f MB)", static_cast<int>(buf - g_up), w, h, need / 1048576.0);
        }
        const uint32_t row = w * 4;
        if (pitch == row)
            std::memcpy(buf->ptr, src, size_t(row) * h);
        else
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(buf->ptr + size_t(y) * pitch, src + size_t(y) * row, row);
        buf->hasDepth = depth;
        if (depth)
        {
            const uint8_t* dsrc = reinterpret_cast<const uint8_t*>(dh) + proto::kOverlayDepthDataOff;
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(buf->ptr + depthOffset + size_t(y) * pitch, dsrc + size_t(y) * row, row);
            buf->depthOffset = depthOffset;
            buf->depthPitch = pitch;
            buf->depthA = dh->a, buf->depthB = dh->b, buf->depthClear = dh->clearDepth;
            g_depthFrames.fetch_add(1, std::memory_order_relaxed);
        }
        buf->w = w;
        buf->h = h;
        buf->pitch = pitch;
        buf->flags = flags;
        buf->frameId = frameId;
        buf->pose = {};
        if (flags & proto::kOverlayHasPose)
        {
            buf->pose.frameId = frameId;
            buf->pose.eye[0] = sh->camEyeX, buf->pose.eye[1] = sh->camEyeY, buf->pose.eye[2] = sh->camEyeZ;
            buf->pose.yaw = sh->camYaw, buf->pose.pitch = sh->camPitch, buf->pose.roll = sh->camRollDeg, buf->pose.fov = sh->camFovDeg;
        }
        ready = true;
        wc::camsync::OnMcFrame();
    }
    AcquireSRWLockExclusive(&g_upLock);
    buf->seq = ++seq;
    buf->state = ready ? kUpReady : kUpFree;
    ReleaseSRWLockExclusive(&g_upLock);
    return true;
}

bool SafeWorkerStep(uint32_t& front, uint32_t& lastPid, uint64_t& seq)
{
    __try
    {
        return WorkerStep(front, lastPid, seq);
    }
    __except (SehFilter(GetExceptionCode(), "overlay worker"))
    {
        return false;
    }
}

DWORD WINAPI WorkerThread(void*)
{
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    uint32_t front = 2, lastPid = 0;
    uint64_t seq = 0;
    WLOG("overlay: worker thread running");
    while (!g_disabled.load(std::memory_order_relaxed))
    {
        if (SafeWorkerStep(front, lastPid, seq))
            continue;
        if (timer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -10000; // 1 ms
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 20);
        }
        else
        {
            Sleep(1);
        }
    }
    WLOG("overlay: worker thread stopped");
    if (timer)
        CloseHandle(timer);
    return 0;
}

// ---- renderer ---------------------------------------------------------------------------------
constexpr char kShader[] = R"(
cbuffer C : register(b0) {
	float4 invertRect; float2 texSize; float2 bbSize; float flipY; uint mode; float paperWhite; float bilinear;
	float occl; float mcA; float mcB; float mcClear;   // occlusion on; MC depth: z = mcB / (d - mcA), d == mcClear: nothing
	float w3A; float w3B; float w3Far; float bias;     // W3 depth: z = w3B / (d - w3A), d == w3Far: sky
	float2 w3Size; float2 w3Used; float soft; float dbg; float edgeAA; float keepNear; };
Texture2D<float4> overlay : register(t0);
Texture2D<float> mcDepth : register(t1);
Texture2D<float> w3Depth : register(t2);
struct VSOut { float4 pos : SV_Position; };
VSOut VSMain(uint id : SV_VertexID) {
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
float4 Texel(int2 t) {
	t = clamp(t, int2(0, 0), int2(texSize) - 1);
	if (flipY > 0.5) t.y = int(texSize.y) - 1 - t.y;   // Minecraft's rows are bottom-up
	return overlay.Load(int3(t, 0));                  // premultiplied alpha, sRGB-encoded
}
// The overlay texel under this back-buffer pixel: exact 1:1 point fetch when the sizes match;
// when stretched, point (crisp HUD) or - for the world layer - bilinear on the premultiplied texels.
float4 Fetch(float2 p) {
	float2 tp = p * texSize / bbSize;
	if (bilinear < 0.5 || all(texSize == bbSize)) return Texel(int2(tp));
	float2 f = tp - 0.5;
	int2 i = int2(floor(f));
	float2 w = f - floor(f);
	return lerp(lerp(Texel(i), Texel(i + int2(1, 0)), w.x), lerp(Texel(i + int2(0, 1)), Texel(i + int2(1, 1)), w.x), w.y);
}
bool InInvert(float2 p) { return all(p >= invertRect.xy) && all(p < invertRect.zw); }
// ---- occlusion ----
float McZ(float2 p) {                 // metres along the view axis, or -1 (no MC depth: HUD, sky, particles)
	int2 t = clamp(int2(p * texSize / bbSize), int2(0, 0), int2(texSize) - 1);
	if (flipY > 0.5) t.y = int(texSize.y) - 1 - t.y;
	float d = mcDepth.Load(int3(t, 0));
	if (d == mcClear) return -1.0;
	float z = mcB / (d - mcA);
	return z > 0.0 ? z : -1.0;
}
float W3Z(int2 t) {
	t = clamp(t, int2(0, 0), int2(w3Size) - 1);
	float d = w3Depth.Load(int3(t, 0));
	if (abs(d - w3Far) < 1e-7) return 1e9;
	float den = d - w3A;
	float z = den != 0.0 ? w3B / den : 1e9;
	return z > 0.0 ? z : 1e9;
}
float Vis1(float zw, float zm) {      // 1 = Minecraft in front, 0 = the Witcher surface hides it
	float b = bias + 0.01 * zm;
	float s = max(soft, 1e-4);
	return saturate((zw + b - zm) / s + 0.5);
}
// Bilinear "PCF" over the 4 nearest W3 depth texels: smooth edges also when W3 renders at a lower
// resolution than the back buffer (FSR/DLSS/XeSS upscaling).
float Visibility(float2 p, float zm) {
	float2 q = p / bbSize * w3Used - 0.5;
	int2 i = int2(floor(q));
	float2 f = q - floor(q);
	float v00 = Vis1(W3Z(i), zm), v10 = Vis1(W3Z(i + int2(1, 0)), zm);
	float v01 = Vis1(W3Z(i + int2(0, 1)), zm), v11 = Vis1(W3Z(i + int2(1, 1)), zm);
	return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}
// One Minecraft sample (premultiplied colour c at distance zm) depth-tested at back-buffer pixel p.
// No Minecraft depth (zm < 0: HUD, screens, sky) or nearer than keepNear (the hand): on top.
float4 Occlude(float4 c, float zm, float2 p) {
	if (occl < 0.5 || c.a <= 0.0 || zm <= keepNear) return c;
	float v = Visibility(p, zm);
	if (dbg > 0.5) { c.rgb = lerp(float3(c.a, 0, 0), c.rgb, v); return c; }   // debug view 1: hidden parts red
	return c * v;                                                             // premultiplied: scale everything
}
// The Minecraft depth behind Fetch(p): the point sample, or - when the world layer is stretched
// bilinearly - the NEAREST of the 4 texels blended into the colour (an edge texel without depth
// would otherwise leave its blended neighbour's colour unoccluded).
float McZSrc(float2 p) {
	if (bilinear < 0.5 || all(texSize == bbSize)) return McZ(p);
	float2 f = p * texSize / bbSize - 0.5;
	float2 b = (floor(f) + 0.5) * bbSize / texSize;   // back-buffer position of texel floor(f)
	float2 s = bbSize / texSize;
	float z = 1e30, zi;
	zi = McZ(b);                    if (zi > 0.0) z = min(z, zi);
	zi = McZ(b + float2(s.x, 0));   if (zi > 0.0) z = min(z, zi);
	zi = McZ(b + float2(0, s.y));   if (zi > 0.0) z = min(z, zi);
	zi = McZ(b + s);                if (zi > 0.0) z = min(z, zi);
	return z < 1e29 ? z : -1.0;
}
float3 SrgbToLinear(float3 c) { return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045)); }
float3 PqEncode(float3 nits) {
	float3 y = pow(saturate(nits / 10000.0), 0.1593017578125);
	return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}
static const float3x3 k709To2020 = {
	0.6274040, 0.3292820, 0.0433136,
	0.0690970, 0.9195400, 0.0113612,
	0.0163916, 0.0880132, 0.8955950 };
float4 Encode(float4 c) {
	if (mode == 0) return c;                         // SDR: blend in display space like Minecraft does
	if (c.a <= 0.0) return float4(0, 0, 0, 0);
	float3 lin = SrgbToLinear(saturate(c.rgb / c.a));
	if (mode == 1) return float4(lin * (paperWhite / 80.0) * c.a, c.a);   // scRGB: 1.0 = 80 nits
	float3 nits = max(mul(k709To2020, lin), 0.0) * paperWhite;            // HDR10: BT.2020 + PQ
	return float4(PqEncode(nits) * c.a, c.a);
}
float4 PSMain(VSOut i) : SV_Target {
	if (InInvert(i.pos.xy)) return float4(0, 0, 0, 0);
	if (dbg > 1.5) {                  // debug view 2: the W3 depth in use, log-scaled grey (near white)
		float z = W3Z(int2(i.pos.xy / bbSize * w3Used));
		float g = z >= 1e8 ? 0.0 : saturate(1.0 - log2(max(z, 0.25)) / 10.0);
		float4 c = Fetch(i.pos.xy);
		return Encode(float4(lerp(float3(g, g, g), c.rgb, 0.35 * c.a), 1.0));
	}
	float4 c = Fetch(i.pos.xy);
	float2 p = i.pos.xy;
	// Silhouette AA (OverlayEdgeAA): Minecraft has no anti-aliasing, W3 has TAA/FSR; soften only the
	// 1-px staircase where Minecraft meets the Witcher picture (alpha edges), never texture detail.
	// M6b: every sample of the blend is depth-tested with ITS OWN Minecraft depth before blending.
	// (Before, the blended pixel was tested with the centre's depth; the ring just outside a block's
	// silhouette has no Minecraft depth, so it was never hidden: the faint outline of occluded blocks.)
	if (edgeAA > 0.5) {
		float4 n0 = Fetch(p + float2(1, 0)), n1 = Fetch(p - float2(1, 0));
		float4 n2 = Fetch(p + float2(0, 1)), n3 = Fetch(p - float2(0, 1));
		float amax = max(max(n0.a, n1.a), max(n2.a, n3.a)), amin = min(min(n0.a, n1.a), min(n2.a, n3.a));
		float zc = McZ(p), z0 = McZ(p + float2(1, 0)), z1 = McZ(p - float2(1, 0)), z2 = McZ(p + float2(0, 1)), z3 = McZ(p - float2(0, 1));
		// Only level geometry (it has Minecraft depth): HUD text and icons stay pixel-sharp.
		if (amax - amin > 0.5 && max(max(zc, z0), max(max(z1, z2), z3)) > 0.0) {
			if (edgeAA > 1.5)   // env WC_EDGEAA_LEGACY=1: the pre-M6b order (before/after captures only)
				return Encode(Occlude(c * 0.5 + (n0 + n1 + n2 + n3) * 0.125, zc, p));
			return Encode(Occlude(c, zc, p) * 0.5 + (Occlude(n0, z0, p) + Occlude(n1, z1, p) + Occlude(n2, z2, p) + Occlude(n3, z3, p)) * 0.125);
		}
	}
	if (occl > 0.5) c = Occlude(c, McZSrc(p), p);   // (a branch: HLSL ?: evaluates both sides)
	return Encode(c);
}
// Crosshair/attack indicator: Minecraft's invert blend against the game's picture (SDR only).
float4 PSInvert(VSOut i) : SV_Target {
	if (!InInvert(i.pos.xy)) discard;
	return float4(Fetch(i.pos.xy).rgb, 0);
}
)";

struct Consts
{
    float invertRect[4];
    float texSize[2];
    float bbSize[2];
    float flipY;
    uint32_t mode;
    float paperWhite;
    float bilinear;
    float occl, mcA, mcB, mcClear;
    float w3A, w3B, w3Far, bias;
    float w3Size[2], w3Used[2];
    float soft, dbg, edgeAA, keepNear;
};
static_assert(sizeof(Consts) == 28 * 4);

constexpr int kFrames = 3;
constexpr int kMaxHistory = 8;        // Minecraft frames kept on the GPU (ini OverlayHistory)
constexpr int kSrvPerHist = 3;          // t0 colour, t1 MC depth, t2 W3 depth: one table per FRAME CONTEXT,
constexpr int kSrvSlots = kFrames * kSrvPerHist; // written when that context records (its GPU use is done)
constexpr int kProbeSlots = 4;        // image-probe readbacks in flight
constexpr uint32_t kProbeRows = 48;   // band height (rows around the screen centre)
constexpr uint32_t kProbeBin = 4;     // pixels per profile bin

struct FrameCtx
{
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    uint64_t fence = 0;
};

// One Minecraft frame on the GPU (frame-matched compositing).
struct HistTex
{
    ID3D12Resource* res = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    bool valid = false;
    uint32_t flags = 0;
    uint64_t frameId = 0, seq = 0;
    bool hasPose = false;
    camsync::CamPose pose;
    // Minecraft's depth of this frame (occlusion)
    ID3D12Resource* depth = nullptr;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COPY_DEST;
    bool hasDepth = false;
    float depthA = 0, depthB = 0, depthClear = 0;
};

// Image probe: a band of the Witcher's picture copied back every Present, to measure how far the
// picture moved (and so which game frame's camera it was rendered with).
struct ProbeSlot
{
    ID3D12Resource* buf = nullptr;
    uint64_t bytes = 0;
    uint64_t fence = 0;   // 0 = free
    uint64_t present = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, rows = 0, pitch = 0;
};

struct Grave
{
    IUnknown* obj;
    uint64_t fence;
};

struct Renderer
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr; // the present queue (occlusion list submits on it too)
    ID3D12RootSignature* rootSig = nullptr;
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* psInvert = nullptr;
    ID3D12PipelineState* psoMain = nullptr;
    ID3D12PipelineState* psoInvert = nullptr;
    DXGI_FORMAT psoFormat = DXGI_FORMAT_UNKNOWN;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    UINT rtvInc = 0;
    ID3D12DescriptorHeap* srvHeap = nullptr;
    UINT srvInc = 0;
    HistTex hist[kMaxHistory];
    int histCount = 0;     // textures in use (ini OverlayHistory)
    uint64_t histSeq = 0;
    uint32_t texW = 0, texH = 0, texFlags = 0; // texFlags: of the frame being drawn
    bool texValid = false;                     // at least one frame uploaded
    uint32_t lastUploadFlags = 0;
    ProbeSlot probe[kProbeSlots];
    std::vector<float> probePrev, probeCur;
    uint64_t probePrevPresent = 0;
    FrameCtx ctx[kFrames];
    uint32_t frame = 0;
    ID3D12Fence* fence = nullptr;
    uint64_t fenceValue = 0;
    HANDLE fenceEvent = nullptr;
    Grave grave[32]{};
    int graveCount = 0;
    // capture (debug/test)
    ID3D12Resource* readback = nullptr;
    uint64_t readbackBytes = 0;
    uint64_t captureFence = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT captureFp{};
    DXGI_FORMAT captureFmt = DXGI_FORMAT_UNKNOWN;
    int captureMode = 0;
    float capturePaperWhite = 200;
} g_r;

SRWLOCK g_renderLock = SRWLOCK_INIT;
IDXGISwapChain* g_primary = nullptr;
uint64_t g_primaryLast = 0, g_primaryPresents = 0;
uint32_t g_vpW = 0, g_vpH = 0;
uint64_t g_timingMatch = 0, g_timingOther = 0, g_timingNone = 0;
uint64_t g_lastStatusLog = 0;

std::atomic<bool> g_captureRequested{false};
std::atomic<bool> g_captureDone{false};
wchar_t g_capturePath[MAX_PATH * 2]{};

int g_paperWhite = 0;        // resolved nits
int g_gameHdrEnabled = -1;   // -1 unknown, from dx12user.settings
struct ModeCache
{
    const void* sc = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    int cs = -2;
    int mode = 0;
} g_modeCache;

void Bury(IUnknown* obj)
{
    if (!obj)
        return;
    if (g_r.graveCount == static_cast<int>(sizeof(g_r.grave) / sizeof(g_r.grave[0])))
    {
        // Full (shouldn't happen): leak rather than free something the GPU may still use.
        WLOG("overlay: graveyard full; leaking %p", static_cast<void*>(obj));
        return;
    }
    g_r.grave[g_r.graveCount++] = {obj, g_r.fenceValue};
}

void Retire(uint64_t completed)
{
    for (int i = 0; i < g_r.graveCount;)
    {
        if (g_r.grave[i].fence <= completed)
        {
            g_r.grave[i].obj->Release();
            g_r.grave[i] = g_r.grave[--g_r.graveCount];
        }
        else
        {
            ++i;
        }
    }
    AcquireSRWLockExclusive(&g_upLock);
    for (auto& b : g_up)
        if (b.state == kUpInFlight && b.fence != 0 && b.fence <= completed)
            b.state = kUpFree;
    ReleaseSRWLockExclusive(&g_upLock);
}

// Reads HdrEnabled / HdrPaperWhite from Documents\The Witcher 3\dx12user.settings once.
void ReadGameHdrSettings()
{
    if (g_gameHdrEnabled != -1)
        return;
    g_gameHdrEnabled = 0;
    int pw = 0;
    using PFN_SHGetKnownFolderPath = HRESULT(WINAPI*)(REFGUID, DWORD, HANDLE, PWSTR*);
    using PFN_CoTaskMemFree = void(WINAPI*)(LPVOID);
    auto getPath = reinterpret_cast<PFN_SHGetKnownFolderPath>(SysProc(L"shell32.dll", "SHGetKnownFolderPath"));
    auto memFree = reinterpret_cast<PFN_CoTaskMemFree>(SysProc(L"ole32.dll", "CoTaskMemFree"));
    static const GUID kDocuments = {0xFDD39AD0, 0x238F, 0x46AF, {0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7}};
    PWSTR docs = nullptr;
    if (getPath && SUCCEEDED(getPath(kDocuments, 0, nullptr, &docs)) && docs)
    {
        wchar_t path[MAX_PATH * 2];
        swprintf_s(path, L"%s\\The Witcher 3\\dx12user.settings", docs);
        if (memFree)
            memFree(docs);
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            static char buf[256 * 1024];
            DWORD n = 0;
            ReadFile(f, buf, sizeof(buf) - 1, &n, nullptr);
            CloseHandle(f);
            buf[n] = 0;
            if (const char* p = strstr(buf, "HdrEnabled="))
                g_gameHdrEnabled = _strnicmp(p + 11, "true", 4) == 0 ? 1 : 0;
            if (const char* p = strstr(buf, "HdrPaperWhite="))
                pw = atoi(p + 14);
            WLOG("overlay: game settings %ls: HdrEnabled=%d HdrPaperWhite=%d", path, g_gameHdrEnabled, pw);
        }
    }
    g_paperWhite = g_config.overlayPaperWhite > 0 ? g_config.overlayPaperWhite : pw > 0 ? pw : 200;
    if (g_paperWhite < 80)
        g_paperWhite = 80;
    if (g_paperWhite > 1000)
        g_paperWhite = 1000;
    WLOG("overlay: HDR paper white for the HUD = %d nits (%s)", g_paperWhite,
         g_config.overlayPaperWhite > 0 ? "ini OverlayPaperWhite" : pw > 0 ? "game HdrPaperWhite" : "default");
}

bool OutputIsHdr(IDXGISwapChain* sc)
{
    IDXGIOutput* out = nullptr;
    bool hdr = false;
    if (SUCCEEDED(sc->GetContainingOutput(&out)) && out)
    {
        IDXGIOutput6* o6 = nullptr;
        if (SUCCEEDED(out->QueryInterface(IID_PPV_ARGS(&o6))) && o6)
        {
            DXGI_OUTPUT_DESC1 d{};
            if (SUCCEEDED(o6->GetDesc1(&d)))
                hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            o6->Release();
        }
        out->Release();
    }
    return hdr;
}

// 0 SDR, 1 scRGB, 2 HDR10/PQ
int DecideMode(IDXGISwapChain* sc, const ScRec& rec, DXGI_FORMAT fmt)
{
    if (g_modeCache.sc == sc && g_modeCache.fmt == fmt && g_modeCache.cs == rec.colorSpace)
        return g_modeCache.mode;
    ReadGameHdrSettings();
    int mode = 0;
    const char* why = "default (no colour space set)";
    auto fromCs = [](int cs) {
        return cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709         ? 1
               : cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 2
                                                                  : 0;
    };
    if (g_config.overlayColorSpace >= 0)
    {
        mode = fromCs(g_config.overlayColorSpace);
        why = "ini OverlayColorSpace";
    }
    else if (rec.colorSpace >= 0)
    {
        mode = fromCs(rec.colorSpace);
        why = "IDXGISwapChain3::SetColorSpace1";
    }
    else if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        mode = 1;
        why = "FP16 back buffer (scRGB assumed)";
    }
    else if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM && g_gameHdrEnabled == 1 && OutputIsHdr(sc))
    {
        mode = 2;
        why = "10-bit back buffer + game HdrEnabled + HDR display";
    }
    WLOG("overlay: back buffer %s, colour space %s -> HUD encoding %s (%s), paper white %d nits", FormatName(fmt),
         ColorSpaceName(rec.colorSpace), ModeName(mode), why, g_paperWhite);
    g_modeCache = {sc, fmt, rec.colorSpace, mode};
    g_stats.colorMode = mode;
    return mode;
}

bool Compile(pD3DCompile compile, const char* entry, const char* target, ID3DBlob** out)
{
    ID3DBlob* errors = nullptr;
    const HRESULT hr = compile(kShader, sizeof(kShader) - 1, "witchercraft_overlay", nullptr, nullptr, entry, target,
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
    if (FAILED(hr))
        WLOG("overlay: shader %s failed 0x%08lX: %s", entry, static_cast<unsigned long>(hr),
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    if (errors)
        errors->Release();
    return SUCCEEDED(hr);
}

void InitOcclusionLists(); // occlusion runtime below

bool InitRenderer(ID3D12CommandQueue* queue)
{
    HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&g_r.device));
    if (FAILED(hr) || !g_r.device)
    {
        Disable("queue->GetDevice failed", hr);
        return false;
    }
    auto compile = reinterpret_cast<pD3DCompile>(SysProc(L"d3dcompiler_47.dll", "D3DCompile"));
    if (!compile)
    {
        Disable("d3dcompiler_47.dll / D3DCompile not available");
        return false;
    }
    if (!Compile(compile, "VSMain", "vs_5_0", &g_r.vs) || !Compile(compile, "PSMain", "ps_5_0", &g_r.ps) ||
        !Compile(compile, "PSInvert", "ps_5_0", &g_r.psInvert))
    {
        Disable("shader compilation failed");
        return false;
    }

    // Root signature: b0 = 12 constants, t0 = overlay texture (descriptor table).
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kSrvPerHist;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = sizeof(Consts) / 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    auto serialize = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(SysProc(L"d3d12.dll", "D3D12SerializeRootSignature"));
    ID3DBlob* rsBlob = nullptr;
    ID3DBlob* rsErr = nullptr;
    hr = serialize ? serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr) : E_NOINTERFACE;
    if (rsErr)
        rsErr->Release();
    if (SUCCEEDED(hr))
        hr = g_r.device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&g_r.rootSig));
    if (rsBlob)
        rsBlob->Release();
    if (FAILED(hr))
    {
        Disable("root signature failed", hr);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kFrames;
    hr = g_r.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.rtvHeap));
    if (SUCCEEDED(hr))
    {
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kSrvSlots;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = g_r.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_r.srvHeap));
    }
    if (FAILED(hr))
    {
        Disable("descriptor heaps failed", hr);
        return false;
    }
    g_r.rtvInc = g_r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_r.srvInc = g_r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (auto& c : g_r.ctx)
    {
        hr = g_r.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.alloc));
        if (SUCCEEDED(hr))
            hr = g_r.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.alloc, nullptr, IID_PPV_ARGS(&c.list));
        if (SUCCEEDED(hr))
            hr = c.list->Close();
        if (FAILED(hr))
        {
            Disable("command allocator/list failed", hr);
            return false;
        }
    }
    InitOcclusionLists();
    hr = g_r.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_r.fence));
    g_r.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || !g_r.fenceEvent)
    {
        Disable("fence failed", hr);
        return false;
    }
    g_workerDevice = g_r.device;
    g_r.queue = queue;
    HANDLE t = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (!t)
    {
        Disable("worker thread failed");
        return false;
    }
    CloseHandle(t);
    WLOG("overlay: renderer ready on device %p (queue %p)", static_cast<void*>(g_r.device), static_cast<void*>(queue));
    SafeInstallDepthHooks(g_r.device, "renderer init"); // no-op if the swapchain-creation path already did it
    camsync::SetLatency(g_config.overlayLatencyFrames);
    camsync::SetFrameMatch(g_config.overlayFrameMatch);
    WLOG("overlay: OverlayHistory=%d OverlayProbe=%d", g_config.overlayHistory, g_config.overlayProbe ? 1 : 0);
    return true;
}

bool EnsurePso(DXGI_FORMAT fmt)
{
    if (g_r.psoMain && g_r.psoFormat == fmt)
        return true;
    Bury(g_r.psoMain);
    Bury(g_r.psoInvert);
    g_r.psoMain = g_r.psoInvert = nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_r.rootSig;
    pd.VS = {g_r.vs->GetBufferPointer(), g_r.vs->GetBufferSize()};
    pd.PS = {g_r.ps->GetBufferPointer(), g_r.ps->GetBufferSize()};
    auto& rt = pd.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_ONE; // premultiplied alpha straight from Minecraft
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;
    HRESULT hr = g_r.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.psoMain));
    if (SUCCEEDED(hr))
    {
        // Minecraft's BlendFunction.INVERT: out = src * (1 - dst) + dst * (1 - src).
        pd.PS = {g_r.psInvert->GetBufferPointer(), g_r.psInvert->GetBufferSize()};
        rt.SrcBlend = D3D12_BLEND_INV_DEST_COLOR;
        rt.DestBlend = D3D12_BLEND_INV_SRC_COLOR;
        hr = g_r.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_r.psoInvert));
    }
    if (FAILED(hr))
    {
        Disable("pipeline state creation failed", hr);
        return false;
    }
    g_r.psoFormat = fmt;
    WLOG("overlay: pipelines built for %s", FormatName(fmt));
    return true;
}

// The history of Minecraft frames on the GPU (OverlayHistory textures of one size). Recreated only
// when none of our GPU work is in flight (the caller checks), so descriptors can be rewritten.
bool EnsureHistory(uint32_t w, uint32_t h)
{
    const int want = g_config.overlayHistory < 2 ? 2 : g_config.overlayHistory > kMaxHistory ? kMaxHistory : g_config.overlayHistory;
    if (g_r.histCount == want && g_r.texW == w && g_r.texH == h)
        return true;
    for (auto& t : g_r.hist)
    {
        Bury(t.res);
        Bury(t.depth);
        t = HistTex{};
    }
    g_r.histCount = 0;
    g_r.texValid = false;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    for (int i = 0; i < want; ++i)
    {
        HistTex& t = g_r.hist[i];
        const HRESULT hr =
            g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t.res));
        if (FAILED(hr))
        {
            Disable("overlay texture creation failed", hr);
            return false;
        }
        t.state = D3D12_RESOURCE_STATE_COPY_DEST;
        // Minecraft's depth of the same frame (occlusion).
        D3D12_RESOURCE_DESC dd = td;
        dd.Format = DXGI_FORMAT_R32_FLOAT;
        const HRESULT hd =
            g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t.depth));
        if (FAILED(hd))
        {
            Disable("depth texture creation failed", hd);
            return false;
        }
        t.depthState = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    g_r.histCount = want;
    g_r.texW = w;
    g_r.texH = h;
    g_stats.texW = w;
    g_stats.texH = h;
    WLOG("overlay: %d overlay textures %ux%u + Minecraft depth (frame history for frame-matched compositing, %.0f MB)", want, w, h,
         want * double(w) * h * 8 / 1048576.0);
    return true;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

// Copies a frame into the oldest history texture. Our command lists run in order on one queue,
// so an earlier list still reading that texture finishes before this copy.
void RecordUpload(ID3D12GraphicsCommandList* list, const UploadBuf& up)
{
    int slot = 0;
    for (int i = 1; i < g_r.histCount; ++i)
        if (g_r.hist[i].seq < g_r.hist[slot].seq)
            slot = i;
    HistTex& t = g_r.hist[slot];
    if (t.state != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        const auto b = Transition(t.res, t.state, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = t.res;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = up.res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, up.w, up.h, 1, up.pitch};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto b = Transition(t.res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &b);
    t.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    t.hasDepth = up.hasDepth;
    if (up.hasDepth && t.depth)
    {
        if (t.depthState != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            const auto db = Transition(t.depth, t.depthState, D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &db);
        }
        D3D12_TEXTURE_COPY_LOCATION ddst{};
        ddst.pResource = t.depth;
        ddst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dsrc{};
        dsrc.pResource = up.res;
        dsrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dsrc.PlacedFootprint.Offset = up.depthOffset;
        dsrc.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, up.w, up.h, 1, up.depthPitch};
        list->CopyTextureRegion(&ddst, 0, 0, 0, &dsrc, nullptr);
        const auto db = Transition(t.depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &db);
        t.depthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        t.depthA = up.depthA, t.depthB = up.depthB, t.depthClear = up.depthClear;
    }
    t.valid = true;
    t.flags = up.flags;
    t.frameId = up.frameId;
    t.seq = ++g_r.histSeq;
    t.hasPose = (up.flags & proto::kOverlayHasPose) && up.pose.fov >= 10.0f;
    t.pose = up.pose;
    if ((up.flags ^ g_r.lastUploadFlags) & proto::kOverlayWorldLayer || !g_r.texValid)
        WLOG("overlay: Minecraft frames are %s", (up.flags & proto::kOverlayWorldLayer)
                                                      ? "the WORLD LAYER (full MC render on transparent; crosshair invert off)"
                                                      : "HUD-only (upstream SkyCraft)");
    g_r.lastUploadFlags = up.flags;
    g_r.texValid = true;
}

// How different two cameras are, in degrees of view direction (+ 20 deg per metre of eye offset).
float PoseDistance(const camsync::CamPose& a, const camsync::CamPose& b)
{
    float dy = std::fmod(a.yaw - b.yaw, 360.0f);
    if (dy > 180.0f)
        dy -= 360.0f;
    else if (dy < -180.0f)
        dy += 360.0f;
    const float c = std::cos((a.pitch + b.pitch) * 0.5f * 0.0174532925f);
    const float dp = a.pitch - b.pitch;
    const double ex = a.eye[0] - b.eye[0], ey = a.eye[1] - b.eye[1], ez = a.eye[2] - b.eye[2];
    return std::sqrt(dy * dy * c * c + dp * dp) + 20.0f * static_cast<float>(std::sqrt(ex * ex + ey * ey + ez * ez));
}

// Which history texture to draw for this Present.
int PickFrame(const camsync::PresentInfo& pi, camsync::Pick& how, float& err)
{
    int newest = -1;
    for (int i = 0; i < g_r.histCount; ++i)
        if (g_r.hist[i].valid && (newest < 0 || g_r.hist[i].seq > g_r.hist[newest].seq))
            newest = i;
    how = camsync::Pick::Newest;
    err = 0;
    if (newest < 0 || !camsync::FrameMatch() || !pi.havePose || !g_r.hist[newest].hasPose)
        return newest;
    int nearest = -1;
    float best = 1e9f;
    for (int i = 0; i < g_r.histCount; ++i)
    {
        const HistTex& t = g_r.hist[i];
        if (!t.valid || !t.hasPose)
            continue;
        if (pi.pose.frameId != 0 && t.frameId == pi.pose.frameId)
        {
            how = camsync::Pick::Exact;
            return i;
        }
        const float d = PoseDistance(t.pose, pi.pose);
        if (d < best)
            best = d, nearest = i;
    }
    if (nearest < 0)
        return newest;
    how = camsync::Pick::Nearest;
    err = best;
    return nearest;
}

// ---- occlusion runtime ----------------------------------------------------
// Per Present (present thread only):
//   1. pick the W3 depth the game used most since the previous Present (dynamic resolution: it can
//      change every frame), re-read its desc under our own reference (a pointer can be reused by
//      a new resource), and copy its depth plane into a cached texture of exactly that size/format;
//   2. all of that is recorded in a SEPARATE command list, executed and fenced on its own: if
//      anything in it fails, only occlusion is switched off (g_occ.failed), never the overlay;
//   3. the compositor's descriptor table of this frame context gets the copy's SRV (no shared
//      descriptors to rewrite, nothing to wait for).
struct CalPair
{
    float x; // 1 / z (MC metres)
    float d; // W3 raw depth
};

struct CopyTex
{
    ID3D12Resource* res = nullptr;
    uint32_t w = 0, h = 0;
    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN, srv = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    uint64_t lastUse = 0;
};
constexpr int kCopyCache = 8; // dynamic resolution walks through many sizes

struct ReadbackMeta
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    uint32_t usedW = 0, usedH = 0;
    bool cal = false;
};

struct Occlusion
{
    // failure isolation
    bool failed = false;
    std::string failWhy;
    // this frame
    bool haveCopy = false;      // a W3 depth copy was recorded for this Present
    CopyTex* cur = nullptr;
    uint32_t usedW = 0, usedH = 0;
    int chosen = -1, lastChosen = -1;
    uint64_t switches = 0, framesWith = 0, framesWithout = 0;
    std::string why = "no W3 depth seen yet";
    CopyTex copies[kCopyCache];
    // own command lists (one per frame context) + their fence values
    ID3D12CommandAllocator* alloc[kFrames]{};
    ID3D12GraphicsCommandList* list[kFrames]{};
    uint64_t fence[kFrames]{};
    // model (z = B / (d - A)); detection
    int autoReverse = -1;
    const char* reverseWhy = "default";
    bool calibrated = false;
    float calA = 0, calB = 0;
    std::vector<CalPair> pairs;
    std::string lastCal = "not calibrated";
    // readbacks (statistics every 60 presents; calibration on request)
    ID3D12Resource* rb = nullptr;
    uint64_t rbBytes = 0;
    uint64_t rbFence = 0;
    ReadbackMeta rbMeta;
    ID3D12Resource* mcRb = nullptr;
    uint64_t mcRbBytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT mcFp{};
    float mcA = 0, mcB = 0, mcClear = 0;
    bool mcFlip = true, mcValid = false;
    camsync::CamPose calPose;
    uint32_t calBbW = 0, calBbH = 0;
    float statMedian = -1, statZero = 0, statOne = 0;
    // requests from the console / hotkeys (any thread)
    std::atomic<int> reqCal{0}, reqReset{0}, reqNext{0}, reqRetry{0};
    // counters
    uint64_t drawsOccluded = 0, drawsPlain = 0;
    std::string lastPlainWhy;
} g_occ;

SRWLOCK g_occTextLock = SRWLOCK_INIT;
std::string g_occStatus = "occlusion: not started";

void OccFail(const char* why, long hr = 0)
{
    if (g_occ.failed)
        return;
    g_occ.failed = true;
    g_occ.failWhy = why;
    g_occHooks = false; // hooks become pure pass-through
    WLOG("occlusion: DISABLED (%s, hr=0x%08lX): Minecraft is drawn on top again; the overlay itself keeps running. wcoccon(1) retries.",
         why, static_cast<unsigned long>(hr));
}

int OccReverse()
{
    return g_config.occlusionReverseZ >= 0 ? g_config.occlusionReverseZ : g_occ.autoReverse >= 0 ? g_occ.autoReverse : 1;
}

void OccModel(float& A, float& B, float& farD, const char** what)
{
    const int rev = OccReverse();
    farD = rev ? 0.0f : 1.0f;
    if (g_config.occlusionModelSet)
    {
        A = g_config.occlusionDepthA, B = g_config.occlusionDepthB;
        *what = "ini OcclusionDepthA/B";
        return;
    }
    if (g_occ.calibrated)
    {
        A = g_occ.calA, B = g_occ.calB;
        *what = "calibrated";
        return;
    }
    const float n = g_config.occlusionNear > 0.001f ? g_config.occlusionNear : 0.2f, f = g_config.occlusionFar;
    if (rev)
    {
        A = f > n ? -n / (f - n) : 0.0f;
        B = f > n ? f * n / (f - n) : n;
    }
    else
    {
        A = f > n ? f / (f - n) : 1.0f;
        B = f > n ? -f * n / (f - n) : -n;
    }
    *what = "near/far";
}

// Logs candidates the hooks registered since last time (the hooks themselves never log).
void LogNewCandidates()
{
    const int n = std::min(g_candCount.load(std::memory_order_acquire), kMaxCand);
    for (int i = 0; i < n; ++i)
    {
        DepthCand& c = g_cand[i];
        ID3D12Resource* r = c.res.load(std::memory_order_acquire);
        int expected = 0;
        if (r && c.logged.compare_exchange_strong(expected, 1))
            WLOG("occlusion: depth candidate #%d: %p %ux%u %s (present %llu)", i, static_cast<void*>(r), c.w.load(), c.h.load(),
                 DepthFormatName(static_cast<DXGI_FORMAT>(c.fmt.load())), static_cast<unsigned long long>(c.lastPresent.load()));
    }
}

// The depth the game used most in the window that just ended (stamp == win).
int PickThisFrame(uint64_t win, std::string& why)
{
    const int n = std::min(g_candCount.load(std::memory_order_acquire), kMaxCand);
    if (!g_listHooked)
    {
        why = "depth hooks not installed";
        return -1;
    }
    if (n == 0)
    {
        why = "no depth buffer with the screen's aspect ratio used by the game yet";
        return -1;
    }
    const int forced = g_config.occlusionDepthPick;
    if (forced >= 0)
    {
        if (forced < n && g_cand[forced].res.load() && g_cand[forced].frameStamp.load() == win)
            return forced;
        why = "OcclusionDepthPick=" + std::to_string(forced) + " not used this frame";
        return -1;
    }
    int best = -1;
    uint32_t bestUses = 0;
    uint64_t bestArea = 0;
    for (int i = 0; i < n; ++i)
    {
        const DepthCand& c = g_cand[i];
        if (!c.res.load() || c.frameStamp.load() != win)
            continue;
        const uint32_t u = c.frameUses.load();
        const uint64_t area = uint64_t(c.w.load()) * c.h.load();
        if (u > bestUses || (u == bestUses && area > bestArea))
            best = i, bestUses = u, bestArea = area;
    }
    if (best < 0)
        why = "no candidate depth used this frame";
    return best;
}

CopyTex* GetCopyTex(uint32_t w, uint32_t h, DXGI_FORMAT typeless, DXGI_FORMAT srv, uint64_t P, const D3D12_RESOURCE_DESC& src)
{
    CopyTex* lru = &g_occ.copies[0];
    for (CopyTex& c : g_occ.copies)
    {
        if (c.res && c.w == w && c.h == h && c.typeless == typeless)
        {
            c.lastUse = P;
            return &c;
        }
        if (!c.res || c.lastUse < lru->lastUse)
            lru = &c;
    }
    if (lru->res)
        Bury(lru->res); // the GPU may still read it: freed after our last fence
    *lru = CopyTex{};
    D3D12_RESOURCE_DESC cd = src;
    cd.Format = typeless;
    cd.MipLevels = 1;
    cd.Alignment = 0;
    cd.DepthOrArraySize = 1;
    cd.SampleDesc = {1, 0};
    cd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    const HRESULT hr =
        g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&lru->res));
    if (FAILED(hr))
    {
        lru->res = nullptr;
        OccFail("could not create the depth copy texture", hr);
        return nullptr;
    }
    lru->w = w, lru->h = h, lru->typeless = typeless, lru->srv = srv, lru->state = D3D12_RESOURCE_STATE_COPY_DEST, lru->lastUse = P;
    static int s_copyLogs = 0;
    if (++s_copyLogs <= 24)
        WLOG("occlusion: depth copy texture %ux%u %s%s", w, h, DepthFormatName(typeless), s_copyLogs == 24 ? " (no more of these logged)" : "");
    return lru;
}

bool AddRefSafe(ID3D12Resource* r)
{
    __try
    {
        r->AddRef();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool EnsureReadback(ID3D12Resource*& buf, uint64_t& have, uint64_t need)
{
    if (buf && have >= need)
        return true;
    Bury(buf);
    buf = nullptr;
    have = 0;
    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC rd = BufferDesc(need);
    if (FAILED(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&buf))))
    {
        buf = nullptr;
        return false;
    }
    have = need;
    return true;
}

// Records the W3 depth copy (+ statistics readback) for this Present into our occlusion list.
// Returns false (with g_occ.why) when there is nothing to copy this frame. The game's resource is
// AddRef'd and released after our fence: it can't vanish under the copy, and we never hold it longer.
bool RecordOccList(ID3D12GraphicsCommandList* list, uint64_t win, uint64_t P, bool readback, bool cal)
{
    std::string why;
    const int pick = PickThisFrame(win, why);
    if (pick < 0)
    {
        g_occ.why = why;
        return false;
    }
    DepthCand& c = g_cand[pick];
    ID3D12Resource* r = c.res.load();
    if (!r || !AddRefSafe(r))
    {
        g_occ.why = "candidate vanished";
        return false;
    }
    // The pointer may belong to a NEW resource since it was registered: trust only a fresh desc.
    D3D12_RESOURCE_DESC d{};
    DXGI_FORMAT tl, sv;
    if (!SafeGetDesc(r, d) || !CandShape(d) || !DepthFormats(d.Format, tl, sv))
    {
        r->Release();
        c.res.store(nullptr); // stale slot: forget it
        g_occ.why = "candidate changed shape";
        return false;
    }
    if (static_cast<uint32_t>(d.Width) != c.w.load() || d.Height != c.h.load() || static_cast<uint32_t>(d.Format) != c.fmt.load())
    {
        c.w = static_cast<uint32_t>(d.Width), c.h = d.Height, c.fmt = static_cast<uint32_t>(d.Format);
        c.vpW = 0, c.vpH = 0;
        c.stateKnown = false;
    }
    CopyTex* ct = GetCopyTex(static_cast<uint32_t>(d.Width), d.Height, tl, sv, P, d);
    if (!ct)
    {
        r->Release();
        return false;
    }
    const D3D12_RESOURCE_STATES state = g_config.occlusionDepthState
                                            ? static_cast<D3D12_RESOURCE_STATES>(g_config.occlusionDepthState)
                                            : c.stateKnown.load() ? static_cast<D3D12_RESOURCE_STATES>(c.lastState.load())
                                                                  : D3D12_RESOURCE_STATE_DEPTH_WRITE;
    // Depth PLANE only (subresource 0): planar D32S8/D24S8 keep their stencil plane untouched.
    D3D12_RESOURCE_BARRIER b[2];
    UINT nb = 0;
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
    {
        b[nb] = Transition(r, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        b[nb++].Transition.Subresource = 0;
    }
    if (ct->state != D3D12_RESOURCE_STATE_COPY_DEST)
        b[nb++] = Transition(ct->res, ct->state, D3D12_RESOURCE_STATE_COPY_DEST);
    if (nb)
        list->ResourceBarrier(nb, b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = ct->res;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    src.pResource = r;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    nb = 0;
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
    {
        b[nb] = Transition(r, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        b[nb++].Transition.Subresource = 0;
    }
    b[nb++] = Transition(ct->res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(nb, b);
    ct->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    if (g_r.graveCount < static_cast<int>(sizeof(g_r.grave) / sizeof(g_r.grave[0])))
        g_r.grave[g_r.graveCount++] = {r, UINT64_MAX}; // our reference; fence set after Signal (FixGraves)
    else
        r->Release(); // can't happen in practice (graveyard full): give the reference back now

    const uint32_t vw = c.vpW.load(), vh = c.vpH.load();
    g_occ.usedW = vw && vw <= d.Width ? vw : static_cast<uint32_t>(d.Width);
    g_occ.usedH = vh && vh <= d.Height ? vh : d.Height;
    g_occ.cur = ct;
    if (pick != g_occ.lastChosen)
    {
        ++g_occ.switches;
        if (g_occ.switches <= 20 || g_occ.switches % 500 == 0)
            WLOG("occlusion: W3 depth now #%d %llux%u %s (uses this frame %u, state 0x%X%s)%s", pick, static_cast<unsigned long long>(d.Width),
                 d.Height, DepthFormatName(d.Format), c.frameUses.load(), static_cast<unsigned>(state), c.stateKnown ? "" : " assumed",
                 g_occ.switches == 20 ? " (further switches logged every 500)" : "");
        g_occ.lastChosen = pick;
    }
    g_occ.chosen = pick;
    // Reverse Z from the game's own clear value of this buffer.
    float clear;
    const uint32_t bits = c.clearBits.load();
    std::memcpy(&clear, &bits, 4);
    if ((clear == 0.0f || clear == 1.0f) && g_occ.autoReverse != (clear == 0.0f ? 1 : 0))
    {
        g_occ.autoReverse = clear == 0.0f ? 1 : 0;
        g_occ.reverseWhy = "the game's depth clear value";
        WLOG("occlusion: W3 depth is %s (clear value %g)", clear == 0.0f ? "REVERSED Z (far = 0)" : "standard Z (far = 1)", clear);
    }
    // Statistics / calibration readback of the copy.
    if (readback && !g_occ.rbFence)
    {
        D3D12_RESOURCE_DESC cd = ct->res->GetDesc();
        UINT64 total = 0;
        ReadbackMeta m;
        g_r.device->GetCopyableFootprints(&cd, 0, 1, 0, &m.fp, nullptr, nullptr, &total);
        if (EnsureReadback(g_occ.rb, g_occ.rbBytes, total))
        {
            auto t = Transition(ct->res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &t);
            D3D12_TEXTURE_COPY_LOCATION rdst{}, rsrc{};
            rdst.pResource = g_occ.rb;
            rdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            rdst.PlacedFootprint = m.fp;
            rsrc.pResource = ct->res;
            rsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&rdst, 0, 0, 0, &rsrc, nullptr);
            t = Transition(ct->res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &t);
            m.usedW = g_occ.usedW, m.usedH = g_occ.usedH, m.cal = cal;
            g_occ.rbMeta = m;
            g_occ.rbFence = UINT64_MAX; // set after Signal
        }
    }
    return true;
}

// MC depth of the drawn frame for calibration (main list, after the draw: our own resource only).
void RecordMcDepthReadback(ID3D12GraphicsCommandList* list, const HistTex& mc)
{
    g_occ.mcValid = false;
    if (!mc.depth || !mc.hasDepth)
        return;
    D3D12_RESOURCE_DESC md = mc.depth->GetDesc();
    UINT64 total = 0;
    g_r.device->GetCopyableFootprints(&md, 0, 1, 0, &g_occ.mcFp, nullptr, nullptr, &total);
    if (!EnsureReadback(g_occ.mcRb, g_occ.mcRbBytes, total))
        return;
    auto b = Transition(mc.depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = g_occ.mcRb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = g_occ.mcFp;
    src.pResource = mc.depth;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = Transition(mc.depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &b);
    g_occ.mcA = mc.depthA, g_occ.mcB = mc.depthB, g_occ.mcClear = mc.depthClear;
    g_occ.mcFlip = (mc.flags & proto::kOverlayBottomUp) != 0;
    g_occ.mcValid = true;
}

float ReadW3Depth(const uint8_t* base, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, uint32_t x, uint32_t y)
{
    const uint8_t* p = base + fp.Offset + size_t(y) * fp.Footprint.RowPitch;
    switch (fp.Footprint.Format)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: {
        uint32_t v;
        std::memcpy(&v, p + size_t(x) * 4, 4);
        return (v & 0xFFFFFFu) / 16777215.0f;
    }
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_D16_UNORM: {
        uint16_t v;
        std::memcpy(&v, p + size_t(x) * 2, 2);
        return v / 65535.0f;
    }
    default: { // R32 family (also the depth plane of D32S8: footprint R32_TYPELESS)
        float v;
        std::memcpy(&v, p + size_t(x) * 4, 4);
        return v;
    }
    }
}

void FitCalibration()
{
    std::vector<CalPair>& v = g_occ.pairs;
    if (v.size() < 30)
    {
        g_occ.lastCal = "calibration: too few samples (" + std::to_string(v.size()) + ")";
        WLOG("occlusion: %s", g_occ.lastCal.c_str());
        return;
    }
    float A0, B0, farD;
    const char* what;
    OccModel(A0, B0, farD, &what);
    std::vector<float> xs;
    for (const auto& p : v)
        xs.push_back(p.x);
    std::sort(xs.begin(), xs.end());
    const float x10 = xs[xs.size() / 10], x90 = xs[xs.size() * 9 / 10];
    const bool twoParam = x10 > 0 && x90 / x10 > 1.25f;
    float A = A0, B = B0;
    std::vector<int> keep(v.size());
    for (size_t i = 0; i < v.size(); ++i)
        keep[i] = static_cast<int>(i);
    for (int iter = 0; iter < 3; ++iter)
    {
        if (twoParam)
        {
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (int i : keep)
                sx += v[i].x, sy += v[i].d, sxx += double(v[i].x) * v[i].x, sxy += double(v[i].x) * v[i].d;
            const double n = double(keep.size()), den = n * sxx - sx * sx;
            if (std::fabs(den) < 1e-12)
                break;
            B = static_cast<float>((n * sxy - sx * sy) / den);
            A = static_cast<float>((sy - B * sx) / n);
        }
        else
        {
            std::vector<float> bs;
            for (int i : keep)
                bs.push_back((v[i].d - A) / v[i].x);
            std::nth_element(bs.begin(), bs.begin() + bs.size() / 2, bs.end());
            B = bs[bs.size() / 2];
        }
        std::vector<std::pair<float, int>> res;
        for (int i : keep)
            res.push_back({std::fabs(v[i].d - (A + B * v[i].x)), i});
        std::sort(res.begin(), res.end());
        keep.clear();
        for (size_t i = 0; i < res.size() * 8 / 10; ++i)
            keep.push_back(res[i].second);
    }
    std::vector<float> rel;
    for (int i : keep)
    {
        const float den = v[i].d - A;
        if (den != 0 && B / den > 0)
            rel.push_back(std::fabs((B / den) * v[i].x - 1.0f));
    }
    std::sort(rel.begin(), rel.end());
    const float med = rel.empty() ? 1.0f : rel[rel.size() / 2];
    char line[400];
    std::snprintf(line, sizeof(line),
                  "calibration from %zu samples (%s fit, 1/z %.3f..%.3f): z = %.6g / (d - %.6g), median distance error %.1f%%. Keep it with "
                  "OcclusionDepthA=%.8g and OcclusionDepthB=%.8g in witchercraft_native.ini",
                  v.size(), twoParam ? "2-parameter" : "1-parameter (A kept)", x10, x90, B, A, med * 100.0f, A, B);
    g_occ.lastCal = line;
    if (med < 0.08f)
    {
        g_occ.calibrated = true;
        g_occ.calA = A, g_occ.calB = B;
        WLOG("occlusion: %s -> APPLIED", line);
    }
    else
    {
        WLOG("occlusion: %s -> NOT applied (error too large: aim at a flat, clear spot / an MC block on a W3 box)", line);
    }
}

void ProcessOccReadback()
{
    void* mem = nullptr;
    const D3D12_RANGE rr{0, static_cast<SIZE_T>(g_occ.rbBytes)};
    if (FAILED(g_occ.rb->Map(0, &rr, &mem)) || !mem)
        return;
    const ReadbackMeta& m = g_occ.rbMeta;
    const auto* base = static_cast<const uint8_t*>(mem);
    const uint32_t W = m.fp.Footprint.Width, H = m.fp.Footprint.Height;
    std::vector<float> s;
    s.reserve(8192);
    uint32_t zeros = 0, ones = 0, n = 0;
    for (uint32_t y = 4; y < m.usedH && y < H; y += 8)
        for (uint32_t x = 4; x < m.usedW && x < W; x += 8)
        {
            const float d = ReadW3Depth(base, m.fp, x, y);
            ++n;
            zeros += d == 0.0f;
            ones += d == 1.0f;
            s.push_back(d);
        }
    if (!s.empty())
    {
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        g_occ.statMedian = s[s.size() / 2];
        g_occ.statZero = float(zeros) / n, g_occ.statOne = float(ones) / n;
        if (std::string(g_occ.reverseWhy) != "the game's depth clear value" && g_config.occlusionReverseZ < 0)
        {
            const int r = (zeros > ones * 2 && zeros > n / 50) ? 1 : (ones > zeros * 2 && ones > n / 50) ? 0 : g_occ.statMedian < 0.5f ? 1 : 0;
            if (r != g_occ.autoReverse)
                WLOG("occlusion: W3 depth looks %s (median %.4f, exact 0: %.1f%%, exact 1: %.1f%%)", r ? "REVERSED (far = 0)" : "standard (far = 1)",
                     g_occ.statMedian, g_occ.statZero * 100, g_occ.statOne * 100);
            g_occ.autoReverse = r;
            g_occ.reverseWhy = "depth statistics";
        }
    }
    if (m.cal)
    {
        float A, B, farD;
        const char* what;
        OccModel(A, B, farD, &what);
        const uint32_t bw = g_occ.calBbW, bh = g_occ.calBbH;
        const void* mcMem = nullptr;
        bool mcMapped = false;
        if (g_occ.mcValid && g_occ.mcRb)
        {
            void* mm = nullptr;
            const D3D12_RANGE mr{0, static_cast<SIZE_T>(g_occ.mcRbBytes)};
            mcMapped = SUCCEEDED(g_occ.mcRb->Map(0, &mr, &mm)) && mm;
            mcMem = mm;
        }
        std::vector<CalPair> block, ground;
        const camsync::CamPose& cp = g_occ.calPose;
        const float pitchRad = cp.pitch * 0.0174532925f, tv = std::tan((cp.fov > 10 ? cp.fov : 70.0f) * 0.5f * 0.0174532925f);
        for (uint32_t y = bh / 2 - 96; y < bh / 2 + 96; y += 4)
            for (uint32_t x = bw / 2 - 96; x < bw / 2 + 96; x += 4)
            {
                const uint32_t wx = static_cast<uint32_t>(double(x) / bw * m.usedW), wy = static_cast<uint32_t>(double(y) / bh * m.usedH);
                if (wx >= W || wy >= H)
                    continue;
                const float d = ReadW3Depth(base, m.fp, wx, wy);
                if (std::fabs(d - farD) < 1e-7f)
                    continue;
                if (mcMapped)
                {
                    const uint32_t mw = g_occ.mcFp.Footprint.Width, mh = g_occ.mcFp.Footprint.Height;
                    uint32_t mx = static_cast<uint32_t>(double(x) / bw * mw), my = static_cast<uint32_t>(double(y) / bh * mh);
                    if (g_occ.mcFlip)
                        my = mh - 1 - my;
                    float md;
                    std::memcpy(&md, static_cast<const uint8_t*>(mcMem) + g_occ.mcFp.Offset + size_t(my) * g_occ.mcFp.Footprint.RowPitch + size_t(mx) * 4, 4);
                    if (md != g_occ.mcClear)
                    {
                        const float z = g_occ.mcB / (md - g_occ.mcA);
                        if (z > 1.0f && z < 200.0f) // > 1 m: never the hand / held item
                            block.push_back({1.0f / z, d});
                    }
                }
                const float yv = (1.0f - 2.0f * (y + 0.5f) / bh) * tv;
                const float den = std::sin(pitchRad) - yv * std::cos(pitchRad);
                if (den > 0.05f)
                {
                    const float z = g_config.occlusionEyeHeight / den;
                    if (z > 0.3f && z < 60.0f)
                        ground.push_back({1.0f / z, d});
                }
            }
        if (mcMapped)
        {
            const D3D12_RANGE none{0, 0};
            g_occ.mcRb->Unmap(0, &none);
        }
        const bool useBlock = block.size() >= 200;
        const std::vector<CalPair>& add = useBlock ? block : ground;
        if (!useBlock && cp.pitch < 20.0f)
        {
            g_occ.lastCal = "calibration: no Minecraft block at the crosshair and not looking down at the ground (pitch " +
                            std::to_string(static_cast<int>(cp.pitch)) + " deg < 20): nothing measured";
            WLOG("occlusion: %s", g_occ.lastCal.c_str());
        }
        else
        {
            WLOG("occlusion: calibration sample: %zu pairs (%s mode, pitch %.1f deg)", add.size(),
                 useBlock ? "Minecraft block on a W3 surface" : "flat ground", cp.pitch);
            g_occ.pairs.insert(g_occ.pairs.end(), add.begin(), add.end());
            if (g_occ.pairs.size() > 20000)
                g_occ.pairs.erase(g_occ.pairs.begin(), g_occ.pairs.begin() + (g_occ.pairs.size() - 20000));
            FitCalibration();
        }
    }
    const D3D12_RANGE none{0, 0};
    g_occ.rb->Unmap(0, &none);
}

std::string OccStatusLine()
{
    float A, B, farD;
    const char* what;
    OccModel(A, B, farD, &what);
    char buf[700];
    const int ch = g_occ.chosen;
    std::string depth = g_occ.failed ? "FAILED: " + g_occ.failWhy
                        : ch >= 0 ? "#" + std::to_string(ch) + " " + std::to_string(g_cand[ch].w.load()) + "x" + std::to_string(g_cand[ch].h.load()) +
                                        " " + DepthFormatName(static_cast<DXGI_FORMAT>(g_cand[ch].fmt.load())) + " used " +
                                        std::to_string(g_occ.usedW) + "x" + std::to_string(g_occ.usedH)
                                  : "none (" + g_occ.why + ")";
    std::snprintf(buf, sizeof(buf),
                  "occlusion %s | W3 depth %s | switches %llu, frames with/without depth %llu/%llu | model z = %.5g / (d - %.5g) (%s, far d=%g, %s Z via "
                  "%s) | median d %.4f | MC depth frames %llu bad %llu | draws occluded %llu plain %llu%s%s | bias %.2f m soft %.2f m keep %.2f m "
                  "debug %d | %s",
                  g_config.overlayOcclusion ? "ON" : "OFF", depth.c_str(), static_cast<unsigned long long>(g_occ.switches),
                  static_cast<unsigned long long>(g_occ.framesWith), static_cast<unsigned long long>(g_occ.framesWithout), B, A, what, farD,
                  farD == 0.0f ? "reversed" : "standard", g_occ.reverseWhy, g_occ.statMedian,
                  static_cast<unsigned long long>(g_depthFrames.load()), static_cast<unsigned long long>(g_depthBad.load()),
                  static_cast<unsigned long long>(g_occ.drawsOccluded), static_cast<unsigned long long>(g_occ.drawsPlain),
                  g_occ.lastPlainWhy.empty() ? "" : " (plain: ", g_occ.lastPlainWhy.empty() ? "" : (g_occ.lastPlainWhy + ")").c_str(),
                  g_config.occlusionBias, g_config.occlusionSoft, g_config.occlusionKeepNear, g_config.occlusionDebug, g_occ.lastCal.c_str());
    return buf;
}

// Ini re-read (file time), hotkeys; once per Present.
void OccPoll(uint64_t now)
{
    static uint64_t s_lastIni = 0;
    static FILETIME s_iniTime{};
    if (now - s_lastIni > 2000 && g_config.iniPath[0])
    {
        s_lastIni = now;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(g_config.iniPath, GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &s_iniTime) != 0)
        {
            const bool first = s_iniTime.dwLowDateTime == 0 && s_iniTime.dwHighDateTime == 0;
            s_iniTime = fa.ftLastWriteTime;
            if (!first && ReloadOcclusionConfig())
                WLOG("occlusion: ini changed -> OverlayOcclusion=%d bias %.3f soft %.3f keep %.2f pick %d reverse %d near %.3f far %.1f model %s debug %d",
                     g_config.overlayOcclusion, g_config.occlusionBias, g_config.occlusionSoft, g_config.occlusionKeepNear,
                     g_config.occlusionDepthPick, g_config.occlusionReverseZ, g_config.occlusionNear, g_config.occlusionFar,
                     g_config.occlusionModelSet ? "set" : "auto", g_config.occlusionDebug);
        }
    }
    if (!g_config.occlusionHotkeys)
        return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId() || !(GetAsyncKeyState(VK_CONTROL) & 0x8000) || !(GetAsyncKeyState(VK_SHIFT) & 0x8000))
        return;
    static bool s_down[4] = {};
    const int keys[4] = {'O', 'C', 'D', 'N'};
    for (int i = 0; i < 4; ++i)
    {
        const bool down = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (down && !s_down[i])
        {
            if (i == 0)
            {
                g_config.overlayOcclusion = !g_config.overlayOcclusion;
                if (g_config.overlayOcclusion)
                    g_occ.reqRetry = 1;
            }
            else if (i == 1)
                g_occ.reqCal = 1;
            else if (i == 2)
                g_config.occlusionDebug = (g_config.occlusionDebug + 1) % 3;
            else
                g_occ.reqNext = 1;
            WLOG("occlusion: hotkey Ctrl+Shift+%c -> occlusion %d debug %d", keys[i], g_config.overlayOcclusion, g_config.occlusionDebug);
        }
        s_down[i] = down;
    }
}

// SRVs of this frame context's table: t0 colour, t1 MC depth (history slot `pick`), t2 W3 depth copy.
void WriteFrameSrvs(uint32_t ci, int pick, const CopyTex* w3)
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_r.srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(ci) * kSrvPerHist * g_r.srvInc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    g_r.device->CreateShaderResourceView(g_r.hist[pick].res, &sd, cpu);
    cpu.ptr += g_r.srvInc;
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    g_r.device->CreateShaderResourceView(g_r.hist[pick].depth, &sd, cpu);
    cpu.ptr += g_r.srvInc;
    sd.Format = w3 ? w3->srv : DXGI_FORMAT_R32_FLOAT;
    sd.Texture2D.PlaneSlice = 0; // the depth plane of planar D32S8 / D24S8
    g_r.device->CreateShaderResourceView(w3 ? w3->res : nullptr, &sd, cpu); // null = reads 0
}

void InitOcclusionLists()
{
    for (int i = 0; i < kFrames; ++i)
    {
        // Occlusion work has its own lists: a failure there only switches occlusion off.
        if (FAILED(g_r.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_occ.alloc[i]))) ||
            FAILED(g_r.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_occ.alloc[i], nullptr, IID_PPV_ARGS(&g_occ.list[i]))) ||
            FAILED(g_occ.list[i]->Close()))
        {
            g_occ.list[i] = nullptr;
            OccFail("could not create its command lists");
        }
    }
}

// Sets the fence of the game-resource references taken while recording (graves with fence MAX).
void FixGraves(uint64_t v)
{
    for (int i = 0; i < g_r.graveCount; ++i)
        if (g_r.grave[i].fence == UINT64_MAX)
            g_r.grave[i].fence = v;
}

// 0 ok (copy recorded and submitted), 1 nothing to copy, 2 failed (why in g_occ.why).
int RunOccInner(uint32_t ci, uint64_t win, uint64_t P, bool cal, uint32_t bbW, uint32_t bbH, long& hr)
{
    ID3D12CommandAllocator* a = g_occ.alloc[ci];
    ID3D12GraphicsCommandList* l = g_occ.list[ci];
    hr = a->Reset();
    if (SUCCEEDED(hr))
        hr = l->Reset(a, nullptr);
    if (FAILED(hr))
    {
        g_occ.why = "occlusion command list reset failed";
        return 2;
    }
    const bool readback = cal || P % 60 == 0;
    const bool recorded = RecordOccList(l, win, P, readback, cal);
    // Test hook (overlay_test, env WC_OCC_FAULT=<present>): make this list fail to close like the
    // 0x80070057 seen in game, to prove that only occlusion switches off.
    static const uint64_t s_fault = [] {
        char v[32]{};
        return GetEnvironmentVariableA("WC_OCC_FAULT", v, sizeof(v)) ? _strtoui64(v, nullptr, 10) : 0ull;
    }();
    if (s_fault && P == s_fault)
        l->Close(); // the real Close below now fails (E_FAIL: already closed)
    hr = l->Close();
    if (FAILED(hr))
    {
        FixGraves(g_r.fenceValue); // never executed: give the references back at once
        g_occ.rbFence = 0;
        g_occ.why = "occlusion command list close failed";
        return 2;
    }
    if (!recorded)
        return 1;
    ID3D12CommandList* lists[] = {l};
    g_oECL ? g_oECL(g_r.queue, 1, lists) : g_r.queue->ExecuteCommandLists(1, lists);
    const uint64_t v = ++g_r.fenceValue;
    hr = g_r.queue->Signal(g_r.fence, v);
    if (FAILED(hr))
    {
        g_occ.why = "occlusion Signal failed";
        return 2;
    }
    g_occ.fence[ci] = v;
    FixGraves(v);
    if (g_occ.rbFence == UINT64_MAX && !cal)
        g_occ.rbFence = v;
    (void)bbW, (void)bbH;
    return 0;
}

int SafeRunOccInner(uint32_t ci, uint64_t win, uint64_t P, bool cal, uint32_t bbW, uint32_t bbH, long& hr)
{
    __try
    {
        return RunOccInner(ci, win, P, cal, bbW, bbH, hr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        hr = static_cast<long>(GetExceptionCode());
        return 3;
    }
}

// The W3 depth copy for this Present, in its own command list (executed before the compositor's).
void RunOcc(uint32_t ci, uint64_t win, uint64_t P, uint64_t completed, bool cal, uint32_t bbW, uint32_t bbH, const HistTex& ht)
{
    (void)ht;
    if (!g_occ.list[ci] || completed < g_occ.fence[ci])
    {
        g_occ.why = "occlusion list busy";
        return;
    }
    long hr = 0;
    const int r = SafeRunOccInner(ci, win, P, cal, bbW, bbH, hr);
    if (r == 0)
        g_occ.haveCopy = g_occ.cur != nullptr;
    else if (r == 2)
        OccFail(g_occ.why.c_str(), hr);
    else if (r == 3)
    {
        FixGraves(g_r.fenceValue);
        g_occ.rbFence = 0;
        OccFail("SEH exception while recording the depth copy", hr);
    }
}

// Returns true if a capture copy was recorded.
bool RecordDraw(ID3D12GraphicsCommandList* list, IDXGISwapChain* sc, const DXGI_SWAP_CHAIN_DESC& desc, int mode,
                const LinkView& lv, uint32_t ctxIndex, int texSlot, bool occlude, bool& captured)
{
    captured = false;
    IDXGISwapChain3* sc3 = nullptr;
    HRESULT hr = sc->QueryInterface(IID_PPV_ARGS(&sc3));
    if (FAILED(hr) || !sc3)
    {
        Disable("swapchain has no IDXGISwapChain3", hr);
        return false;
    }
    const UINT idx = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    ID3D12Resource* bb = nullptr;
    hr = sc->GetBuffer(idx, IID_PPV_ARGS(&bb));
    if (FAILED(hr) || !bb)
    {
        Disable("GetBuffer(ID3D12Resource) failed", hr);
        return false;
    }
    const DXGI_FORMAT fmt = desc.BufferDesc.Format;
    const float bbW = float(desc.BufferDesc.Width), bbH = float(desc.BufferDesc.Height);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_r.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(ctxIndex) * g_r.rtvInc;
    D3D12_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = fmt;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_r.device->CreateRenderTargetView(bb, &rd, rtv);

    Consts c{};
    c.texSize[0] = float(g_r.texW);
    c.texSize[1] = float(g_r.texH);
    c.bbSize[0] = bbW;
    c.bbSize[1] = bbH;
    c.flipY = (g_r.texFlags & 1) ? 1.0f : 0.0f;
    c.mode = static_cast<uint32_t>(mode);
    c.paperWhite = float(g_paperWhite);
    c.bilinear = (g_r.texFlags & proto::kOverlayWorldLayer) ? 1.0f : 0.0f;
    c.edgeAA = g_config.overlayEdgeAA && (g_r.texFlags & proto::kOverlayWorldLayer) && g_r.hist[texSlot].hasDepth ? 1.0f : 0.0f;
    static const bool legacyEdgeAA = std::getenv("WC_EDGEAA_LEGACY") && std::atoi(std::getenv("WC_EDGEAA_LEGACY")) != 0; // M6b comparison
    if (c.edgeAA > 0.5f && legacyEdgeAA)
        c.edgeAA = 2.0f;
    if (c.edgeAA > 0.5f)
    {
        const HistTex& ht = g_r.hist[texSlot]; // McZ() needs the MC depth model even without occlusion
        c.mcA = ht.depthA, c.mcB = ht.depthB, c.mcClear = ht.depthClear;
    }
    const bool w3Bound = (occlude || g_config.occlusionDebug == 2) && g_occ.haveCopy && g_occ.cur;
    WriteFrameSrvs(ctxIndex, texSlot, w3Bound ? g_occ.cur : nullptr);
    if (w3Bound)
    {
        const HistTex& ht = g_r.hist[texSlot];
        float A, B, farD;
        const char* what;
        OccModel(A, B, farD, &what);
        c.occl = occlude ? 1.0f : 0.0f;
        c.mcA = ht.depthA, c.mcB = ht.depthB, c.mcClear = ht.depthClear;
        c.w3A = A, c.w3B = B, c.w3Far = farD;
        c.bias = g_config.occlusionBias;
        c.soft = g_config.occlusionSoft;
        c.dbg = float(g_config.occlusionDebug);
        c.w3Size[0] = float(g_occ.cur->w), c.w3Size[1] = float(g_occ.cur->h);
        c.keepNear = g_config.occlusionKeepNear;
        c.w3Used[0] = float(g_occ.usedW), c.w3Used[1] = float(g_occ.usedH);
    }
    // Crosshair + attack indicator around the centre (SkyCraft's rect), inverted against the
    // game's picture. Only meaningful in SDR; in HDR it is simply drawn like the rest.
    // Not for world-layer frames: there the pixels around the crosshair are Minecraft's blocks
    // (MC already inverted its crosshair against them); zeroing/inverting them would punch a hole.
    const bool invert = mode == 0 && lv.cameraMode == 0 && !lv.screenOpen && lv.guiScale > 0 &&
                        !(g_r.texFlags & proto::kOverlayWorldLayer);
    if (invert)
    {
        const float gx = float(lv.guiScale) * bbW / float(g_r.texW);
        const float gy = float(lv.guiScale) * bbH / float(g_r.texH);
        const float cx = bbW * 0.5f, cy = bbH * 0.5f;
        c.invertRect[0] = cx - 12.0f * gx;
        c.invertRect[1] = cy - 12.0f * gy;
        c.invertRect[2] = cx + 12.0f * gx;
        c.invertRect[3] = cy + 28.0f * gy;
    }

    auto b = Transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &b);
    list->SetGraphicsRootSignature(g_r.rootSig);
    list->SetDescriptorHeaps(1, &g_r.srvHeap);
    D3D12_GPU_DESCRIPTOR_HANDLE srv = g_r.srvHeap->GetGPUDescriptorHandleForHeapStart();
    srv.ptr += UINT64(ctxIndex) * kSrvPerHist * g_r.srvInc;
    list->SetGraphicsRoot32BitConstants(0, sizeof(Consts) / 4, &c, 0);
    list->SetGraphicsRootDescriptorTable(1, srv);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0, 0, bbW, bbH, 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(desc.BufferDesc.Width), LONG(desc.BufferDesc.Height)};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->SetPipelineState(g_r.psoMain);
    list->DrawInstanced(3, 1, 0, 0);
    if (invert)
    {
        list->SetPipelineState(g_r.psoInvert);
        list->DrawInstanced(3, 1, 0, 0);
    }

    // Debug/test capture of the composited back buffer.
    if (g_captureRequested.load() && !g_r.captureFence)
    {
        const D3D12_RESOURCE_DESC bd = bb->GetDesc();
        UINT64 total = 0;
        g_r.device->GetCopyableFootprints(&bd, 0, 1, 0, &g_r.captureFp, nullptr, nullptr, &total);
        if (!g_r.readback || g_r.readbackBytes < total)
        {
            Bury(g_r.readback);
            g_r.readback = nullptr;
            const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_READBACK);
            const D3D12_RESOURCE_DESC rdesc = BufferDesc(total);
            if (FAILED(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rdesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                           nullptr, IID_PPV_ARGS(&g_r.readback))))
                g_r.readback = nullptr;
            g_r.readbackBytes = g_r.readback ? total : 0;
        }
        if (g_r.readback)
        {
            b = Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = g_r.readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = g_r.captureFp;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = bb;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            b = Transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            list->ResourceBarrier(1, &b);
            g_r.captureFmt = fmt;
            g_r.captureMode = mode;
            g_r.capturePaperWhite = float(g_paperWhite);
            captured = true;
        }
    }
    if (!captured)
    {
        b = Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &b);
    }
    bb->Release(); // the swapchain owns it; we hold no back-buffer references between frames
    return true;
}

// ---- capture -> BMP ---------------------------------------------------------------------------
float HalfToFloat(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0)
        v = std::ldexp(float(m), -24);
    else if (e == 31)
        v = m ? 0.0f : 65504.0f;
    else
        v = std::ldexp(float(m | 1024), int(e) - 25);
    return s ? -v : v;
}

uint8_t LinearToSrgb8(float l)
{
    l = l < 0 ? 0 : l > 1 ? 1 : l;
    const float s = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(s * 255.0f + 0.5f);
}

float PqToNits(float e)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float p = std::pow(e < 0 ? 0 : e, 1.0f / m2);
    const float num = p - c1 > 0 ? p - c1 : 0;
    return 10000.0f * std::pow(num / (c2 - c3 * p), 1.0f / m1);
}

void WriteCapture()
{
    const auto& fp = g_r.captureFp.Footprint;
    void* p = nullptr;
    const D3D12_RANGE range{0, SIZE_T(g_r.readbackBytes)};
    if (FAILED(g_r.readback->Map(0, &range, &p)) || !p)
    {
        WLOG("overlay: capture map failed");
        return;
    }
    const uint32_t w = fp.Width, h = fp.Height;
    const size_t bytes = size_t(w) * h * 4;
    uint8_t* out = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (out)
    {
        const float pw = g_r.capturePaperWhite;
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t* row = static_cast<const uint8_t*>(p) + g_r.captureFp.Offset + size_t(y) * fp.RowPitch;
            uint8_t* o = out + size_t(y) * w * 4;
            for (uint32_t x = 0; x < w; ++x, o += 4)
            {
                uint8_t r = 0, g = 0, bl = 0;
                switch (g_r.captureFmt)
                {
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                    r = row[x * 4], g = row[x * 4 + 1], bl = row[x * 4 + 2];
                    break;
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                    bl = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
                    break;
                case DXGI_FORMAT_R10G10B10A2_UNORM: {
                    uint32_t v;
                    std::memcpy(&v, row + x * 4, 4);
                    const float c[3] = {(v & 1023) / 1023.0f, ((v >> 10) & 1023) / 1023.0f, ((v >> 20) & 1023) / 1023.0f};
                    if (g_r.captureMode == 2)
                    {
                        // PQ BT.2020 -> linear BT.709 relative to paper white -> sRGB
                        const float n[3] = {PqToNits(c[0]) / pw, PqToNits(c[1]) / pw, PqToNits(c[2]) / pw};
                        r = LinearToSrgb8(1.6604910f * n[0] - 0.5876411f * n[1] - 0.0728499f * n[2]);
                        g = LinearToSrgb8(-0.1245505f * n[0] + 1.1328999f * n[1] - 0.0083494f * n[2]);
                        bl = LinearToSrgb8(-0.0181508f * n[0] - 0.1005789f * n[1] + 1.1187297f * n[2]);
                    }
                    else
                    {
                        r = uint8_t(c[0] * 255 + 0.5f), g = uint8_t(c[1] * 255 + 0.5f), bl = uint8_t(c[2] * 255 + 0.5f);
                    }
                    break;
                }
                case DXGI_FORMAT_R16G16B16A16_FLOAT: {
                    uint16_t hv[3];
                    std::memcpy(hv, row + x * 8, 6);
                    const float k = 80.0f / pw;
                    r = LinearToSrgb8(HalfToFloat(hv[0]) * k);
                    g = LinearToSrgb8(HalfToFloat(hv[1]) * k);
                    bl = LinearToSrgb8(HalfToFloat(hv[2]) * k);
                    break;
                }
                default:
                    break;
                }
                o[0] = bl, o[1] = g, o[2] = r, o[3] = 255;
            }
        }
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof(ih);
        ih.biWidth = LONG(w);
        ih.biHeight = -LONG(h); // top-down
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        ih.biCompression = BI_RGB;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = DWORD(fh.bfOffBits + bytes);
        HANDLE f = CreateFileW(g_capturePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            DWORD wr = 0;
            WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
            WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
            WriteFile(f, out, DWORD(bytes), &wr, nullptr);
            CloseHandle(f);
            WLOG("overlay: captured %ux%u %s back buffer to %ls", w, h, FormatName(g_r.captureFmt), g_capturePath);
        }
        else
        {
            WLOG("overlay: capture: cannot write %ls (%lu)", g_capturePath, GetLastError());
        }
        VirtualFree(out, 0, MEM_RELEASE);
    }
    const D3D12_RANGE none{0, 0};
    g_r.readback->Unmap(0, &none);
}

// ---- image probe ---------------------------------------------------------------
bool ProbeFormatOk(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R10G10B10A2_UNORM ||
           f == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

int FreeProbeSlot(uint64_t present)
{
    const int s = static_cast<int>(present % kProbeSlots);
    return g_r.probe[s].fence == 0 ? s : -1;
}

// Copies a band of rows around the centre of the game's back buffer (before we draw on it).
bool RecordProbe(ID3D12GraphicsCommandList* list, IDXGISwapChain* sc, const DXGI_SWAP_CHAIN_DESC& desc, int slot)
{
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3)
        return false;
    const UINT idx = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(idx, IID_PPV_ARGS(&bb))) || !bb)
        return false;
    ProbeSlot& p = g_r.probe[slot];
    const DXGI_FORMAT fmt = desc.BufferDesc.Format;
    const uint32_t w = desc.BufferDesc.Width, h = desc.BufferDesc.Height;
    const uint32_t rows = h >= kProbeRows * 2 ? kProbeRows : h / 2;
    const uint32_t bpp = fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    const uint32_t pitch = Align(w * bpp, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t need = uint64_t(pitch) * rows;
    if (!p.buf || p.bytes < need)
    {
        Bury(p.buf);
        p.buf = nullptr;
        p.bytes = 0;
        const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_READBACK);
        const D3D12_RESOURCE_DESC rd = BufferDesc(need);
        if (FAILED(g_r.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&p.buf))))
        {
            p.buf = nullptr;
            bb->Release();
            return false;
        }
        p.bytes = need;
    }
    p.fmt = fmt;
    p.width = w;
    p.rows = rows;
    p.pitch = pitch;
    auto b = Transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = p.buf;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {fmt, w, rows, 1, pitch};
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = bb;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const uint32_t y0 = h / 2 - rows / 2;
    const D3D12_BOX box{0, y0, 0, w, y0 + rows, 1};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    b = Transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    list->ResourceBarrier(1, &b);
    bb->Release();
    return true;
}

// Column profile (green channel, kProbeBin-pixel bins) of a probe band, as a gradient (edges).
bool ProbeProfile(const ProbeSlot& p, std::vector<float>& out)
{
    void* mem = nullptr;
    const D3D12_RANGE rr{0, static_cast<SIZE_T>(p.bytes)};
    if (FAILED(p.buf->Map(0, &rr, &mem)) || !mem)
        return false;
    const uint32_t nb = p.width / kProbeBin;
    std::vector<float> col(nb, 0.0f);
    const auto* base = static_cast<const uint8_t*>(mem);
    for (uint32_t r = 0; r < p.rows; r += 2)
    {
        const uint8_t* row = base + size_t(r) * p.pitch;
        for (uint32_t x = 0; x < nb * kProbeBin; ++x)
        {
            float g;
            if (p.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT)
            {
                uint16_t hv;
                std::memcpy(&hv, row + size_t(x) * 8 + 2, 2);
                g = HalfToFloat(hv) * 255.0f;
            }
            else if (p.fmt == DXGI_FORMAT_R10G10B10A2_UNORM)
            {
                uint32_t v;
                std::memcpy(&v, row + size_t(x) * 4, 4);
                g = ((v >> 10) & 1023u) * 0.25f;
            }
            else
            {
                g = row[size_t(x) * 4 + 1];
            }
            col[x / kProbeBin] += g;
        }
    }
    const D3D12_RANGE none{0, 0};
    p.buf->Unmap(0, &none);
    out.assign(nb, 0.0f);
    for (uint32_t i = 1; i + 1 < nb; ++i)
        out[i] = col[i + 1] - col[i - 1];
    return nb >= 32;
}

// Horizontal shift (pixels, + = moved right) of `cur` against `prev`: normalised cross-correlation.
bool ProbeShift(const std::vector<float>& cur, const std::vector<float>& prev, float& shiftPx)
{
    const int nb = static_cast<int>(cur.size());
    if (nb < 32 || static_cast<int>(prev.size()) != nb)
        return false;
    const int R = nb / 4 < 64 ? nb / 4 : 64;
    float c[129];
    int best = 0;
    for (int s = -R; s <= R; ++s)
    {
        double num = 0, a = 0, b = 0;
        const int lo = s > 0 ? 1 + s : 1, hi = s < 0 ? nb - 1 + s : nb - 1;
        for (int i = lo; i < hi; ++i)
        {
            const double x = cur[i], y = prev[i - s];
            num += x * y, a += x * x, b += y * y;
        }
        c[s + R] = a > 1e-3 && b > 1e-3 ? static_cast<float>(num / std::sqrt(a * b)) : 0.0f;
        if (c[s + R] > c[best])
            best = s + R;
    }
    if (c[best] < 0.6f)
        return false;
    float off = 0;
    if (best > 0 && best < 2 * R)
    {
        const float l = c[best - 1], m = c[best], r = c[best + 1];
        const float den = l - 2 * m + r;
        if (den < -1e-6f)
            off = 0.5f * (l - r) / den;
    }
    shiftPx = (static_cast<float>(best - R) + off) * kProbeBin;
    return true;
}

// Reads back finished probe bands in present order and reports the shifts.
void ProcessProbes(uint64_t completed)
{
    for (;;)
    {
        int next = -1;
        for (int i = 0; i < kProbeSlots; ++i)
            if (g_r.probe[i].fence && g_r.probe[i].fence <= completed && (next < 0 || g_r.probe[i].present < g_r.probe[next].present))
                next = i;
        if (next < 0)
            return;
        ProbeSlot& p = g_r.probe[next];
        float shift = 0;
        bool valid = false;
        if (ProbeProfile(p, g_r.probeCur))
        {
            valid = g_r.probePrevPresent + 1 == p.present && ProbeShift(g_r.probeCur, g_r.probePrev, shift);
            g_r.probePrev.swap(g_r.probeCur);
            g_r.probePrevPresent = p.present;
        }
        camsync::OnProbeShift(p.present, valid, shift);
        p.fence = 0;
    }
}

// The Witcher's own graphics settings that matter for sync (re-read every 30 s; cheap).
void ReadSyncSettings()
{
    using PFN_SHGetKnownFolderPath = HRESULT(WINAPI*)(REFGUID, DWORD, HANDLE, PWSTR*);
    using PFN_CoTaskMemFree = void(WINAPI*)(LPVOID);
    static auto getPath = reinterpret_cast<PFN_SHGetKnownFolderPath>(SysProc(L"shell32.dll", "SHGetKnownFolderPath"));
    static auto memFree = reinterpret_cast<PFN_CoTaskMemFree>(SysProc(L"ole32.dll", "CoTaskMemFree"));
    static const GUID kDocuments = {0xFDD39AD0, 0x238F, 0x46AF, {0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7}};
    static camsync::GameSettings s_last;
    PWSTR docs = nullptr;
    if (!getPath || FAILED(getPath(kDocuments, 0, nullptr, &docs)) || !docs)
        return;
    wchar_t path[MAX_PATH * 2];
    swprintf_s(path, L"%s\\The Witcher 3\\dx12user.settings", docs);
    if (memFree)
        memFree(docs);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    static char buf[256 * 1024];
    DWORD n = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(f);
    buf[n] = 0;
    auto num = [](const char* key) {
        const char* p = strstr(buf, key);
        return p ? atoi(p + strlen(key)) : 0;
    };
    auto yes = [](const char* key) {
        const char* p = strstr(buf, key);
        return p && _strnicmp(p + strlen(key), "true", 4) == 0;
    };
    camsync::GameSettings s;
    s.known = true;
    s.frameGen = num("\nDLSSGMode=") != 0 || num("\nFSRFramegen=") != 0 || yes("\nXessFrameGeneration=");
    s.motionBlur = yes("\nAllowMotionBlur=");
    s.blur = yes("\nAllowBlur=");
    s.limitFps = num("\nLimitFPS=");
    s.vsync = yes("\nVSync=");
    if (!s_last.known || s.frameGen != s_last.frameGen || s.motionBlur != s_last.motionBlur || s.blur != s_last.blur ||
        s.limitFps != s_last.limitFps || s.vsync != s_last.vsync)
        camsync::SetGameSettings(s);
    s_last = s;
}

// What the last Present composited (offline test / diagnostics).
SRWLOCK g_compLock = SRWLOCK_INIT;
wc::overlay::Composited g_comp;

wc::overlay::Pose ToPose(const camsync::CamPose& c)
{
    wc::overlay::Pose p;
    p.frameId = c.frameId;
    p.eye[0] = c.eye[0], p.eye[1] = c.eye[1], p.eye[2] = c.eye[2];
    p.yaw = c.yaw, p.pitch = c.pitch, p.fovDeg = c.fov, p.rollDeg = c.roll;
    return p;
}

// ---- per-Present work -------------------------------------------------------------------------
bool IsKnownDirectQueue(uintptr_t v)
{
    const QueueRec* r = FindQueue(reinterpret_cast<const void*>(v));
    return r && r->type == D3D12_COMMAND_LIST_TYPE_DIRECT;
}

// Looks for a known DIRECT queue pointer inside the swapchain object (it keeps its queue).
ID3D12CommandQueue* ScanSwapChainForQueue(const void* sc, int* offset, int* hits)
{
    ID3D12CommandQueue* found = nullptr;
    *offset = -1;
    *hits = 0;
    __try
    {
        const uintptr_t* p = static_cast<const uintptr_t*>(sc);
        for (int i = 0; i < 0x1000 / 8; ++i)
        {
            const uintptr_t v = p[i];
            if (v && IsKnownDirectQueue(v))
            {
                if (!found)
                {
                    found = reinterpret_cast<ID3D12CommandQueue*>(v);
                    *offset = i * 8;
                }
                if (reinterpret_cast<uintptr_t>(found) != v)
                    return nullptr; // two different queues: ambiguous
                ++*hits;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return found;
}

ID3D12CommandQueue* ResolveQueue(IDXGISwapChain* sc, ScRec& rec)
{
    static ID3D12CommandQueue* s_candidate = nullptr;
    static int s_candidateFrames = 0;
    if (rec.queue)
        return rec.queue;

    // a) IDXGISwapChain::GetDevice(ID3D12CommandQueue): a D3D12 swapchain's "device" is its queue.
    ID3D12CommandQueue* viaGetDevice = nullptr;
    if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&viaGetDevice))) && viaGetDevice)
    {
        const D3D12_COMMAND_LIST_TYPE t = viaGetDevice->GetDesc().Type;
        viaGetDevice->Release();
        if (t != D3D12_COMMAND_LIST_TYPE_DIRECT)
            viaGetDevice = nullptr;
    }
    // b) the queue pointer stored in the swapchain object (must be one we saw submitting).
    int offset = -1, hits = 0;
    ID3D12CommandQueue* viaScan = ScanSwapChainForQueue(sc, &offset, &hits);
    // c) timing: the last DIRECT queue this (presenting) thread submitted to.
    ID3D12CommandQueue* viaTiming = t_lastDirect;

    static int s_logs = 0;
    if (s_logs < 8)
    {
        ++s_logs;
        WLOG("overlay: queue for swapchain %p: GetDevice=%p scan=%p(+0x%X, %d hits) lastDirectOnThread=%p (known queues: %d)",
             static_cast<void*>(sc), static_cast<void*>(viaGetDevice), static_cast<void*>(viaScan), offset, hits,
             static_cast<void*>(viaTiming), g_queueCount.load());
    }
    ID3D12CommandQueue* q = nullptr;
    const char* src = nullptr;
    if (viaGetDevice)
        q = viaGetDevice, src = "IDXGISwapChain::GetDevice";
    else if (viaScan)
        q = viaScan, src = "swapchain memory scan";
    else if (viaTiming)
    {
        // Weakest evidence: require the same answer for 60 presents in a row.
        if (viaTiming == s_candidate)
            ++s_candidateFrames;
        else
            s_candidate = viaTiming, s_candidateFrames = 0;
        if (s_candidateFrames >= 60)
            q = viaTiming, src = "last DIRECT ExecuteCommandLists before Present";
    }
    if (!q)
        return nullptr;
    WLOG("overlay: swapchain %p -> present queue %p (via %s)", static_cast<void*>(sc), static_cast<void*>(q), src);
    ScSetQueue(sc, q, src);
    rec.queue = q;
    strncpy_s(rec.queueSource, src, _TRUNCATE);
    return q;
}

void RenderFrame(IDXGISwapChain* sc)
{
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc)))
        return;
    const uint64_t now = GetTickCount64();
    if (g_primary != sc)
    {
        // Several swapchains may present (FG proxies, tools). Stick to one; switch only when the
        // current one has been silent for a second.
        if (g_primary && now - g_primaryLast < 1000)
            return;
        WLOG("overlay: compositing into swapchain %p (%ux%u %s, %u buffers, swap effect %d, hwnd %p)", static_cast<void*>(sc),
             desc.BufferDesc.Width, desc.BufferDesc.Height, FormatName(desc.BufferDesc.Format), desc.BufferCount,
             desc.SwapEffect, static_cast<void*>(desc.OutputWindow));
        g_primary = sc;
        g_primaryPresents = 0;
        g_timingMatch = g_timingOther = g_timingNone = 0;
    }
    g_primaryLast = now;
    ++g_primaryPresents;
    // Which game frame (camera) this image was rendered with.
    const camsync::PresentInfo pi = camsync::OnPresent();
    // Depth uses the hooks counted since the previous Present carry stamp occWin.
    const uint64_t occWin = g_presentNo.fetch_add(1, std::memory_order_relaxed);
    const uint64_t occP = occWin + 1;
    OccPoll(now);

    const uint32_t bbW = desc.BufferDesc.Width, bbH = desc.BufferDesc.Height;
    g_bbW.store(bbW, std::memory_order_relaxed);
    g_bbH.store(bbH, std::memory_order_relaxed);
    if (bbW != g_vpW || bbH != g_vpH)
    {
        g_vpW = bbW;
        g_vpH = bbH;
        wc::host::SetViewport(bbW, bbH);
        g_stats.bbW = bbW;
        g_stats.bbH = bbH;
        WLOG("overlay: back buffer %ux%u %s -> host::SetViewport(%u, %u) (Minecraft renders its HUD at this size)", bbW, bbH,
             FormatName(desc.BufferDesc.Format), bbW, bbH);
    }

    ScRec rec = ScGet(sc);
    ID3D12CommandQueue* queue = ResolveQueue(sc, rec);
    if (!queue)
        return;
    if (!g_r.device)
    {
        // The queue we submit to must be a real D3D12 queue, not a wrapper expecting wrapped lists.
        if (!InModule(VtableOf(queue), L"d3d12"))
        {
            char mod[160];
            ModuleOf(VtableOf(queue), mod, sizeof(mod));
            WLOG("overlay: present queue %p vtable is in %s (not d3d12*.dll)", static_cast<void*>(queue), mod);
            Disable("present queue is not a native D3D12 queue");
            return;
        }
        if (!InitRenderer(queue))
            return;
    }

    // Timing cross-check of the queue choice (logged once).
    if (g_primaryPresents <= 600)
    {
        if (t_lastDirect == queue)
            ++g_timingMatch;
        else if (t_lastDirect)
            ++g_timingOther;
        else
            ++g_timingNone;
        if (g_primaryPresents == 600)
        {
            int direct = 0;
            for (int i = 0; i < g_queueCount.load(); ++i)
                direct += g_queues[i].type == D3D12_COMMAND_LIST_TYPE_DIRECT;
            WLOG("overlay: queue check over 600 presents: present thread's last DIRECT submit was our queue %llu, another queue "
                 "%llu, none %llu; DIRECT queues seen: %d",
                 static_cast<unsigned long long>(g_timingMatch), static_cast<unsigned long long>(g_timingOther),
                 static_cast<unsigned long long>(g_timingNone), direct);
        }
    }

    const uint64_t completed = g_r.fence->GetCompletedValue();
    if (completed == UINT64_MAX)
    {
        Disable("device removed (fence reads UINT64_MAX)");
        return;
    }
    Retire(completed);
    ProcessProbes(completed);
    if (g_occ.rbFence && completed >= g_occ.rbFence)
    {
        g_occ.rbFence = 0;
        ProcessOccReadback();
    }
    if (g_occ.reqReset.exchange(0))
    {
        g_occ.pairs.clear();
        g_occ.calibrated = false;
        g_occ.lastCal = "calibration reset";
        WLOG("occlusion: calibration reset");
    }
    if (g_occ.reqRetry.exchange(0) && g_occ.failed && g_occ.list[0])
    {
        g_occ.failed = false;
        g_occ.failWhy.clear();
        WLOG("occlusion: retrying after the earlier failure");
    }
    g_occHooks = g_listHooked && g_config.overlayOcclusion && !g_occ.failed && !g_disabled;
    if (occP % 30 == 0)
        LogNewCandidates();
    if (g_occ.reqNext.exchange(0))
    {
        const int n = std::min(g_candCount.load(), kMaxCand);
        for (int k = 1; k <= n; ++k)
        {
            const int i = (std::max(g_occ.lastChosen, g_config.occlusionDepthPick) + k + n) % n;
            if (g_cand[i].res.load() && g_cand[i].lastPresent.load() + 120 > occP)
            {
                g_config.occlusionDepthPick = i;
                break;
            }
        }
        WLOG("occlusion: next candidate requested -> OcclusionDepthPick=%d (-1 = automatic)", g_config.occlusionDepthPick);
    }
    if (g_r.captureFence && completed >= g_r.captureFence)
    {
        WriteCapture();
        g_r.captureFence = 0;
        g_captureRequested = false;
        g_captureDone = true;
    }

    const LinkView lv = ReadLink();
    // SkyCraft's rule: MC alive + in its world, and no host menu/loading screen.
    // M6a: only in Minecraft mode (not during a scene), or with F7 build mode on.
    const bool show = lv.mapped && lv.mcAlive && lv.inWorld && lv.inGame && !lv.menu && !lv.loading && wc::control::OverlayWanted();
    if (show)
        g_wantFramesTick = now;
    const int mode = DecideMode(sc, rec, desc.BufferDesc.Format);

    static uint64_t s_lastOccLog = 0;
    if (now - s_lastOccLog > 10000 && (g_depthFrames.load() || g_occ.chosen >= 0 || g_candCount.load()))
    {
        s_lastOccLog = now;
        const std::string line = OccStatusLine();
        WLOG("%s", line.c_str());
        AcquireSRWLockExclusive(&g_occTextLock);
        g_occStatus = line;
        ReleaseSRWLockExclusive(&g_occTextLock);
    }
    if (now - g_lastStatusLog > 30000)
    {
        g_lastStatusLog = now;
        ReadSyncSettings();
        WLOG("overlay: status presents=%llu draws=%llu uploads=%llu acquired=%llu busy=%llu | show=%d (mapped %d mc %d world %d "
             "inGame %d menu %d loading %d) tex=%ux%u bb=%ux%u %s",
             static_cast<unsigned long long>(g_stats.presents.load()), static_cast<unsigned long long>(g_stats.draws.load()),
             static_cast<unsigned long long>(g_stats.uploads.load()), static_cast<unsigned long long>(g_stats.acquired.load()),
             static_cast<unsigned long long>(g_stats.skippedBusy.load()), show, lv.mapped, lv.mcAlive, lv.inWorld, lv.inGame,
             lv.menu, lv.loading, g_r.texW, g_r.texH, bbW, bbH, ModeName(mode));
    }

    // Every frame the worker prepared, oldest first (all go into the history: the camera may
    // have been placed with any of them).
    UploadBuf* ready[kUploads]{};
    int nReady = 0;
    if (show)
    {
        AcquireSRWLockExclusive(&g_upLock);
        for (auto& b : g_up)
            if (b.state == kUpReady)
                ready[nReady++] = &b;
        ReleaseSRWLockExclusive(&g_upLock);
        for (int i = 1; i < nReady; ++i)
            for (int j = i; j > 0 && ready[j]->seq < ready[j - 1]->seq; --j)
                std::swap(ready[j], ready[j - 1]);
    }
    g_stats.visible = show && (g_r.texValid || nReady);
    if (!nReady && !(show && g_r.texValid))
        return;

    const uint32_t ci = g_r.frame % kFrames;
    FrameCtx& ctx = g_r.ctx[ci];
    if (completed < ctx.fence)
    {
        g_stats.skippedBusy.fetch_add(1, std::memory_order_relaxed);
        return; // never block the game's present thread on our own GPU work
    }
    if (nReady)
    {
        // The newest frame decides the size; older ones of another size (resize) are dropped.
        const UploadBuf* nw = ready[nReady - 1];
        const int want = g_config.overlayHistory < 2 ? 2 : g_config.overlayHistory > kMaxHistory ? kMaxHistory : g_config.overlayHistory;
        if (nw->w != g_r.texW || nw->h != g_r.texH || g_r.histCount != want)
        {
            if (completed < g_r.fenceValue)
                return; // recreate the history only with none of our GPU work in flight
            if (!EnsureHistory(nw->w, nw->h))
                return;
        }
    }
    HRESULT hr = ctx.alloc->Reset();
    if (SUCCEEDED(hr))
        hr = ctx.list->Reset(ctx.alloc, nullptr);
    if (FAILED(hr))
    {
        Disable("command list reset failed", hr);
        return;
    }
    // Image probe first: the band must be the Witcher's own picture, before we draw on it.
    int probeSlot = -1;
    if (show && pi.driving && g_config.overlayProbe && ProbeFormatOk(desc.BufferDesc.Format))
    {
        probeSlot = FreeProbeSlot(pi.present);
        if (probeSlot >= 0 && !RecordProbe(ctx.list, sc, desc, probeSlot))
            probeSlot = -1;
    }
    int uploaded = 0;
    for (int i = 0; i < nReady; ++i)
    {
        UploadBuf* up = ready[i];
        AcquireSRWLockExclusive(&g_upLock);
        const bool fits = up->w == g_r.texW && up->h == g_r.texH;
        up->state = fits ? kUpInFlight : kUpFree;
        up->fence = 0; // set after Signal
        ReleaseSRWLockExclusive(&g_upLock);
        if (!fits)
        {
            ready[i] = nullptr;
            continue;
        }
        RecordUpload(ctx.list, *up);
        ++uploaded;
    }
    bool drew = false, captured = false;
    camsync::Pick how = camsync::Pick::Newest;
    float pickErr = 0;
    const int pick = show && g_r.texValid ? PickFrame(pi, how, pickErr) : -1;
    if (pick >= 0)
    {
        if (!EnsurePso(desc.BufferDesc.Format))
            return;
        g_r.texFlags = g_r.hist[pick].flags;
        // Occlusion: W3's depth for this image (own list, see RunOcc), then the depth test.
        const HistTex& ht = g_r.hist[pick];
        std::string plain;
        if (!g_config.overlayOcclusion)
            plain = "OverlayOcclusion=0";
        else if (g_occ.failed)
            plain = "occlusion failed earlier: " + g_occ.failWhy;
        else if (!(ht.flags & proto::kOverlayWorldLayer))
            plain = "HUD-only frames";
        else if (!ht.hasDepth)
            plain = "the Minecraft frame has no depth (old mod, -Dwitchercraft.depth=false, a screen open, or too large)";
        else if (lv.screenOpen)
            plain = "a Minecraft screen is open";
        const bool cal = g_occ.reqCal.load() != 0;
        const bool wantCopy = g_occHooks.load() && !g_occ.failed && (plain.empty() || g_config.occlusionDebug == 2 || cal);
        g_occ.haveCopy = false;
        g_occ.cur = nullptr;
        if (wantCopy)
            RunOcc(ci, occWin, occP, completed, cal && ht.hasDepth ? true : cal, bbW, bbH, ht);
        if (plain.empty() && !g_occ.haveCopy)
            plain = "no W3 depth this frame (" + g_occ.why + ")";
        if (wantCopy)
            (g_occ.haveCopy ? g_occ.framesWith : g_occ.framesWithout)++;
        const bool occlude = plain.empty();
        if (occlude)
            ++g_occ.drawsOccluded;
        else
        {
            ++g_occ.drawsPlain;
            if (g_occ.lastPlainWhy != plain)
            {
                g_occ.lastPlainWhy = plain;
                static uint64_t s_lastWhyLog = 0; // flips with the chat screen / DRS hiccups: rate-limit
                if (g_config.overlayOcclusion && (ht.flags & proto::kOverlayWorldLayer) && now - s_lastWhyLog > 10000)
                {
                    s_lastWhyLog = now;
                    WLOG("occlusion: drawing Minecraft WITHOUT occlusion: %s", plain.c_str());
                }
            }
        }
        if (occlude && g_occ.lastPlainWhy.size())
        {
            g_occ.lastPlainWhy.clear();
            static bool s_loggedActive = false;
            if (!s_loggedActive)
                WLOG("occlusion: depth test active");
            s_loggedActive = true;
        }
        drew = RecordDraw(ctx.list, sc, desc, mode, lv, ci, pick, occlude, captured);
        if (cal && g_occ.haveCopy && !g_disabled)
        {
            RecordMcDepthReadback(ctx.list, ht); // after the draw; the W3 half is in the occlusion list
            g_occ.reqCal = 0;
            g_occ.calBbW = bbW, g_occ.calBbH = bbH;
            g_occ.calPose = ht.pose;
            WLOG("occlusion: calibration requested (pose pitch %.1f deg, MC depth %s)", ht.pose.pitch, g_occ.mcValid ? "yes" : "no");
        }        drew = RecordDraw(ctx.list, sc, desc, mode, lv, ci, pick, occlude, captured);
        if (g_disabled)
            return;
        camsync::OnPicked(how, pickErr);
        AcquireSRWLockExclusive(&g_compLock);
        g_comp.valid = true;
        g_comp.present = pi.present;
        g_comp.gameFrame = pi.gameFrame;
        g_comp.pick = static_cast<int>(how);
        g_comp.frameId = g_r.hist[pick].frameId;
        g_comp.pose = ToPose(g_r.hist[pick].pose);
        g_comp.camValid = pi.havePose;
        g_comp.cam = ToPose(pi.pose);
        ReleaseSRWLockExclusive(&g_compLock);
    }
    hr = ctx.list->Close();
    if (FAILED(hr))
    {
        Disable("command list close failed", hr);
        return;
    }
    ID3D12CommandList* lists[] = {ctx.list};
    if (g_oECL && VtableOf(queue) == g_queueVtable)
        g_oECL(queue, 1, lists); // straight to the original: not counted as a game submission
    else
        queue->ExecuteCommandLists(1, lists);
    const uint64_t v = ++g_r.fenceValue;
    hr = queue->Signal(g_r.fence, v);
    if (FAILED(hr))
    {
        Disable("queue Signal failed", hr);
        return;
    }
    ctx.fence = v;
    ++g_r.frame;
    if (uploaded)
    {
        AcquireSRWLockExclusive(&g_upLock);
        for (int i = 0; i < nReady; ++i)
            if (ready[i])
                ready[i]->fence = v;
        ReleaseSRWLockExclusive(&g_upLock);
        g_stats.uploads.fetch_add(static_cast<uint64_t>(uploaded), std::memory_order_relaxed);
    }
    if (g_occ.rbFence == UINT64_MAX)
        g_occ.rbFence = v; // calibration readbacks complete with the main list (it holds the MC half)
    if (probeSlot >= 0)
    {
        g_r.probe[probeSlot].fence = v;
        g_r.probe[probeSlot].present = pi.present;
    }
    if (captured)
        g_r.captureFence = v;
    if (drew)
    {
        const uint64_t n = g_stats.draws.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1)
            WLOG("overlay: first HUD frame composited (%ux%u overlay onto %ux%u %s, %s)", g_r.texW, g_r.texH, bbW, bbH,
                 FormatName(desc.BufferDesc.Format), ModeName(mode));
    }
}

void OnPresent(IDXGISwapChain* sc, UINT flags)
{
    g_stats.presents.fetch_add(1, std::memory_order_relaxed);
    if (flags & DXGI_PRESENT_TEST)
        return;
    if (!TryAcquireSRWLockExclusive(&g_renderLock))
        return; // another thread is presenting right now: skip rather than wait
    t_ours = true; // our own command lists go through the hooked list vtable too
    RenderFrame(sc);
    t_ours = false;
    ReleaseSRWLockExclusive(&g_renderLock);
}

void SafeOnPresent(IDXGISwapChain* sc, UINT flags)
{
    __try
    {
        OnPresent(sc, flags);
    }
    __except (SehFilter(GetExceptionCode(), "Present hook"))
    {
    }
}

struct FireLog
{
    const char* name;
    std::atomic<int> logged{0};
    std::atomic<HMODULE> lastCaller{nullptr};
};
FireLog g_firePresent{"Present"}, g_firePresent1{"Present1"};

// Logs which module calls a hook (first call and whenever the caller module changes, a few times).
void NoteFire(FireLog& f, void* ret, const void* obj)
{
    if (f.logged.load(std::memory_order_relaxed) >= 6)
        return;
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(ret), &m);
    if (m == f.lastCaller.exchange(m))
        return;
    f.logged.fetch_add(1);
    char caller[160];
    ModuleOf(ret, caller, sizeof(caller));
    WLOG("overlay: %s hook fired: swapchain %p (vtable %p), called from %s on thread %lu", f.name, obj, VtableOf(obj), caller,
         GetCurrentThreadId());
}

void SafeNoteQueue(ID3D12CommandQueue* q, void* ret)
{
    __try
    {
        NoteQueue(q, ret);
    }
    __except (SehFilter(GetExceptionCode(), "ExecuteCommandLists hook"))
    {
    }
}

void BeforeResize(IDXGISwapChain* sc)
{
    // Our command lists only reference a back buffer while they execute; make sure none is
    // still pending before the game destroys the buffers. Bounded waits only.
    bool locked = false;
    for (int i = 0; i < 500 && !locked; ++i)
    {
        locked = TryAcquireSRWLockExclusive(&g_renderLock) != 0;
        if (!locked)
            Sleep(1);
    }
    if (g_r.fence && g_r.fenceValue && g_r.fence->GetCompletedValue() < g_r.fenceValue)
    {
        if (SUCCEEDED(g_r.fence->SetEventOnCompletion(g_r.fenceValue, g_r.fenceEvent)))
            WaitForSingleObject(g_r.fenceEvent, 1000);
    }
    if (locked)
        ReleaseSRWLockExclusive(&g_renderLock);
    (void)sc;
}

void AfterResize(IDXGISwapChain* sc, HRESULT hr, UINT n, UINT w, UINT h, DXGI_FORMAT f, IUnknown* const* queues, const char* via)
{
    WLOG("overlay: %s(swapchain %p, %u buffers, %ux%u, %s) -> 0x%08lX", via, static_cast<void*>(sc), n, w, h,
         f == DXGI_FORMAT_UNKNOWN ? "keep format" : FormatName(f), static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && queues && queues[0])
    {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(queues[0]->QueryInterface(IID_PPV_ARGS(&q))) && q)
        {
            q->Release();
            ScSetQueue(sc, q, "ResizeBuffers1");
        }
    }
}

void SafeBeforeResize(IDXGISwapChain* sc)
{
    __try
    {
        BeforeResize(sc);
    }
    __except (SehFilter(GetExceptionCode(), "ResizeBuffers hook"))
    {
    }
}

void SafeAfterResize(IDXGISwapChain* sc, HRESULT hr, UINT n, UINT w, UINT h, DXGI_FORMAT f, IUnknown* const* queues,
                     const char* via)
{
    __try
    {
        AfterResize(sc, hr, n, w, h, f, queues, via);
    }
    __except (SehFilter(GetExceptionCode(), "ResizeBuffers hook"))
    {
    }
}

// ---- hooks ------------------------------------------------------------------------------------
void SafeRetireAfterDisable()
{
    __try
    {
        if (g_r.fence && g_r.graveCount)
            Retire(g_r.fence->GetCompletedValue());
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    const bool outer = t_presentDepth++ == 0;
    if (outer && !g_disabled.load(std::memory_order_relaxed))
    {
        NoteFire(g_firePresent, _ReturnAddress(), sc);
        SafeOnPresent(sc, flags);
    }
    else if (outer && g_r.graveCount && TryAcquireSRWLockExclusive(&g_renderLock))
    {
        SafeRetireAfterDisable();
        ReleaseSRWLockExclusive(&g_renderLock);
    }
    const HRESULT hr = g_oPresent(sc, sync, flags);
    --t_presentDepth;
    return hr;
}

HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params)
{
    const bool outer = t_presentDepth++ == 0;
    if (outer && !g_disabled.load(std::memory_order_relaxed))
    {
        NoteFire(g_firePresent1, _ReturnAddress(), sc);
        SafeOnPresent(sc, flags);
    }
    else if (outer && g_r.graveCount && TryAcquireSRWLockExclusive(&g_renderLock))
    {
        SafeRetireAfterDisable();
        ReleaseSRWLockExclusive(&g_renderLock);
    }
    const HRESULT hr = g_oPresent1(sc, sync, flags, params);
    --t_presentDepth;
    return hr;
}

HRESULT STDMETHODCALLTYPE HkResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
{
    if (!g_disabled.load(std::memory_order_relaxed))
        SafeBeforeResize(sc);
    const HRESULT hr = g_oResizeBuffers(sc, n, w, h, f, flags);
    if (!g_disabled.load(std::memory_order_relaxed))
        SafeAfterResize(sc, hr, n, w, h, f, nullptr, "ResizeBuffers");
    return hr;
}

HRESULT STDMETHODCALLTYPE HkResizeBuffers1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags,
                                           const UINT* nodeMasks, IUnknown* const* queues)
{
    if (!g_disabled.load(std::memory_order_relaxed))
        SafeBeforeResize(sc);
    const HRESULT hr = g_oResizeBuffers1(sc, n, w, h, f, flags, nodeMasks, queues);
    if (!g_disabled.load(std::memory_order_relaxed))
        SafeAfterResize(sc, hr, n, w, h, f, queues, "ResizeBuffers1");
    return hr;
}

HRESULT STDMETHODCALLTYPE HkSetColorSpace1(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE cs)
{
    const HRESULT hr = g_oSetColorSpace1(sc, cs);
    if (!g_disabled.load(std::memory_order_relaxed))
    {
        WLOG("overlay: SetColorSpace1(swapchain %p, %s) -> 0x%08lX", static_cast<void*>(sc), ColorSpaceName(cs),
             static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr))
            ScSetColorSpace(sc, cs);
    }
    return hr;
}

void STDMETHODCALLTYPE HkExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    if (!g_disabled.load(std::memory_order_relaxed))
        SafeNoteQueue(q, _ReturnAddress());
    g_oECL(q, n, lists);
}

// ---- installer --------------------------------------------------------------------------------
bool SafeInstallFactoryHooks()
{
    __try
    {
        return InstallFactoryHooks();
    }
    __except (SehFilter(GetExceptionCode(), "factory hook install"))
    {
        return false;
    }
}

bool SafeInstallViaDummyDevice()
{
    __try
    {
        return InstallViaDummyDevice();
    }
    __except (SehFilter(GetExceptionCode(), "dummy-device hook install"))
    {
        return false;
    }
}

DWORD WINAPI InstallerThread(void*)
{
    WLOG("overlay: installer thread started");
    if (!SafeInstallFactoryHooks())
        WLOG("overlay: factory hooks FAILED; relying on the dummy-device fallback");
    const int delay = g_config.overlayFallbackDelayMs > 0 ? g_config.overlayFallbackDelayMs : 0;
    for (int waited = 0; waited < delay && !g_swapHooked && !g_disabled; waited += 100)
        Sleep(100);
    if (!g_swapHooked && !g_disabled)
    {
        WLOG("overlay: no swapchain creation seen within %d ms; using the dummy-device fallback", delay);
        SafeInstallViaDummyDevice();
    }
    // Watchdog: hooks installed but the game never presents through them.
    for (int i = 0; i < 300 && !g_disabled; ++i)
        Sleep(100);
    if (g_swapHooked && g_stats.presents.load() == 0)
        WLOG("overlay: WARNING Present hook installed but never called after 30 s: the game presents through a "
             "swapchain/vtable we did not hook");
    return 0;
}
} // namespace

void Start()
{
    if (!g_config.overlay)
    {
        WLOG("overlay: disabled in ini (Overlay=0)");
        return;
    }
    HANDLE t = CreateThread(nullptr, 0, InstallerThread, nullptr, 0, nullptr);
    if (t)
        CloseHandle(t);
    else
        WLOG("overlay: cannot start installer thread (%lu)", GetLastError());
}

bool InstallNow(bool viaDummyDevice)
{
    bool ok = SafeInstallFactoryHooks();
    if (viaDummyDevice)
        ok = SafeInstallViaDummyDevice() && ok;
    return ok;
}

void RequestCapture(const wchar_t* bmpPath)
{
    wcsncpy_s(g_capturePath, bmpPath, _TRUNCATE);
    g_captureDone = false;
    g_captureRequested = true;
}

bool CaptureDone()
{
    return g_captureDone.load();
}

Stats GetStats()
{
    Stats s;
    s.presents = g_stats.presents.load();
    s.draws = g_stats.draws.load();
    s.uploads = g_stats.uploads.load();
    s.acquired = g_stats.acquired.load();
    s.skippedBusy = g_stats.skippedBusy.load();
    s.texW = g_stats.texW.load();
    s.texH = g_stats.texH.load();
    s.bbW = g_stats.bbW.load();
    s.bbH = g_stats.bbH.load();
    s.colorMode = g_stats.colorMode.load();
    s.visible = g_stats.visible.load();
    s.disabled = g_disabled.load();
    return s;
}

bool LastDrawnPose(Pose& out)
{
    AcquireSRWLockShared(&g_poseLock);
    const bool ok = g_poseValid;
    if (ok)
        out = g_pose;
    ReleaseSRWLockShared(&g_poseLock);
    return ok;
}

bool OcclusionModel(float& A, float& B, bool& calibrated, bool& lastOccluded)
{
    float farD;
    const char* what;
    OccModel(A, B, farD, &what);
    calibrated = g_occ.calibrated;
    lastOccluded = g_occ.lastPlainWhy.empty() && g_occ.drawsOccluded > 0;
    return g_occ.lastChosen >= 0 && !g_occ.failed;
}

void OcclusionStats(uint64_t& switches, uint64_t& framesWith, uint64_t& framesWithout, bool& failed)
{
    switches = g_occ.switches, framesWith = g_occ.framesWith, framesWithout = g_occ.framesWithout, failed = g_occ.failed;
}

std::string OcclusionCommand(int cmd, float value)
{
    switch (cmd)
    {
    case 1:
        g_config.overlayOcclusion = value != 0.0f;
        if (g_config.overlayOcclusion)
            g_occ.reqRetry = 1;
        break;
    case 2: g_config.occlusionBias = value; break;
    case 3: g_config.occlusionDepthPick = static_cast<int>(value); break;
    case 4: g_config.occlusionNear = value, g_config.occlusionModelSet = false, g_occ.calibrated = false; break;
    case 5: g_config.occlusionFar = value, g_config.occlusionModelSet = false, g_occ.calibrated = false; break;
    case 6: g_config.occlusionReverseZ = static_cast<int>(value), g_config.occlusionModelSet = false, g_occ.calibrated = false; break;
    case 7: g_occ.reqCal = 1; break;
    case 8: g_occ.reqReset = 1, g_config.occlusionModelSet = false; break;
    case 9: g_config.occlusionDebug = static_cast<int>(value); break;
    case 10: g_config.occlusionSoft = value; break;
    case 11: g_occ.reqNext = 1; break;
    default: break;
    }
    if (cmd != 0)
        WLOG("occlusion: command %d (%g)", cmd, value);
    AcquireSRWLockShared(&g_occTextLock);
    std::string s = g_occStatus;
    ReleaseSRWLockShared(&g_occTextLock);
    return s;
}

Composited LastComposited()
{
    AcquireSRWLockShared(&g_compLock);
    const Composited c = g_comp;
    ReleaseSRWLockShared(&g_compLock);
    return c;
}
} // namespace wc::overlay
