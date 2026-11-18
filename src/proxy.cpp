// proxy.cpp -- DllMain, resolving the original d3dx9_40.dll, font-creation hooks.
//
// The game loads d3dx9_40.dll from its own folder (verified with a module list:
// no copy from System32 is involved), so placing this DLL there makes us the
// single entry point for every D3DX call the engine makes.
#include "a11y.h"

#include <stdio.h>
#include <string.h>

extern "C" {
struct FwdEntry {
    const char* name;
    void** slot;
};
extern const FwdEntry g_fwd[];
void forward_install_defaults(void);
extern void* p_D3DXCreateFontW;
extern void* p_D3DXCreateFontA;
extern void* p_D3DXCreateFontIndirectW;
extern void* p_D3DXCreateFontIndirectA;
}

static HMODULE g_orig;
static CRITICAL_SECTION g_resCs;
static int g_resCsReady;
static wchar_t g_origPath[MAX_PATH];
static int g_origFailed;

static void BuildOrigPath(void)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(0, self, MAX_PATH);
    wchar_t* slash = wcsrchr(self, L'\\');
    if (slash) { *slash = 0; }
    wsprintfW(g_origPath, L"%s\\d3dx9_40_orig.dll", self);
}

// Called from the generated lazy trampolines the first time an entry point is
// used.  By then the loader lock is gone, so LoadLibrary is safe here.
extern "C" void* proxy_resolve(const char* name)
{
    if (!g_resCsReady) {
        InitializeCriticalSection(&g_resCs);
        g_resCsReady = 1;
    }
    EnterCriticalSection(&g_resCs);
    if (!g_orig && !g_origFailed) {
        g_orig = LoadLibraryW(g_origPath);
        if (!g_orig) {
            g_origFailed = 1;
            Logf(L"FATAL: cannot load %s (GetLastError=%lu)", g_origPath, GetLastError());
        } else {
            Logf(L"original DLL loaded: %s = %p", g_origPath, (void*)g_orig);
        }
    }
    void* fn = 0;
    if (g_orig) {
        fn = (void*)GetProcAddress(g_orig, name);
        if (!fn) { Logf(L"missing export in original: %hs", name); }
    }
    LeaveCriticalSection(&g_resCs);
    return fn;
}

// Point every slot at the real function.  Must run after the original loaded.
static void ResolveAll(void)
{
    if (!proxy_resolve("D3DXCreateSprite")) { return; }
    int ok = 0, missing = 0;
    for (const FwdEntry* e = g_fwd; e->name; e++) {
        void* fn = (void*)GetProcAddress(g_orig, e->name);
        if (fn) { *e->slot = fn; ok++; } else { missing++; }
    }
    Logf(L"RESOLVE ok=%d missing=%d", ok, missing);
}

// ---- D3DXCreateFont* interception ----------------------------------------
typedef HRESULT(__stdcall* CreateFontWFn)(void* dev, INT height, UINT width, UINT weight,
                                          UINT mip, BOOL italic, DWORD charset,
                                          DWORD precision, DWORD quality,
                                          DWORD pitchfamily, LPCWSTR face, void** font);
static CreateFontWFn g_origCreateFontW;

static HRESULT __stdcall wrap_CreateFontW(void* dev, INT height, UINT width, UINT weight,
                                          UINT mip, BOOL italic, DWORD charset,
                                          DWORD precision, DWORD quality,
                                          DWORD pitchfamily, LPCWSTR face, void** font)
{
    if (!g_origCreateFontW) {
        g_origCreateFontW = (CreateFontWFn)proxy_resolve("D3DXCreateFontW");
    }
    if (!g_origCreateFontW) { return (HRESULT)0x8876086C; } // D3DERR_INVALIDCALL
    HRESULT hr = g_origCreateFontW(dev, height, width, weight, mip, italic, charset,
                                   precision, quality, pitchfamily, face, font);
    if (SUCCEEDED(hr) && font && *font) {
        Logf(L"CREATEFONT h=%d weight=%u face=%s font=%p", height, weight,
             face ? face : L"(null)", *font);
        FontHookOnCreated(*font);
    }
    return hr;
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) { return TRUE; }
    DisableThreadLibraryCalls(self);

    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(self, dir, MAX_PATH);
    for (wchar_t* p = dir + wcslen(dir); p > dir; --p) {
        if (*p == L'\\') { *p = 0; break; }
    }

    LogInit(dir);
    Logf(L"=== Lucy accessibility patch %s loaded (pid %lu) ===", A11Y_VERSION,
         GetCurrentProcessId());
    ConfigLoad();

    forward_install_defaults();
    BuildOrigPath();

    // Resolve the original DLL now: we are past the import table of Engine.dll,
    // but the loader lock is held, so this only happens on the first D3DX call.
    // Intercept the font factories: the font objects are created here.
    p_D3DXCreateFontW = (void*)&wrap_CreateFontW;
    (void)p_D3DXCreateFontA;
    (void)p_D3DXCreateFontIndirectW;
    (void)p_D3DXCreateFontIndirectA;

    StartA11y(self);
    return TRUE;
}

// Called by the tick thread once the original DLL is available.
void ProxyInitLate(void)
{
    ResolveAll();
    if (g_orig) {
        // restore the one slot we replaced after resolving everything
        p_D3DXCreateFontW = (void*)&wrap_CreateFontW;
    }
}

void ProxyNoteFontsCaptured(void) {}
