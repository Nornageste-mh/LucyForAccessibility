// fonthook.cpp -- capture the text the engine paints with ID3DXFont::DrawTextW.
//
// The engine creates its fonts through D3DXCreateFontW (only the W entry point
// is imported) and then draws every glyph and every line through the font
// object's vtable.  Slot 15 of that vtable is the DrawTextW entry:
//
//   a1 = font object, a2 = unused (always 0), a3 = LPCWSTR string,
//   a4 = character count (-1 = NUL terminated), a5 = address of {x, y, w, h},
//   a6 = format flags, a7 = colour, and the call pops 7 arguments (ret 0x1C).
//
// The typewriter effect is drawn one character at a time (count == 1, format
// 0x100/0x101); the whole line is drawn with count == -1, format 0x410 and a
// box 990 pixels wide.  UI strings are drawn in small boxes (200x24 etc.).
#include "a11y.h"

#include <string.h>

typedef HRESULT(__stdcall* DrawTextWFn)(void* self, void* unused, const WCHAR* str,
                                        INT count, const void* rect, DWORD format,
                                        DWORD color);

static void* g_origVtable;      // untouched copy (points into d3dx9_40_orig)
static void* g_ourVtable;       // copy with slot 15 replaced
static DrawTextWFn g_origDrawText;
static int g_vtableReady;

// ---- line assembly state --------------------------------------------------
#define PENDING_MAX 8
#define LINE_LEN 2048
struct Pending {
    wchar_t text[LINE_LEN];
    DWORD appeared;
    int used;
    int evaluated;
};
static Pending g_pending[PENDING_MAX];
static CRITICAL_SECTION g_cs;
static int g_csReady;
static wchar_t g_curRaw[LINE_LEN];      // last raw story draw (grows while typing)
static DWORD g_curRawTick;
static wchar_t g_lastEmitted[LINE_LEN]; // last line handed to the pipeline
static DWORD g_lastEmitTick;
static int g_storyActive;

static void Cs(void)
{
    if (!g_csReady) {
        InitializeCriticalSection(&g_cs);
        g_csReady = 1;
    }
}

// Replace "\n" with a pause so the speech backend reads natural sentences.
static void Normalize(wchar_t* s)
{
    for (wchar_t* p = s; *p; p++) {
        if (*p == L'\r' || *p == L'\n' || *p == L'|') { *p = L'，'; }
    }
    // collapse duplicated punctuation produced by the replacement
    while (wcsstr(s, L"，，")) {
        wchar_t* p = wcsstr(s, L"，，");
        memmove(p, p + 1, (wcslen(p + 1) + 1) * sizeof(wchar_t));
    }
    // trim
    wchar_t* p = s;
    while (*p == L' ' || *p == L'，') { p++; }
    if (p != s) { memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t)); }
    size_t n = wcslen(s);
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'，')) { s[--n] = 0; }
}

static int IsStoryDraw(const wchar_t* str, INT count, DWORD format, const LONG* box)
{
    if (!str || !*str) { return 0; }
    if (count == 1) { return 0; }              // per-glyph typewriter draw
    if (format & 0x100) { return 0; }          // small UI glyph runs
    if (box && box[2] < 600) { return 0; }     // narrow box: menu/UI text
    (void)box;
    return 1;
}

// Called from the tick thread.
static void EvaluatePending(DWORD now)
{
    for (int i = 0; i < PENDING_MAX; i++) {
        Pending* p = &g_pending[i];
        if (!p->used || p->evaluated) { continue; }
        // The audio judge answers 1 (voiced), 0 (not voiced) or -1 (undecided).
        // Never guess: ask again after the recommended delay.
        int more = Audio_RecommendDelayMs();
        if (more > 0) { continue; }
        int voiced = Audio_WasLineVoiced();
        if (voiced < 0) { continue; }
        p->evaluated = 1;
        Logf(L"line ready: voiced=%d \"%s\"", voiced, p->text);
        RepeatPush(p->text);
        if (!voiced || g_cfg.speakVoicedLines) {
            Say(p->text);
        } else {
            Logf(L"line is voiced -> not read automatically (press Backspace to re-read)");
        }
        (void)now;
    }
}

void TextInit(void)
{
    Cs();
    TextSetStoryActive(1);
}

void TextSetStoryActive(int on) { g_storyActive = on; }

// DrawTextW hook: capture, then draw exactly as before.
static HRESULT __stdcall HookDrawTextW(void* self, void* unused, const WCHAR* str,
                                      INT count, const void* rect, DWORD format,
                                      DWORD color)
{
    if (g_storyActive && IsStoryDraw(str, count, format, (const LONG*)rect)) {
        Cs();
        EnterCriticalSection(&g_cs);
        TextOnDraw(str, format, rect ? (unsigned)((const LONG*)rect)[2] : 0,
                   0, color, count);
        LeaveCriticalSection(&g_cs);
    }
    return g_origDrawText(self, unused, str, count, rect, format, color);
}

// Called from the tick thread through TextOnDraw's data; see TextTick.
void TextOnDraw(const wchar_t* text, unsigned fmt, unsigned width, unsigned fontPtr,
                unsigned color, int count)
{
    (void)fmt; (void)width; (void)fontPtr; (void)color; (void)count;
    DWORD now = GetTickCount();
    if (wcscmp(g_curRaw, text)) {
        // A typewriter effect extends the previous string, a new line does not.
        size_t oldLen = wcslen(g_curRaw);
        int extends = (oldLen > 0) && !wcsncmp(g_curRaw, text, oldLen);
        if (!extends) {
            Audio_OnLineStarted();
        }
        wcsncpy_s(g_curRaw, LINE_LEN, text, _TRUNCATE);
        g_curRawTick = now;
    }
}

static void EmitLines(const wchar_t* text, DWORD now)
{
    if (!text || !*text) { return; }
    if (!wcscmp(text, g_lastEmitted)) { return; }
    wcsncpy_s(g_lastEmitted, LINE_LEN, text, _TRUNCATE);
    g_lastEmitTick = now;

    // find a free pending slot
    for (int i = 0; i < PENDING_MAX; i++) {
        if (!g_pending[i].used) {
            wchar_t buf[LINE_LEN];
            wcsncpy_s(buf, LINE_LEN, text, _TRUNCATE);
            Normalize(buf);
            wcsncpy_s(g_pending[i].text, LINE_LEN, buf, _TRUNCATE);
            g_pending[i].appeared = now;
            g_pending[i].used = 1;
            g_pending[i].evaluated = 0;
            Audio_OnLineLaunched();
            Logf(L"line drawn: \"%s\"", buf);
            return;
        }
    }
    Logf(L"pending queue full, dropping \"%s\"", text);
}

void TextTick(DWORD now)
{
    Cs();
    EnterCriticalSection(&g_cs);
    int full = 0;
    wchar_t snap[LINE_LEN];
    snap[0] = 0;
    DWORD snapTick = g_curRawTick;
    if (g_curRaw[0]) {
        wcsncpy_s(snap, LINE_LEN, g_curRaw, _TRUNCATE);
        full = 1;
    }
    LeaveCriticalSection(&g_cs);

    if (full && (DWORD)(now - snapTick) >= (DWORD)g_cfg.minStableMs) {
        EmitLines(snap, snapTick);
    }
    EvaluatePending(now);
}

// ---- vtable patching ------------------------------------------------------
void FontHookSetOriginal(void* origDrawText)
{
    g_origDrawText = (DrawTextWFn)origDrawText;
}

// Called by the proxy for every font the engine creates.
void FontHookOnCreated(void* font)
{
    if (!font) { return; }
    void** vt = *(void***)font;
    if (!vt) { return; }
    Cs();
    EnterCriticalSection(&g_cs);
    if (!g_vtableReady) {
        // copy 32 slots; the object's vtable lives in d3dx9_40_orig
        void** copy = (void**)LocalAlloc(LPTR, 32 * sizeof(void*));
        if (copy) {
            for (int i = 0; i < 32; i++) { copy[i] = vt[i]; }
            g_origVtable = vt;
            g_origDrawText = (DrawTextWFn)vt[15];
            copy[15] = (void*)HookDrawTextW;
            g_ourVtable = copy;
            g_vtableReady = 1;
            Logf(L"font vtable %p captured, DrawTextW=%p -> our hook", g_origVtable,
                 (void*)g_origDrawText);
        }
    }
    if (g_vtableReady) {
        *(void***)font = (void**)g_ourVtable;
    }
    LeaveCriticalSection(&g_cs);
}
