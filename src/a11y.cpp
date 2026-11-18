// a11y.cpp -- logging, config, repeat buffer and the low-level keyboard hook.
#include "a11y.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

A11yConfig g_cfg;

static wchar_t g_dataDir[MAX_PATH];
static wchar_t g_logPath[MAX_PATH];
static CRITICAL_SECTION g_logCs;
static FILE* g_log;
static int g_logReady;
static HMODULE g_self;
static HHOOK g_kbHook;
static HANDLE g_kbThread;
static DWORD g_kbTid;

static void EnsureDir(const wchar_t* path)
{
    wchar_t tmp[MAX_PATH];
    wcsncpy_s(tmp, MAX_PATH, path, _TRUNCATE);
    for (wchar_t* p = tmp + 3; *p; p++) {
        if (*p == L'\\') {
            *p = 0;
            CreateDirectoryW(tmp, 0);
            *p = L'\\';
        }
    }
    CreateDirectoryW(tmp, 0);
}

void LogInit(const wchar_t* baseDir)
{
    InitializeCriticalSection(&g_logCs);
    wsprintfW(g_dataDir, L"%s\\a11y", baseDir);
    EnsureDir(g_dataDir);
    wsprintfW(g_logPath, L"%s\\%s", g_dataDir, A11Y_LOG_NAME);
    g_log = _wfopen(g_logPath, L"ab");
    if (g_log) {
        // Write UTF-8 ourselves: the speech layer appends to the same file and
        // both writers must produce identical encoding.
        if (ftell(g_log) == 0) {
            const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
            fwrite(bom, 1, 3, g_log);
        }
        g_logReady = 1;
    }
}

const wchar_t* DataDir(void) { return g_dataDir; }

void Logf(const wchar_t* fmt, ...)
{
    if (!g_logReady) { return; }
    wchar_t msg[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char utf8[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, utf8, (int)sizeof(utf8), 0, 0);
    if (n <= 0) { return; }
    char line[4300];
    int m = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u.%03u] %s\n",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, utf8);
    if (m <= 0) { return; }
    EnterCriticalSection(&g_logCs);
    fwrite(line, 1, (size_t)m, g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}

// ---------------------------------------------------------------- config ---
static void Trim(wchar_t* s)
{
    wchar_t* p = s;
    while (*p == L' ' || *p == L'\t') { p++; }
    if (p != s) { memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t)); }
    size_t n = wcslen(s);
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t' || s[n - 1] == L'\r' || s[n - 1] == L'\n')) {
        s[--n] = 0;
    }
}

void ConfigLoad(void)
{
    // defaults
    wcscpy_s(g_cfg.backend, L"auto");
    g_cfg.rate = 50;
    g_cfg.volume = 100;
    g_cfg.voiceWindowMs = 1200;
    g_cfg.speakVoicedLines = 0;
    g_cfg.minStableMs = 260;
    g_cfg.logDraws = 0;
    g_cfg.keyRepeat = VK_BACK;
    g_cfg.keyPrev = VK_BACK;      // with Ctrl
    g_cfg.keyBackendMod = VK_MENU; // Alt
    g_cfg.keyBackend = VK_F9;

    wchar_t path[MAX_PATH];
    wsprintfW(path, L"%s\\%s", g_dataDir, A11Y_INI_NAME);
    FILE* f = _wfopen(path, L"rb");
    if (!f) {
        ConfigSave();
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '#') { continue; }
        *eq = 0;
        wchar_t key[64], val[256];
        MultiByteToWideChar(CP_UTF8, 0, line, -1, key, 64);
        MultiByteToWideChar(CP_UTF8, 0, eq + 1, -1, val, 256);
        Trim(key);
        Trim(val);
        if (!_wcsicmp(key, L"backend")) { wcsncpy_s(g_cfg.backend, val, _TRUNCATE); }
        else if (!_wcsicmp(key, L"rate")) { g_cfg.rate = _wtoi(val); }
        else if (!_wcsicmp(key, L"volume")) { g_cfg.volume = _wtoi(val); }
        else if (!_wcsicmp(key, L"voice_window_ms")) { g_cfg.voiceWindowMs = _wtoi(val); }
        else if (!_wcsicmp(key, L"speak_voiced_lines")) { g_cfg.speakVoicedLines = _wtoi(val); }
        else if (!_wcsicmp(key, L"min_stable_ms")) { g_cfg.minStableMs = _wtoi(val); }
        else if (!_wcsicmp(key, L"log_draws")) { g_cfg.logDraws = _wtoi(val); }
    }
    fclose(f);
    if (g_cfg.rate < 0) { g_cfg.rate = 0; }
    if (g_cfg.rate > 100) { g_cfg.rate = 100; }
    if (g_cfg.volume < 0) { g_cfg.volume = 0; }
    if (g_cfg.volume > 100) { g_cfg.volume = 100; }
    if (g_cfg.voiceWindowMs < 200) { g_cfg.voiceWindowMs = 200; }
    if (g_cfg.voiceWindowMs > 6000) { g_cfg.voiceWindowMs = 6000; }
    if (g_cfg.minStableMs < 80) { g_cfg.minStableMs = 80; }
}

void ConfigSave(void)
{
    wchar_t path[MAX_PATH];
    wsprintfW(path, L"%s\\%s", g_dataDir, A11Y_INI_NAME);
    FILE* f = _wfopen(path, L"wb");
    if (!f) { return; }
    char backend[64] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, g_cfg.backend, -1, backend, 64, 0, 0);
    fprintf(f, "; 露西 -她所期望的一切- 无障碍补丁 配置\r\n"
               "; backend = auto | nvda | zdsr | sapi | tolk\r\n"
               "backend=%s\r\n"
               "rate=%d\r\n"
               "volume=%d\r\n"
               "voice_window_ms=%d\r\n"
               "speak_voiced_lines=%d\r\n"
               "min_stable_ms=%d\r\n"
               "; log_draws=1 logs every painted string (diagnostics only)\r\n"
               "log_draws=%d\r\n",
            backend, g_cfg.rate, g_cfg.volume, g_cfg.voiceWindowMs,
            g_cfg.speakVoicedLines, g_cfg.minStableMs, g_cfg.logDraws);
    fclose(f);
}

// --------------------------------------------------------- repeat buffer ---
// Only story lines and choices are ever pushed here (design rule), so menu
// labels and system messages must use Speech_Speak directly via Say().
#define REPEAT_MAX 24
#define REPEAT_LEN 2048
static wchar_t g_rep[REPEAT_MAX][REPEAT_LEN];
static int g_repCount;
static int g_repPos;
static CRITICAL_SECTION g_repCs;
static int g_repReady;

const wchar_t* RepeatLast(void)
{
    if (g_repCount == 0) { return L""; }
    int idx = (g_repPos - 1 + REPEAT_MAX) % REPEAT_MAX;
    return g_rep[idx];
}

// Ctrl+Backspace walks back through the re-read buffer, one line per press.
static int g_repBack;

void RepeatPush(const wchar_t* text)
{
    if (!text || !*text) { return; }
    if (!g_repReady) { InitializeCriticalSection(&g_repCs); g_repReady = 1; }
    EnterCriticalSection(&g_repCs);
    // ignore an exact repeat of the newest entry (redraws, replays)
    if (g_repCount > 0 && !wcscmp(RepeatLast(), text)) {
        LeaveCriticalSection(&g_repCs);
        return;
    }
    wchar_t* dst = g_rep[g_repPos];
    wcsncpy_s(dst, REPEAT_LEN, text, _TRUNCATE);
    g_repPos = (g_repPos + 1) % REPEAT_MAX;
    if (g_repCount < REPEAT_MAX) { g_repCount++; }
    g_repBack = 0;
    LeaveCriticalSection(&g_repCs);
}

void RepeatPrev(void)
{
    if (g_repCount == 0) {
        Speech_SpeakImmediate(L"没有可重读的剧情文本");
        return;
    }
    if (g_repBack + 1 >= g_repCount) {
        g_repBack = 0;   // wrap to the newest line
    } else {
        g_repBack++;
    }
    int idx = (g_repPos - 1 - g_repBack + REPEAT_MAX * 2) % REPEAT_MAX;
    Say(g_rep[idx]);
}

// ------------------------------------------------------------- speech glue -
// Single exit point for everything the patch says on its own.
void Say(const wchar_t* text)
{
    if (!text || !*text) { return; }
    Logf(L"say: \"%s\"", text);
    Speech_Speak(text);
}

void SayReplay(void)
{
    const wchar_t* t = RepeatLast();
    if (!t || !*t) {
        Logf(L"re-read: buffer empty");
        Speech_SpeakImmediate(L"没有可重读的剧情文本");
        return;
    }
    Logf(L"re-read: \"%s\"", t);
    // Re-reading is a user action: it interrupts whatever is being said now.
    Speech_SpeakImmediate(t);
}

// -------------------------------------------------------------- window/keys -
HWND FindGameWindow(void)
{
    static const wchar_t* kClass = L"NekoEngine";
    struct Ctx { DWORD pid; HWND hwnd; } ctx;
    ctx.pid = GetCurrentProcessId();
    ctx.hwnd = 0;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        Ctx* c = (Ctx*)l;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != c->pid || !IsWindowVisible(h)) { return TRUE; }
        wchar_t cls[64];
        GetClassNameW(h, cls, 64);
        if (!wcscmp(cls, kClass)) { c->hwnd = h; return FALSE; }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.hwnd;
}

static int GameIsForeground(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) { return 0; }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) { return 0; }
    wchar_t cls[64];
    GetClassNameW(fg, cls, 64);
    return !wcscmp(cls, L"NekoEngine");
}

// Hotkeys that belong to the patch. Return 1 to swallow the key.
static int HandleHotkey(int vk, int isDown)
{
    static int s_alt = 0, s_shift = 0;
    if (vk == VK_MENU) { s_alt = isDown; return 0; }
    if (vk == VK_SHIFT) { s_shift = isDown; return 0; }
    if (!isDown) { return 0; }

    int ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    if (vk == g_cfg.keyRepeat && s_alt && s_shift) {
        return 0; // Alt+Shift+Backspace is not ours
    }
    if (vk == g_cfg.keyBackend && s_alt && s_shift) {
        int n = Speech_CycleBackend();
        Logf(L"hotkey: backend cycled -> %s (ret=%d)", Speech_BackendName(), n);
        return 1;
    }
    if (vk == g_cfg.keyRepeat) {
        if (ctrl) { RepeatPrev(); } else { SayReplay(); }
        Logf(L"hotkey: re-read (ctrl=%d) vk=%d", ctrl, vk);
        return 1;
    }
    return 0;
}

static LRESULT CALLBACK LowLevelKb(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)l;
        int isDown = (w == WM_KEYDOWN || w == WM_SYSKEYDOWN);
        if (GameIsForeground()) {
            if (g_cfg.logDraws && isDown) {   // diagnostics: what reaches the hook
                static int s_keySeen;
                if (s_keySeen < 300) {
                    s_keySeen++;
                    Logf(L"key: vk=%d down", (int)k->vkCode);
                }
            }
            if (HandleHotkey((int)k->vkCode, isDown)) { return 1; }
            MenuOnKey((int)k->vkCode, isDown);
            // MenuOnKey has no way to report back here yet; the arrow/enter keys
            // it consumes are swallowed inside via g_swallowKey.
            if (MenuConsumedKey()) { return 1; }
        }
    }
    return CallNextHookEx(g_kbHook, code, w, l);
}

static DWORD WINAPI KeyThread(void*)
{
    g_kbTid = GetCurrentThreadId();
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKb, g_self, 0);
    if (!g_kbHook) {
        Logf(L"keyboard hook failed, GetLastError=%lu", GetLastError());
        return 0;
    }
    Logf(L"keyboard hook installed");
    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static DWORD WINAPI TickThread(void*)
{
    // Everything heavy is initialised here, never in DllMain.
    ProxyInitLate();

    // The launcher (Lucy.exe start-up stub) loads Engine.dll too, so the proxy is
    // also present in a process that never shows a game window.  Wait for the real
    // window before touching speech: starting the screen reader connection in the
    // launcher only produces noise (and a second log) before it exits.
    HWND w = 0;
    for (int i = 0; i < 150 && !w; i++) {
        w = FindGameWindow();
        if (!w) { Sleep(100); }
    }
    if (!w) {
        Logf(L"a11y: no NekoEngine window after 15 s, this process is not the game; idle");
        return 0;
    }
    Logf(L"game window found: %p", (void*)w);
    TextInit();
    Speech_Init();
    Audio_InitAsync();
    Logf(L"a11y: subsystems up, backend=%s", Speech_BackendName());
    MenuSetGameWnd(w);
    MenuInit(w);

    for (;;) {
        Sleep(30);
        DWORD now = GetTickCount();
        TextTick(now);
        MenuTick(now);
    }
    return 0;
}

void StartA11y(HMODULE self)
{
    g_self = self;
    CreateThread(0, 0, TickThread, 0, 0, 0);
    CreateThread(0, 0, KeyThread, 0, 0, 0);
}
