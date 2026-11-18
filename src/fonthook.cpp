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
static wchar_t g_lastFull[LINE_LEN];    // last whole string drawn in the story box
static DWORD g_lastEmitTick;
static int g_storyActive;
static int g_dropped;                   // draws we could not queue (diagnostics)
static int g_lineStarted;               // this line already reported to the audio judge
static DWORD g_lastDrawTick;             // last draw of any kind, for line epochs

static void Cs(void)
{
    if (!g_csReady) {
        InitializeCriticalSection(&g_cs);
        g_csReady = 1;
    }
}

// Replace line breaks with a space and tidy up, so the speech backend reads
// natural sentences instead of punctuation soup.
static void Normalize(wchar_t* s)
{
    wchar_t* w = s;
    for (const wchar_t* r = s; *r; r++) {
        if (*r == L'\r' || *r == L'\n' || *r == L'|' || *r == L'\t') {
            if (w > s && w[-1] != L' ') { *w++ = L' '; }
            continue;
        }
        *w++ = *r;
    }
    *w = 0;
    // collapse runs of spaces
    w = s;
    for (const wchar_t* r = s; *r; r++) {
        if (*r == L' ' && w > s && w[-1] == L' ') { continue; }
        *w++ = *r;
    }
    *w = 0;
    // drop a comma that ended up right after a full stop ("。，")
    wchar_t* p;
    while ((p = wcsstr(s, L"。，")) != 0) {
        memmove(p + 1, p + 2, (wcslen(p + 2) + 1) * sizeof(wchar_t));
    }
    while ((p = wcsstr(s, L"，，")) != 0) {
        memmove(p, p + 1, (wcslen(p + 1) + 1) * sizeof(wchar_t));
    }
    // trim
    p = s;
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
    // The dialogue box is drawn with WORDBREAK (0x10) on top of NOPREFIX (0x400);
    // the modal question boxes use plain 0x400 and are handled separately.
    if ((format & 0x410) != 0x410) { return 0; }
    // A positive but narrow box is a choice button or a settings widget.  Wide
    // boxes (990) are the story text box; after a save is loaded the engine
    // passes a layout box whose width is negative (-82 observed), which is still
    // story text, so only "narrow and positive" is rejected here.
    if (box && box[2] > 0 && box[2] < 600) { return 0; }
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
        p->used = 0;                    // release the slot: the ring must recycle
        Logf(L"line ready: voiced=%d \"%s\"", voiced, p->text);
        RepeatPush(p->text);
        // While a menu is open the game keeps redrawing the story box behind it;
        // those lines were already read and are not what the player is looking at.
        if (MenuScreen() != 1) {   // 1 = SCR_NOVEL, see menus.cpp
            Logf(L"line arrived while on screen %d -> not read", MenuScreen());
            continue;
        }
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

// ---- recent draws ---------------------------------------------------------
// Every draw reaches the hook, but only story lines go to the speech pipeline.
// Settings values are painted as ordinary text next to their row, so the last
// few draws are kept here to answer "what does this row say?".
#define DRAW_MAX 64
#define DRAW_TEXT 128
struct DrawRec {
    int x, y, w, h;
    DWORD fmt;
    DWORD tick;
    wchar_t text[DRAW_TEXT];
    int used;
};
static DrawRec g_draws[DRAW_MAX];
static int g_drawHead;

static void RecordDraw(const WCHAR* str, INT count, const LONG* box, DWORD fmt, DWORD now)
{
    if (!str || !*str || count == 1 || !box) { return; }
    Cs();
    EnterCriticalSection(&g_cs);
    DrawRec* d = &g_draws[g_drawHead % DRAW_MAX];
    g_drawHead++;
    // The engine hands DrawTextW a RECT (left, top, right, bottom).  Some calls
    // pass a degenerate 1x1 box and place the text themselves, so keep both the
    // origin and the real extent: every filter below talks about width/height.
    d->x = box[0];
    d->y = box[1];
    d->w = (box[2] > box[0]) ? (box[2] - box[0]) : 0;
    d->h = (box[3] > box[1]) ? (box[3] - box[1]) : 0;
    d->fmt = fmt;
    d->tick = now;
    d->used = 1;
    wcsncpy_s(d->text, DRAW_TEXT, str, _TRUNCATE);
    LeaveCriticalSection(&g_cs);
}

int TextValueNear(int y, int h, wchar_t* out, int cap)
{    if (!out || cap <= 0) { return 0; }
    out[0] = 0;
    Cs();
    EnterCriticalSection(&g_cs);
    int found = 0;
    // newest first; a row value sits on the same horizontal band as its row
    for (int i = 0; i < DRAW_MAX && !found; i++) {
        int idx = (g_drawHead - 1 - i) % DRAW_MAX;
        if (idx < 0) { idx += DRAW_MAX; }
        DrawRec* d = &g_draws[idx];
        if (!d->used || !d->text[0]) { continue; }
        if (d->y + d->h / 2 < y - 12 || d->y + d->h / 2 > y + h + 12) { continue; }
        wcsncpy_s(out, (size_t)cap, d->text, _TRUNCATE);
        found = 1;
    }
    LeaveCriticalSection(&g_cs);
    return found;
}

// Text of a modal confirmation box, if one is on screen right now.
//
// The game's boxes (read/save/delete/quit) are one multi-line string drawn with
// format 0x400 and a zero box ("10月12日 主人公\n2026.11.18 20:39:01\n读取进度吗？").
// The 是 / 否 buttons underneath are single characters, so they arrive as
// count == 1 typewriter draws and never reach the draw record -- the question
// line itself is the signature.  Story text is 0x410 with a 990 wide box, so the
// two cannot be confused.
int TextDialogLine(wchar_t* out, int cap)
{
    if (!out || cap <= 0) { return 0; }
    out[0] = 0;
    Cs();
    EnterCriticalSection(&g_cs);
    int found = 0;
    DWORD now = GetTickCount();
    for (int i = 0; i < DRAW_MAX && !found; i++) {
        int idx = (g_drawHead - 1 - i) % DRAW_MAX;
        if (idx < 0) { idx += DRAW_MAX; }
        DrawRec* d = &g_draws[idx];
        if (!d->used || !d->text[0]) { continue; }
        if ((DWORD)(now - d->tick) > 400) { continue; }          // only a box that is on screen now
        if (!(d->fmt & 0x400) || (d->fmt & 0x10)) { continue; }   // 0x400, not 0x410
        if (d->x != 0 || d->y != 0 || d->w != 0 || d->h != 0) { continue; }
        const wchar_t* q = wcsrchr(d->text, L'\n');
        if (!q || !q[1]) { continue; }
        q++;
        size_t n = wcslen(q);
        if (n < 3 || n > 120) { continue; }
        if (q[n - 1] != L'？' && q[n - 1] != L'?') { continue; }
        wcsncpy_s(out, (size_t)cap, q, _TRUNCATE);
        found = 1;
    }
    LeaveCriticalSection(&g_cs);
    return found;
}

// ---- choices --------------------------------------------------------------
// The first branch of the story is drawn as two full width bars, each with one
// line of text (measured: text boxes 174x28 and 140x28 at y 156 / 295, hover
// lights the whole bar, a click decides the branch immediately).  They are
// ordinary narrow draws, so they are recognised from the draw record: narrow
// box, one text line, below the top bar and above the pause icons.
int TextChoices(ChoiceItem* out, int cap)
{
    if (!out || cap <= 0) { return 0; }
    Cs();
    EnterCriticalSection(&g_cs);
    DWORD now = GetTickCount();
    int n = 0;
    for (int i = 0; i < DRAW_MAX && n < cap; i++) {
        int idx = (g_drawHead - 1 - i) % DRAW_MAX;
        if (idx < 0) { idx += DRAW_MAX; }
        DrawRec* d = &g_draws[idx];
        if (!d->used || !d->text[0]) { continue; }
        // Choice buttons are painted by the engine as ordinary UI text: the
        // WO rDBREAK dialogue flags are absent, and the box is the 1x1 point at
        // the text origin (measured: "插进去看看。" at 552,154 and
        // "不插进去。" at 569,293, both fmt=0x100).
        if (!(d->fmt & 0x100)) { continue; }
        if (d->w > 2 || d->h > 2) { continue; }        // point box, layout done by the engine
        if ((DWORD)(now - d->tick) > 3000) { continue; } // stale: a previous scene
        if (d->x < 460 || d->x > 820) { continue; }    // button text column
        if (d->y < 130 || d->y > 360) { continue; }    // the two choice rows
        size_t len = wcslen(d->text);
        if (len < 2 || len > 60) { continue; }
        // Slot labels and page counters live in the same corner of the screen;
        // they always contain digits or a time stamp, prose does not.
        int digits = 0;
        for (size_t k = 0; k < len; k++) {
            wchar_t ch = d->text[k];
            if ((ch >= L'0' && ch <= L'9') || ch == L':' || ch == L'.' || ch == L'/') { digits = 1; break; }
        }
        if (digits) { continue; }
        int dup = 0;
        for (int j = 0; j < n; j++) {
            if (!wcscmp(out[j].text, d->text)) { dup = 1; break; }
        }
        if (dup) { continue; }
        // Two buttons are drawn within the same frame; a lone label is not a choice.
        int sameframe = 0;
        for (int j = 0; j < n; j++) {
            DWORD dt = (out[j].tick > d->tick) ? (out[j].tick - d->tick) : (d->tick - out[j].tick);
            if (dt < 1500) { sameframe = 1; break; }
        }
        if (n > 0 && !sameframe) { continue; }
        out[n].x = d->x;
        out[n].y = d->y;
        out[n].w = d->w;
        out[n].h = d->h;
        out[n].tick = d->tick;
        wcsncpy_s(out[n].text, CHOICE_LEN, d->text, _TRUNCATE);
        n++;
    }
    LeaveCriticalSection(&g_cs);
    // top to bottom, the order on screen
    for (int i = 1; i < n; i++) {
        ChoiceItem key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].y > key.y) { out[j + 1] = out[j]; j--; }
        out[j + 1] = key;
    }
    return n;
}
int TextStoryRecent(unsigned maxAgeMs)
{
    Cs();
    EnterCriticalSection(&g_cs);
    DWORD now = GetTickCount();
    int found = 0;
    for (int i = 0; i < DRAW_MAX && !found; i++) {
        int idx = (g_drawHead - 1 - i) % DRAW_MAX;
        if (idx < 0) { idx += DRAW_MAX; }
        DrawRec* d = &g_draws[idx];
        if (!d->used || !d->text[0]) { continue; }
        if ((DWORD)(now - d->tick) > maxAgeMs) { continue; }
        if ((d->fmt & 0x410) != 0x410) { continue; }
        if (d->fmt & 0x100) { continue; }
        found = 1;
    }
    LeaveCriticalSection(&g_cs);
    return found;
}
// DrawTextW hook: capture, then draw exactly as before.
static HRESULT __stdcall HookDrawTextW(void* self, void* unused, const WCHAR* str,
                                      INT count, const void* rect, DWORD format,
                                      DWORD color)
{
    if (str && *str) {
        // A line begins with its very first glyph.  The typewriter draws one
        // character at a time (count == 1) and those draws are filtered out of
        // the line pipeline, but they are exactly the "voice should start now"
        // marker the audio judge needs: the clip plays while the text is still
        // being typed, so reporting the line only when it is complete would put
        // the clip outside the observation window and misjudge a voiced line as
        // unvoiced.
        DWORD now0 = GetTickCount();
        if ((long)(now0 - g_lastDrawTick) > 600) {
            if (!g_lineStarted) { Audio_OnLineStarted(); g_lineStarted = 1; }
        }
        g_lastDrawTick = now0;
    }
    if (str && *str && count != 1) {
        RecordDraw(str, count, (const LONG*)rect, format, GetTickCount());
        if (g_cfg.logDraws) {
            // The idle UI redraws a handful of strings forever, so a plain
            // "changed since last time" check still floods the log.  Remember a
            // small set of recently logged strings instead.
            static wchar_t s_logRing[24][LINE_LEN];
            static int s_logIdx;
            static int s_seen;
            int known = 0;
            for (int i = 0; i < 24 && !known; i++) {
                if (!wcscmp(s_logRing[i], str)) { known = 1; }
            }
            if (!known && s_seen < 20000) {
                s_seen++;
                wcsncpy_s(s_logRing[s_logIdx % 24], LINE_LEN, str, _TRUNCATE);
                s_logIdx++;
                const LONG* b = (const LONG*)rect;
                const wchar_t* tag = L"";
                if (IsStoryDraw(str, count, format, (const LONG*)rect)) { tag = L" [story]"; }
                else if ((format & 0x100) && b && b[2] >= 100 && wcslen(str) <= 10) { tag = L" [name]"; }
                Logf(L"DRAW%s cnt=%d fmt=0x%X x=%ld y=%ld w=%ld h=%ld \"%s\"",
                     tag, count, format, b ? b[0] : -1, b ? b[1] : -1, b ? b[2] : -1,
                     b ? b[3] : -1, str);
            }
        }
    }
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
        if (!extends && !g_lineStarted) {
            // Voice clips start while the line is still typing, so the audio
            // judge has to be told the moment a new line appears, not when it
            // is complete.  One start per line, never two.
            Audio_OnLineStarted();
            g_lineStarted = 1;
        }
        wcsncpy_s(g_curRaw, LINE_LEN, text, _TRUNCATE);
        g_curRawTick = now;
    }
}

static void EmitLines(const wchar_t* text, DWORD now)
{
    if (!text || !*text) { return; }
    if (!wcscmp(text, g_lastEmitted)) { return; }

    // The engine redraws the same complete line many times (once per frame while
    // the box fades in, once per keystroke, ...).  Comparing with the previous
    // string alone is not enough: one intermediate draw in between is enough to
    // make the line look new again and it gets spoken a second time.
    {
        static wchar_t s_emitRing[32][LINE_LEN];
        static DWORD   s_emitTick[32];
        static int     s_emitIdx;
        for (int i = 0; i < 32; i++) {
            if (s_emitTick[i] && !wcscmp(s_emitRing[i], text) &&
                (DWORD)(now - s_emitTick[i]) < 120000) {
                return;
            }
        }
        wcsncpy_s(s_emitRing[s_emitIdx % 32], LINE_LEN, text, _TRUNCATE);
        s_emitTick[s_emitIdx % 32] = now;
        s_emitIdx++;
    }

    // A text box that grows while the player advances (the rules page appends one
    // paragraph per click) redraws the whole string; only the new tail is news.
    const wchar_t* fresh = text;
    size_t haveLen = wcslen(g_lastFull);
    if (haveLen && haveLen < wcslen(text) && !wcsncmp(g_lastFull, text, haveLen)) {
        fresh = text + haveLen;
        while (*fresh == L' ' || *fresh == L'，' || *fresh == L'。') { fresh++; }
        if (!*fresh) { wcsncpy_s(g_lastEmitted, LINE_LEN, text, _TRUNCATE); return; }
    }
    wcsncpy_s(g_lastFull, LINE_LEN, text, _TRUNCATE);
    wcsncpy_s(g_lastEmitted, LINE_LEN, text, _TRUNCATE);
    g_lastEmitTick = now;

    // find a free pending slot
    for (int i = 0; i < PENDING_MAX; i++) {
        if (!g_pending[i].used) {
            wchar_t buf[LINE_LEN];
            wcsncpy_s(buf, LINE_LEN, fresh, _TRUNCATE);
            Normalize(buf);
            if (!buf[0]) { return; }
            // The typewriter reveal can pause in the middle of a latin token and
            // get flushed as a fragment ("P…I…"); the finished token arrives with
            // the next draw, so a short ASCII-only fragment is never news.
            {
                int latin = 1, alnum = 0;
                for (const wchar_t* q = buf; *q; ++q) {
                    if (*q == 0x2026 || *q == L' ') { continue; }   // "…"
                    if ((*q >= L'0' && *q <= L'9') || (*q >= L'a' && *q <= L'z') ||
                        (*q >= L'A' && *q <= L'Z')) { alnum++; continue; }
                    latin = 0;
                    break;
                }
                if (latin && alnum < 4) {
                    Logf(L"line fragment skipped: \"%s\"", buf);
                    return;
                }
            }
            // "「………」" and the like carry no information for a listener; reading
            // them just interrupts the previous sentence.
            {
                int meaningful = 0;
                for (const wchar_t* q = buf; *q; ++q) {
                    if (*q == 0x2026 || *q == L'…' || *q == L' ' || *q == L'　' ||
                        *q == L'。' || *q == L'，' || *q == L'、' || *q == L'!' ||
                        *q == L'?' || *q == L'！' || *q == L'？' || *q == L'「' ||
                        *q == L'」' || *q == L'『' || *q == L'』' || *q == L'.' ||
                        *q == L',' || *q == L'～' || *q == L'~' || *q == L'-' ||
                        *q == L'—' || *q == L'…') { continue; }
                    meaningful = 1;
                    break;
                }
                if (!meaningful) {
                    Logf(L"line punctuation only, skipped: \"%s\"", buf);
                    return;
                }
            }
            wcsncpy_s(g_pending[i].text, LINE_LEN, buf, _TRUNCATE);
            g_pending[i].appeared = now;
            g_pending[i].used = 1;
            g_pending[i].evaluated = 0;
            // One line start per launched line: the audio judge tells voice
            // clips apart by counting the lines the text layer reports.
            if (!g_lineStarted) { Audio_OnLineStarted(); }
            g_lineStarted = 0;
            Audio_OnLineLaunched();
            Logf(L"line drawn: \"%s\"", buf);
            return;
        }
    }
    g_dropped++;
    Logf(L"pending queue full (%d), dropping \"%s\"", g_dropped, text);
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
