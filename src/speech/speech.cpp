// speech.cpp -- the patch's voice output.
//
// Two backends for now, both inside the 32-bit game process:
//   nvda : nvdaControllerClient32.dll, loaded by absolute path from a11y\
//   sapi : ISpVoice (SAPI5 COM), always available on Windows
// "auto" prefers NVDA when the client DLL is present and NVDA is running.
//
// Initialisation happens on the patch's worker thread, never in DllMain: NVDA
// and COM both dislike being set up under the loader lock.
#include "a11y.h"

#include <stdio.h>
#include <string.h>

#include <objbase.h>
#include <sapi.h>

typedef int (__stdcall* NvdaSpeakFn)(const wchar_t*);
typedef int (__stdcall* NvdaCancelFn)(void);
typedef int (__stdcall* NvdaTestFn)(void);

static HMODULE g_nvda;
static NvdaSpeakFn g_nvdaSpeak;
static NvdaCancelFn g_nvdaCancel;
static NvdaTestFn g_nvdaTest;
static ISpVoice* g_spVoice;
static int g_inited;
static int g_useNvda;
static wchar_t g_backendName[32] = L"(none)";
static CRITICAL_SECTION g_speechCs;
static int g_csReady;

static void Cs(void)
{
    if (!g_csReady) {
        InitializeCriticalSection(&g_speechCs);
        g_csReady = 1;
    }
}

static int LoadNvda(void)
{
    if (g_nvda) { return 1; }
    const wchar_t* names[] = { L"nvdaControllerClient32.dll", L"nvdaControllerClient.dll" };
    wchar_t path[MAX_PATH];
    for (int i = 0; i < 2; i++) {
        wsprintfW(path, L"%s\\%s", DataDir(), names[i]);
        g_nvda = LoadLibraryW(path);
        if (!g_nvda) {
            wsprintfW(path, L"%s\\..\\%s", DataDir(), names[i]);
            g_nvda = LoadLibraryW(path);
        }
        if (g_nvda) { break; }
    }
    if (!g_nvda) {
        Logf(L"speech: nvda client not found");
        return 0;
    }
    g_nvdaSpeak = (NvdaSpeakFn)GetProcAddress(g_nvda, "nvdaController_speakText");
    g_nvdaCancel = (NvdaCancelFn)GetProcAddress(g_nvda, "nvdaController_cancelSpeech");
    g_nvdaTest = (NvdaTestFn)GetProcAddress(g_nvda, "nvdaController_testIfRunning");
    if (!g_nvdaSpeak || !g_nvdaTest) {
        Logf(L"speech: nvda client exports missing");
        return 0;
    }
    if (g_nvdaTest() != 0) {
        Logf(L"speech: nvda is not running");
        return 0;
    }
    return 1;
}

static int InitSapi(void)
{
    if (g_spVoice) { return 1; }
    HRESULT hr = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    (void)hr;  // S_OK / S_FALSE / RPC_E_CHANGED_MODE are all survivable
    hr = CoCreateInstance(CLSID_SpVoice, 0, CLSCTX_ALL, IID_ISpVoice, (void**)&g_spVoice);
    if (FAILED(hr) || !g_spVoice) {
        Logf(L"speech: SAPI voice unavailable, hr=0x%08lX", (unsigned long)hr);
        return 0;
    }
    // Prefer a Chinese voice: an English voice returns S_OK for Chinese text and
    // stays silent, which looks like a bug to the player.
    ISpObjectTokenCategory* cat = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_SpObjectTokenCategory, 0, CLSCTX_ALL,
                                   IID_ISpObjectTokenCategory, (void**)&cat))) {
        if (SUCCEEDED(cat->SetId(SPCAT_VOICES, FALSE))) {
            IEnumSpObjectTokens* en = 0;
            if (SUCCEEDED(cat->EnumTokens(L"Language=804", 0, &en)) && en) {
                ISpObjectToken* tok = 0;
                ULONG got = 0;
                if (SUCCEEDED(en->Next(1, &tok, &got)) && got && tok) {
                    g_spVoice->SetVoice(tok);
                    Logf(L"speech: SAPI voice set to Language=804 (Chinese)");
                    tok->Release();
                }
                en->Release();
            }
        }
        cat->Release();
    }
    if (g_cfg.rate >= 0) {
        g_spVoice->SetRate((long)((g_cfg.rate - 50) / 10));
    }
    if (g_cfg.volume >= 0) {
        g_spVoice->SetVolume((USHORT)(g_cfg.volume * 257 / 100));
    }
    return 1;
}

static void SetBackendName(void)
{
    if (g_useNvda == 1) { wcscpy_s(g_backendName, L"NVDA"); }
    else if (g_useNvda == 2) { wcscpy_s(g_backendName, L"SAPI"); }
    else { wcscpy_s(g_backendName, L"无"); }
}

static void SelectBackend(const wchar_t* want)
{
    g_useNvda = 0;
    if (!want || !_wcsicmp(want, L"auto")) {
        if (LoadNvda()) { g_useNvda = 1; }
        else if (InitSapi()) { g_useNvda = 2; }
    } else if (!_wcsicmp(want, L"nvda")) {
        if (LoadNvda()) { g_useNvda = 1; }
    } else if (!_wcsicmp(want, L"sapi")) {
        if (InitSapi()) { g_useNvda = 2; }
    } else if (!_wcsicmp(want, L"zdsr") || !_wcsicmp(want, L"tolk")) {
        Logf(L"speech: backend %s is not wired in this build yet", want);
        if (LoadNvda()) { g_useNvda = 1; }
        else if (InitSapi()) { g_useNvda = 2; }
    }
    SetBackendName();
    Logf(L"speech: backend = %s", g_backendName);
}

int Speech_Init(void)
{
    Cs();
    EnterCriticalSection(&g_speechCs);
    if (!g_inited) {
        SelectBackend(g_cfg.backend);
        g_inited = 1;
    }
    LeaveCriticalSection(&g_speechCs);
    return g_useNvda;
}

void Speech_Speak(const wchar_t* text)
{
    if (!text || !*text) { return; }
    Cs();
    EnterCriticalSection(&g_speechCs);
    if (!g_inited) { SelectBackend(g_cfg.backend); g_inited = 1; }
    if (g_useNvda == 1 && g_nvdaSpeak) {
        g_nvdaSpeak(text);
    } else if (g_useNvda == 2 && g_spVoice) {
        g_spVoice->Speak(text, SPF_ASYNC | SPF_PURGEBEFORESPEAK, 0);
    } else {
        Logf(L"speech: no backend, dropped: %s", text);
    }
    LeaveCriticalSection(&g_speechCs);
}

void Speech_Stop(void)
{
    Cs();
    EnterCriticalSection(&g_speechCs);
    if (g_useNvda == 1 && g_nvdaCancel) { g_nvdaCancel(); }
    else if (g_useNvda == 2 && g_spVoice) {
        g_spVoice->Speak(0, SPF_ASYNC | SPF_PURGEBEFORESPEAK, 0);
    }
    LeaveCriticalSection(&g_speechCs);
}

int Speech_CycleBackend(void)
{
    Cs();
    EnterCriticalSection(&g_speechCs);
    g_useNvda = (g_useNvda == 1) ? 2 : 1;
    if (g_useNvda == 1 && !LoadNvda()) { g_useNvda = 2; }
    if (g_useNvda == 2 && !InitSapi()) { g_useNvda = 1; }
    SetBackendName();
    LeaveCriticalSection(&g_speechCs);

    wchar_t msg[64];
    wsprintfW(msg, L"语音后端：%s", g_backendName);
    Speech_Speak(msg);
    Logf(L"speech: cycled -> %s", g_backendName);
    return g_useNvda;
}

const wchar_t* Speech_BackendName(void) { return g_backendName; }

void Speech_Shutdown(void)
{
    if (g_spVoice) {
        g_spVoice->Speak(0, SPF_ASYNC | SPF_PURGEBEFORESPEAK, 0);
        g_spVoice->Release();
        g_spVoice = 0;
    }
    if (g_nvda) {
        FreeLibrary(g_nvda);
        g_nvda = 0;
    }
}
