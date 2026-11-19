// menus.cpp -- keyboard access for a mouse-only UI.
//
// Measured fact (recon-keys, 2026-11-18): the title screen, pause menu, settings,
// save/load, backlog, extras and every confirmation box ignore the keyboard
// completely -- only the story screen reacts to Enter/Space/Escape/Z/wheel.
// There is no game-side focus model to hook, so the patch emulates the mouse on
// the game's own rectangles: arrow keys move a virtual cursor, Enter performs the
// real hover + click the user would otherwise have to do by hand.  The player
// never sees a new widget -- only the game's own hover highlight moves.
//
// Rectangles are client coordinates of the 1280x720 game window and were measured
// from screenshots (see docs/交互形态判定.md for the method and its limits).
#include "a11y.h"
#include "inputhook.h"

#include <string.h>

enum {
    SCR_TITLE = 0,
    SCR_NOVEL,
    SCR_PAUSE,
    SCR_SETTINGS,
    SCR_EXTRAS,
    SCR_SAVE,
    SCR_LOAD,
    SCR_CONFIRM,
    SCR_BACKLOG,
    SCR_HELP,
    SCR_CHOICE,
    SCR_COUNT
};

enum { K_CLICK = 0, K_SLIDER, K_TOGGLE, K_SLOT };

struct MenuItem {
    const wchar_t* label;
    int x, y, w, h;
    int kind;
    int gotoScreen;   // -1 = stay
    int risky;        // 1 = needs a second Enter to confirm
};

// ---- title screen (measured 1073..1152 x, rows 394/478/561/644) -----------
static const MenuItem kTitle[] = {
    { L"开始", 1073, 394, 79, 37, K_CLICK, SCR_NOVEL, 0 },
    { L"特典", 1074, 478, 78, 37, K_CLICK, SCR_EXTRAS, 0 },
    { L"读取", 1073, 561, 80, 36, K_CLICK, SCR_LOAD, 0 },
    { L"选项", 1072, 644, 80, 36, K_CLICK, SCR_SETTINGS, 0 },
    { L"操作说明", 1200, 113, 52, 52, K_CLICK, -1, 0 },
    { L"退出游戏", 1200, 28, 52, 52, K_CLICK, -1, 1 },
};

// ---- pause menu (one row of icons at y 627..712) --------------------------
static const MenuItem kPause[] = {
    { L"帮助", 98, 631, 37, 81, K_CLICK, SCR_HELP, 0 },
    { L"跳过", 259, 631, 39, 80, K_CLICK, SCR_NOVEL, 0 },
    { L"历史", 431, 631, 41, 80, K_CLICK, SCR_BACKLOG, 0 },
    { L"保存", 609, 631, 48, 81, K_CLICK, SCR_SAVE, 0 },
    { L"读取", 783, 631, 38, 81, K_CLICK, SCR_LOAD, 0 },
    { L"选项", 963, 631, 42, 80, K_CLICK, SCR_SETTINGS, 0 },
    { L"标题", 1139, 627, 62, 84, K_CLICK, SCR_TITLE, 1 },
    { L"返回游戏", 1203, 32, 35, 38, K_CLICK, SCR_NOVEL, 0 },
};

// ---- settings (sliders 526 px wide, language and fullscreen pairs) --------
static const MenuItem kSettings[] = {
    { L"文字速度", 558, 121, 526, 24, K_SLIDER, -1, 0 },
    { L"背景音乐", 558, 208, 526, 24, K_SLIDER, -1, 0 },
    { L"效果音", 558, 293, 526, 24, K_SLIDER, -1, 0 },
    { L"人物声音", 558, 379, 526, 24, K_SLIDER, -1, 0 },
    { L"文字窗口", 558, 537, 526, 24, K_TOGGLE, -1, 0 },
    { L"韩文", 554, 450, 211, 46, K_CLICK, -1, 1 },
    { L"日文", 871, 450, 210, 46, K_CLICK, -1, 1 },
    { L"全屏开", 554, 626, 211, 45, K_CLICK, -1, 0 },
    { L"全屏关", 871, 626, 210, 45, K_CLICK, -1, 0 },
    { L"返回", 1203, 32, 35, 38, K_CLICK, -2, 0 },   // -2 = previous screen
};

// ---- extras --------------------------------------------------------------
static const MenuItem kExtras[] = {
    { L"成就", 1072, 339, 70, 34, K_CLICK, -1, 0 },
    { L"相册", 1072, 414, 70, 34, K_CLICK, -1, 0 },
    { L"未解锁项目", 1072, 494, 112, 32, K_CLICK, -1, 0 },
    { L"未解锁项目", 1072, 574, 112, 32, K_CLICK, -1, 0 },
    { L"未解锁项目", 1072, 654, 112, 32, K_CLICK, -1, 0 },
    { L"返回", 1203, 32, 35, 38, K_CLICK, -2, 0 },
};

// ---- save / load (4 x 3 slot grid, 10 pages) -----------------------------
// Measured grid: column left edges 154/413/672/932 (width 191),
// row tops 125/302/479 (height 110).  The file a slot maps to is
// Saves\SaveNN.nkslot with NN = 20*(page-1) + 4*row + col, so page 1 slot 1 is
// Save00 and page 2 row 2 column 1 is Save24.
static const int kSlotCol[4] = { 154, 413, 672, 932 };
static const int kSlotRow[3] = { 125, 302, 479 };
static MenuItem kSaveLoad[15];
static int g_savePage = 1;
static int g_slotsReady;

static void BuildSaveLoad(void)
{
    if (g_slotsReady) { return; }
    int n = 0;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            MenuItem* it = &kSaveLoad[n++];
            it->label = L"存档格";
            it->x = kSlotCol[c];
            it->y = kSlotRow[r];
            it->w = 191;
            it->h = 110;
            it->kind = K_SLOT;
            it->gotoScreen = -1;
            it->risky = 0;
        }
    }
    struct { const wchar_t* l; int x, y, w, h; int g; } tail[3] = {
        { L"上一页", 1013, 648, 51, 38, -1 },
        { L"下一页", 1074, 648, 51, 38, -1 },
        { L"返回", 1203, 32, 35, 38, -2 },
    };
    for (int i = 0; i < 3; i++) {
        MenuItem* it = &kSaveLoad[n++];
        it->label = tail[i].l;
        it->x = tail[i].x; it->y = tail[i].y;
        it->w = tail[i].w; it->h = tail[i].h;
        it->kind = K_CLICK;
        it->gotoScreen = tail[i].g;
        it->risky = 0;
    }
    g_slotsReady = 1;
}

// ---- confirmation box ----------------------------------------------------
static const MenuItem kConfirm[] = {
    { L"是", 528, 361, 52, 40, K_CLICK, -2, 0 },
    { L"否", 694, 360, 51, 40, K_CLICK, -2, 0 },
};

// ---- backlog -------------------------------------------------------------
static const MenuItem kBacklog[] = {
    { L"返回", 1203, 32, 35, 38, K_CLICK, SCR_NOVEL, 0 },
};

// ---- help page (whole screen is one image) -------------------------------
static const MenuItem kHelp[] = {
    { L"操作说明页，按回车翻页，按退出键返回", 0, 0, 1280, 720, K_CLICK, -1, 0 },
};

// ---- story choices (rectangles and wording come from the live draws) -----
static MenuItem kChoice[CHOICE_MAX];
static wchar_t kChoiceText[CHOICE_MAX][CHOICE_LEN];
static int g_choiceCount;

struct ScreenDef {
    const MenuItem* items;
    int count;
};

static ScreenDef g_choiceScreen = { kChoice, 0 };

static const ScreenDef g_screens[SCR_COUNT] = {
    { kTitle, (int)(sizeof(kTitle) / sizeof(kTitle[0])) },
    { 0, 0 },                                                   // novel: no menu
    { kPause, (int)(sizeof(kPause) / sizeof(kPause[0])) },
    { kSettings, (int)(sizeof(kSettings) / sizeof(kSettings[0])) },
    { kExtras, (int)(sizeof(kExtras) / sizeof(kExtras[0])) },
    { kSaveLoad, 15 },
    { kSaveLoad, 15 },
    { kConfirm, (int)(sizeof(kConfirm) / sizeof(kConfirm[0])) },
    { kBacklog, (int)(sizeof(kBacklog) / sizeof(kBacklog[0])) },
    { kHelp, (int)(sizeof(kHelp) / sizeof(kHelp[0])) },
    { 0, 0 },                                                   // choice: built at runtime
};

static HWND g_wnd;

// Menu rectangles were measured on the game's default 1280x720 client area.
// MenuInit records the real client size; click points are scaled from that
// design space so a resized window cannot make us click a neighbouring item.
static int g_cliW = 1280;
static int g_cliH = 720;
static int g_screen = SCR_TITLE;
static int g_prev = SCR_TITLE;
static int g_sel;
static int g_consumed;
static int g_pendingScreen = -1;
static DWORD g_pendingScreenTick;
static int g_confirmPending = -1;    // risky item waiting for its second Enter
static int g_suppressName;           // do not speak the screen name (caller will)
static DWORD g_slotWaitUntil;        // a slot click that produced no dialog
static int g_slotWasSave;
static DWORD g_choiceMuteUntil;      // do not re-detect choices right after a click
static DWORD g_dlgMuteUntil;         // do not re-detect a box that is closing
static int g_saveLoadMode = SCR_SAVE; // which of the two we opened (page counter cannot tell)
static DWORD g_pageSeenAt;           // last time the "N/10" counter was on screen
static DWORD s_screenAt;             // when we entered the current screen

// Build the choice screen from the live draw record.  The bars stay on screen
// while the branch is being resolved, so the list is built once and then kept
// until the bars disappear.
static void FillChoiceScreen(const ChoiceItem* items, int n)
{
    if (n > CHOICE_MAX) { n = CHOICE_MAX; }
    for (int i = 0; i < n; i++) {
        MenuItem* it = &kChoice[i];
        wcsncpy_s(kChoiceText[i], CHOICE_LEN, items[i].text, _TRUNCATE);
        it->label = kChoiceText[i];
        it->x = items[i].x;
        it->y = items[i].y;
        it->w = items[i].w;
        it->h = items[i].h;
        it->kind = K_CLICK;
        it->gotoScreen = -1;
        it->risky = 0;
        // Options belong to the re-readable text, exactly like story lines.
        RepeatPush(items[i].text);
        Logf(L"choice %d: rect=%d,%d,%d,%d \"%s\"", i + 1, it->x, it->y, it->w, it->h,
             it->label);
    }
    g_choiceCount = n;
}

// slider fractions: remember per item so left/right keeps moving one way
static int g_sliderFrac[16];

// click state machine

static const ScreenDef* Cur(void)
{
    if (g_screen == SCR_CHOICE) {
        g_choiceScreen.count = g_choiceCount;
        return &g_choiceScreen;
    }
    return &g_screens[g_screen];
}

static void ToScreen(int cx, int cy, POINT* out)
{
    out->x = cx;
    out->y = cy;
    if (g_wnd) { ClientToScreen(g_wnd, out); }
}

// A D3D window that is not foreground drops synthetic mouse input: the click
// would land on whatever window is actually in front.  The patch may be driven
// from a screen reader command or from a script, so never assume the game is
// active -- take the foreground first.
static void EnsureForeground(void)
{
    HWND fg = GetForegroundWindow();
    if (!g_wnd) { return; }
    // Called unconditionally on purpose: an already-foreground window still has
    // to be re-activated before the engine accepts the click (the external
    // helper that does work calls SetForegroundWindow every time).
    SetForegroundWindow(g_wnd);
    Sleep(120);
    fg = GetForegroundWindow();
    fg = GetForegroundWindow();
    if (fg != g_wnd) {
        // The foreground thread owns the focus queue; share it for a moment.
        DWORD fgThread = fg ? GetWindowThreadProcessId(fg, 0) : 0;
        DWORD myThread = GetCurrentThreadId();
        if (fgThread && fgThread != myThread) {
            AttachThreadInput(myThread, fgThread, TRUE);
            SetForegroundWindow(g_wnd);
            AttachThreadInput(myThread, fgThread, FALSE);
            Sleep(120);
        }
    }
    Logf(L"menu: foreground game=%p now=%p", (void*)g_wnd, (void*)GetForegroundWindow());
}

// The engine takes the pointer position from WM_MOUSEMOVE and only counts a click
// when the button stays down for well over a second (measured: 700 ms ignored,
// 2000 ms accepted).  That sequence takes ~2.2 s, far longer than a low level
// keyboard hook may block, so inputhook.cpp plays it out on the tick thread and
// the click is queued here.
static void ClickOnce(POINT screenPt)
{
    if (g_wnd && InputHookReady()) {
        POINT cp = screenPt;
        ScreenToClient(g_wnd, &cp);
        // design space (1280x720) -> real client pixels, measured now
        RECT rcNow;
        if (GetClientRect(g_wnd, &rcNow) && rcNow.right > 0 && rcNow.bottom > 0) {
            g_cliW = rcNow.right;
            g_cliH = rcNow.bottom;
        }
        if (g_cliW != 1280 || g_cliH != 720) {
            cp.x = (LONG)((long)cp.x * g_cliW / 1280);
            cp.y = (LONG)((long)cp.y * g_cliH / 720);
        }
        if (InputClickAt(cp.x, cp.y)) {
            Logf(L"menu: click queued client=%ld,%ld", cp.x, cp.y);
        } else {
            Logf(L"menu: click queue full, click dropped");
        }
        return;
    }
    // Fallback when the engine import patch failed: real hardware input.  This
    // path is known to be ignored by the title menu, but leaving it in means a
    // broken patch degrades instead of doing nothing at all.
    EnsureForeground();
    SetCursorPos(screenPt.x, screenPt.y);
    Sleep(120);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(60);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    Logf(L"menu: clicked (hardware) at %ld,%ld", screenPt.x, screenPt.y);
}

static void ClickAt(POINT screenPt, int twice)
{
    ClickOnce(screenPt);
    if (twice) {
        // Queued back to back with a gap; the first click of a pair is often
        // swallowed by the engine, so the caller asks for two.
        ClickOnce(screenPt);
        Logf(L"menu: double click queued (the first one is often swallowed)");
    }
}

static void SpeakMenu(const wchar_t* text)
{
    // Menu feedback must not pollute the re-read buffer (story lines only).
    // Menu feedback is an interruption by design: it must not queue behind a
    // long story line the player is currently listening to.
    Logf(L"menu: announce \"%s\"", text);
    Speech_SpeakImmediate(text);
}

static void Announce(int idx)
{
    const ScreenDef* s = Cur();
    if (!s->items || idx < 0 || idx >= s->count) { return; }
    const MenuItem* it = &s->items[idx];
    wchar_t buf[192];
    if (it->kind == K_SLOT) {
        int row = idx / 4, col = idx % 4;
        int slotNo = 20 * (g_savePage - 1) + 4 * row + col;
        wsprintfW(buf, L"第%d页，第%d行第%d列，存档位%d，%d之%d",
                  g_savePage, row + 1, col + 1, slotNo, idx + 1, s->count);
    } else if (it->kind == K_SLIDER) {
        wchar_t val[128];
        if (TextValueNear(it->y, it->h, val, 128)) {
            wsprintfW(buf, L"%s，%s，%d之%d，左右键调整", it->label, val, idx + 1, s->count);
        } else {
            wsprintfW(buf, L"%s，%d之%d，左右键调整", it->label, idx + 1, s->count);
        }
    } else if (it->kind == K_TOGGLE) {
        wchar_t val[128];
        if (TextValueNear(it->y, it->h, val, 128)) {
            wsprintfW(buf, L"%s，%s，%d之%d", it->label, val, idx + 1, s->count);
        } else {
            wsprintfW(buf, L"%s，%d之%d", it->label, idx + 1, s->count);
        }
    } else {
        wsprintfW(buf, L"%s，%d之%d", it->label, idx + 1, s->count);
    }
    SpeakMenu(buf);
}

// Value of a setting row: the game paints it as text; ask the text layer.
static void AnnounceValue(int idx)
{
    const ScreenDef* s = Cur();
    if (!s->items) { return; }
    const MenuItem* it = &s->items[idx];
    if (it->kind != K_SLIDER && it->kind != K_TOGGLE) { return; }
    wchar_t val[128];
    if (TextValueNear(it->y, it->h, val, 128)) {
        wchar_t buf[192];
        wsprintfW(buf, L"%s", val);
        SpeakMenu(buf);
    }
}

static void Activate(void)
{
    const ScreenDef* s = Cur();
    if (!s->items || g_sel < 0 || g_sel >= s->count) { return; }
    const MenuItem* it = &s->items[g_sel];

    if (g_screen == SCR_CHOICE) {
        // A branch click is final: no second click (it would land on whatever
        // the game shows next).  If the first one is swallowed the bars are
        // still there, so the player just presses Enter again.
        POINT cpt;
        ToScreen(it->x + it->w / 2, it->y + it->h / 2, &cpt);
        g_choiceMuteUntil = GetTickCount() + 2500;
        Logf(L"menu: choice %d \"%s\" -> click %ld,%ld", g_sel + 1, it->label,
             it->x + it->w / 2, it->y + it->h / 2);
        ClickAt(cpt, 0);
        return;
    }

    if (it->risky && g_confirmPending != g_sel) {
        g_confirmPending = g_sel;
        wchar_t buf[192];
        wsprintfW(buf, L"%s，再按一次回车确认", it->label);
        SpeakMenu(buf);
        Logf(L"menu: risky item \"%s\" armed", it->label);
        return;
    }
    g_confirmPending = -1;

    POINT pt;
    if (it->kind == K_SLOT) {
        ToScreen(it->x + it->w / 2, it->y + it->h / 2, &pt);
        int row = g_sel / 4, col = g_sel % 4;
        int slotNo = 20 * (g_savePage - 1) + 4 * row + col;
        Logf(L"menu: %s slot page=%d row=%d col=%d file=Save%02d.nkslot",
             g_screen == SCR_SAVE ? L"save" : L"load", g_savePage, row + 1, col + 1,
             slotNo);
        g_slotWaitUntil = GetTickCount() + 6000;  // synthetic click holds 2000 ms, dialog needs ~2.5 s
        g_slotWasSave = (g_screen == SCR_SAVE);
        ClickAt(pt, 0);
        return;
    }
    if (it->kind == K_SLIDER) {
        // Sliders have no keyboard: a click at a fraction of the track sets the
        // value there, so left/right walks the track in small steps.
        int frac = g_sliderFrac[g_sel % 16];
        if (frac <= 0) { frac = 50; }
        ToScreen(it->x + it->w * frac / 100, it->y + it->h / 2, &pt);
    } else {
        ToScreen(it->x + it->w / 2, it->y + it->h / 2, &pt);
    }
    // page arrows on the save/load screens
    if (g_screen == SCR_SAVE || g_screen == SCR_LOAD) {
        if (it->label[0] == L'上' || it->label[0] == L'下') {
            int page = g_savePage + (it->label[0] == L'上' ? -1 : 1);
            if (page < 1) { page = 1; }
            if (page > 10) { page = 10; }
            if (page != g_savePage) {
                g_savePage = page;
                wchar_t buf[64];
                wsprintfW(buf, L"第%d页", g_savePage);
                SpeakMenu(buf);
            }
        }
    }
    Logf(L"menu: activate \"%s\" (screen %d) -> client %ld,%ld", it->label, g_screen,
         it->x + it->w / 2, it->y + it->h / 2);
    // A confirm button must be clicked exactly once: the game acts on the first
    // click, so a second one lands on whatever is behind the box (and reopening
    // the box was exactly what happened).  Title items keep the double click.
    int twice = (g_screen == SCR_TITLE) ? 1 : 0;
    if (g_screen == SCR_CONFIRM) {
        // the box lingers for a frame after the click; do not bounce back into it
        g_dlgMuteUntil = GetTickCount() + 3000;
        if (g_sel == 0 && g_prev == SCR_LOAD) {
            // "是" on the load box: the game reads the slot and goes back to the
            // story, so follow it once the click (2 s hold) and the load settle.
            g_pendingScreen = SCR_NOVEL;
            g_pendingScreenTick = GetTickCount() + 4000;
            Logf(L"menu: load confirmed, waiting for the story screen");
        }
    }
    ClickAt(pt, twice);

    if (it->gotoScreen >= 0) {
        g_prev = g_screen;
        if (it->gotoScreen == SCR_SAVE || it->gotoScreen == SCR_LOAD) {
            g_saveLoadMode = it->gotoScreen;
        }
        g_pendingScreen = it->gotoScreen;
        g_pendingScreenTick = GetTickCount() + 400;
    } else if (it->gotoScreen == -2) {
        g_pendingScreen = g_prev;
        g_pendingScreenTick = GetTickCount() + 400;
    } else if (g_screen == SCR_HELP) {
        // help page: the game advances it with Enter; that key is passed through
    }
}

// Adjust a slider or toggle with left/right. Returns 1 when handled.
static int Adjust(int dir)
{
    const ScreenDef* s = Cur();
    if (!s->items || g_sel < 0 || g_sel >= s->count) { return 0; }
    const MenuItem* it = &s->items[g_sel];
    if (it->kind != K_SLIDER) { return 0; }
    int frac = g_sliderFrac[g_sel % 16];
    if (frac <= 0) { frac = 50; }
    frac += dir * 6;
    if (frac < 0) { frac = 0; }
    if (frac > 100) { frac = 100; }
    g_sliderFrac[g_sel % 16] = frac;
    POINT pt;
    ToScreen(it->x + it->w * frac / 100, it->y + it->h / 2, &pt);
    Logf(L"menu: slider \"%s\" -> %d%%", it->label, frac);
    ClickAt(pt, 0);
    wchar_t buf[128];
    wsprintfW(buf, L"%s，百分之%d", it->label, frac);
    SpeakMenu(buf);
    return 1;
}

// Framework convention (docs/操作逻辑公约.md): Up/Down walk the controls, and
// Left/Right only adjust the focused control (a slider).  So "walk" is a single
// sequence in reading order -- top to bottom, then left to right -- which covers
// the vertical lists (title, pause) and the slot grids (save/load) alike.
static void MoveOrder(int dir)
{
    const ScreenDef* s = Cur();
    if (!s->items || s->count <= 1) { return; }
    if (s->count > 32) { return; }
    if (g_sel < 0 || g_sel >= s->count) { g_sel = 0; }
    int order[32];
    for (int i = 0; i < s->count; i++) { order[i] = i; }
    // insertion sort by (row band, x); the band keeps items of one row together
    // even when their y values differ by a few pixels
    for (int i = 1; i < s->count; i++) {
        int cur = order[i];
        int keyY = s->items[cur].y / 24;
        int keyX = s->items[cur].x;
        int j = i - 1;
        while (j >= 0) {
            int py = s->items[order[j]].y / 24;
            int px = s->items[order[j]].x;
            if (py < keyY || (py == keyY && px <= keyX)) { break; }
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = cur;
    }
    int pos = 0;
    for (int i = 0; i < s->count; i++) { if (order[i] == g_sel) { pos = i; break; } }
    pos += dir;
    if (pos < 0) { pos = 0; }
    if (pos >= s->count) { pos = s->count - 1; }
    if (order[pos] == g_sel) { return; }
    g_sel = order[pos];
    g_confirmPending = -1;
    Logf(L"menu: selected %d \"%s\"", g_sel, s->items[g_sel].label);
    Announce(g_sel);
    AnnounceValue(g_sel);
}

void MenuInit(HWND w)
{
    BuildSaveLoad();
    g_wnd = w;
    g_screen = SCR_TITLE;
    g_sel = 0;
    RECT rc;
    if (w && GetClientRect(w, &rc)) {
        Logf(L"menu: init, window=%p client=%ldx%ld (may still be the initial "
             L"size; click coordinates use the size at click time)", (void*)w, rc.right, rc.bottom);
    } else {
        Logf(L"menu: init, window=%p", (void*)w);
    }
    InputHookInit(w);
    InputHookSetHold(g_cfg.clickHoldMs);
}

void MenuSetGameWnd(HWND w) { g_wnd = w; }
int MenuScreen(void) { return g_screen; }
int MenuConsumedKey(void) { return g_consumed; }

static DWORD s_screenMuteUntil = 0;

void MenuSetScreen(int screen)
{
    if (screen < 0 || screen >= SCR_COUNT || screen == g_screen) { return; }
    Logf(L"menu: screen %d -> %d", g_screen, screen);
    if (screen != SCR_NOVEL && screen != SCR_PAUSE) { g_prev = g_screen; }
    g_screen = screen;
    g_sel = 0;
    g_confirmPending = -1;
    // Any click still queued belongs to the screen we are leaving.  Letting it
    // land on the next page is what made clicks look "uncontrolled" (a save slot
    // click reopening the confirm box every few seconds).
    InputClickFlush();
    g_pageSeenAt = GetTickCount();
    s_screenAt = g_pageSeenAt;
    // The engine fades a new page in for the better part of a second.  Keys that
    // arrive during the fade belong to nobody: they must not be interpreted as
    // input for a page the player cannot see yet (0.2.4.0 bug: a fast Enter on the
    // save page activated a slot while the page was still fading in).
    s_screenMuteUntil = GetTickCount() + 700;
    if (g_suppressName) { g_suppressName = 0; return; }
    switch (screen) {
    case SCR_PAUSE: SpeakMenu(L"暂停菜单"); break;
    case SCR_SETTINGS: SpeakMenu(L"选项"); break;
    case SCR_EXTRAS: SpeakMenu(L"特典"); break;
    case SCR_SAVE: SpeakMenu(L"保存"); break;
    case SCR_LOAD: SpeakMenu(L"读取"); break;
    case SCR_CONFIRM: SpeakMenu(L"确认"); break;
    case SCR_BACKLOG: SpeakMenu(L"历史"); break;
    case SCR_TITLE: SpeakMenu(L"标题画面"); break;
    case SCR_CHOICE: SpeakMenu(L"选项"); break;
    default: break;
    }
}

void MenuOnKey(int vk, int isDown)
{
    g_consumed = 0;

    // Auto-repeat: holding Enter must not click twice in a row.
    static int s_enterHeld, s_spaceHeld;
    if (vk == VK_RETURN) {
        int repeat = isDown && s_enterHeld;
        s_enterHeld = isDown;
        if (repeat) { return; }
    } else if (vk == VK_SPACE) {
        int repeat = isDown && s_spaceHeld;
        s_spaceHeld = isDown;
        if (repeat) { return; }
    }
    if (!isDown) { return; }
    // A synthetic click is still being played out, and the engine needs the button
    // held for ~2 s, so a second activation now would only fill the queue.  Story
    // and pass-through screens never click, so they keep working.  Navigation and
    // slider adjustment stay live: only activation has to wait for the click.
    if (InputClickBusy() && g_screen != SCR_NOVEL && g_screen != SCR_HELP &&
        g_screen != SCR_BACKLOG) {
        int nav = (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT ||
                   vk == VK_BACK || vk == VK_HOME || vk == VK_END);
        if (!nav) {
            Logf(L"menu: click in progress, key %d ignored", vk);
            return;
        }
    }
    // Page still fading in: swallow keys so nobody activates an invisible item.
    if (g_screen != SCR_NOVEL && g_screen != SCR_HELP && g_screen != SCR_BACKLOG &&
        (long)(GetTickCount() - s_screenMuteUntil) < 0) {
        Logf(L"menu: screen settling, key %d ignored", vk);
        return;
    }
    const ScreenDef* s = Cur();

    if (vk == VK_ESCAPE) {
        if (g_screen == SCR_CHOICE) {
            // Escape belongs to the game (it opens the pause menu); stop
            // re-detecting the bars that stay visible behind it.
            g_choiceMuteUntil = GetTickCount() + 3000;
            g_suppressName = 1;
            MenuSetScreen(SCR_NOVEL);
            return;
        }
        if (g_screen == SCR_CONFIRM) {
            // cancelling a modal box means clicking its 否 button
            g_sel = 1;
            Activate();
            MenuSetScreen(g_prev);
            return;
        }
        if (g_screen == SCR_NOVEL) {
            // the game opens its pause menu itself
            g_pendingScreen = SCR_PAUSE;
            g_pendingScreenTick = GetTickCount() + 350;
            return;
        }
        if (g_screen == SCR_PAUSE) {
            // Escape in the pause menu is the game's own "back to the story":
            // follow it deterministically instead of guessing from redraws.
            g_pendingScreen = SCR_NOVEL;
            g_pendingScreenTick = GetTickCount() + 350;
            return;
        }
        // every other screen closes with Escape as well, and the game always
        // lands on the pause menu from there (not "wherever we came from", which
        // may be a screen our state only guessed).
        g_pendingScreen = SCR_PAUSE;
        g_pendingScreenTick = GetTickCount() + 350;
        return;
    }

    if (g_screen == SCR_NOVEL) {
        return;   // story screen: the game owns Enter/Space/Z/wheel
    }
    if (g_screen == SCR_CHOICE) {
        // The branch bars have no keyboard, so arrows pick a bar and Enter runs
        // the click the player would have made with the mouse.  Nothing else is
        // swallowed: Space, Escape, wheel and skip still go to the game.
        if (vk == VK_UP)    { MoveOrder(-1); g_consumed = 1; return; }
        if (vk == VK_DOWN)  { MoveOrder(1);  g_consumed = 1; return; }
        if (vk == VK_BACK)  { Announce(g_sel); g_consumed = 1; return; }
        if (vk == VK_RETURN || vk == VK_SPACE) {
            Activate();
            g_consumed = 1;
            return;
        }
        return;
    }
    if (g_screen == SCR_HELP) {
        // the help page is the game's own image sequence: Enter/Space advance it
        if (vk == VK_RETURN || vk == VK_SPACE) { return; }
    }
    if (g_screen == SCR_BACKLOG) {
        // the backlog scrolls with the game's own keys; only our own keys are eaten
        return;
    }

    switch (vk) {
    case VK_UP:    MoveOrder(-1); g_consumed = 1; break;
    case VK_DOWN:  MoveOrder(1);  g_consumed = 1; break;
    case VK_LEFT:
        // Left/Right adjust the focused control only; they never move the focus
        // (Framework convention, and the player asked for exactly this).
        Adjust(-1);
        g_consumed = 1;
        break;
    case VK_RIGHT:
        Adjust(1);
        g_consumed = 1;
        break;
    case VK_BACK:
        // Backspace re-reads the focused control (story re-read is the same key,
        // handled outside the menu).
        Announce(g_sel);
        AnnounceValue(g_sel);
        g_consumed = 1;
        break;
    case VK_RETURN:
    case VK_SPACE:
        Activate();
        g_consumed = 1;
        break;
    case VK_HOME:
        if (s->items && s->count) { g_sel = 0; Announce(0); }
        g_consumed = 1;
        break;
    case VK_END:
        if (s->items && s->count) { g_sel = s->count - 1; Announce(g_sel); }
        g_consumed = 1;
        break;
    default:
        break;
    }
}

void MenuTick(DWORD now)
{
    // Advance the queued synthetic clicks (engine needs a long button hold, so
    // this cannot be done inline on the hook thread).
    InputTick();

    // Story choices: two or more narrow bars drawn under the dialogue box mean
    // the game is waiting for a mouse click on one of them.  Turning that into
    // an arrow-key list is the only way a keyboard-only player can branch.
    // Follow the game when it moves without us: a mouse click, the auto mode or
    // a loaded save all land on the story screen while our state still says
    // "title".  Story text on screen is the signal.
    if (g_screen == SCR_TITLE && TextStoryRecent(800)) {
        MenuSetScreen(SCR_NOVEL);
    }
    // Note: we deliberately do NOT follow "pause menu -> story" by watching for a
    // story redraw.  Measured 2026-11-19 (prod_logs/sv160443): opening the save
    // page redraws the dialogue box once, ~1.2 s after the pause menu came up, so
    // every redraw-based rule fired on a page transition and threw the state back
    // to the story screen.  Escape is a deterministic toggle instead (see the
    // VK_ESCAPE handling): story -> pause, pause -> story, any sub-page -> pause.
    // Desync guard: if we think we are in the story but the game never actually
    // started (a swallowed synthetic click, a failed title hand-off), fall back
    // to the title state so Enter tries again instead of the player being stuck
    // on a screen the game does not know about.
    static DWORD s_novelUntil;
    static int s_novelHadStory;
    if (g_screen == SCR_NOVEL) {
        if (!s_novelUntil && !s_novelHadStory) { s_novelUntil = now + 20000; }
        if (TextStoryRecent(1200)) { s_novelHadStory = 1; s_novelUntil = 0; }
        else if (s_novelUntil && (long)(now - s_novelUntil) >= 0) {
            s_novelUntil = 0;
            s_novelHadStory = 0;
            Logf(L"menu: no story text for 20 s, reverting to title state");
            SpeakMenu(L"没有进入剧情，请再按一次回车");
            MenuSetScreen(SCR_TITLE);
        }
    } else { s_novelUntil = 0; s_novelHadStory = 0; }

    // Save / load screens paint nothing but the page counter ("1/10"), so the
    // counter is both our "which screen is this" probe and the page number.  The
    // player reported that the patch did not notice when the game went back from
    // the save page to the pause menu: this is that fix -- when the counter
    // disappears the save/load page is gone, and Escape always lands on pause.
    static int s_pageSeenOnScreen;
    static DWORD s_pageCheck;
    if ((long)(now - s_pageCheck) >= 0) {
        s_pageCheck = now + 250;
        int page = 0, total = 0;
        int hasPage = TextPageIndicator(&page, &total);
        if (hasPage) {
            g_pageSeenAt = now;
            if (page >= 1 && page != g_savePage) {
                Logf(L"menu: page counter sync %d -> %d of %d", g_savePage, page, total);
                g_savePage = page;
            }
            if (g_screen == SCR_SAVE || g_screen == SCR_LOAD) {
                s_pageSeenOnScreen = 1;
            } else if (g_screen != SCR_CONFIRM) {
                // The player reached the page with the mouse or a loaded save:
                // adopt it as our screen state instead of staying on "pause".
                int target = (g_saveLoadMode == SCR_LOAD) ? SCR_LOAD : SCR_SAVE;
                g_suppressName = 1;
                MenuSetScreen(target);
                s_pageSeenOnScreen = 1;
                wchar_t buf[80];
                wsprintfW(buf, L"第%d页，共%d页", page, total);
                SpeakMenu(buf);
            }
        } else if ((g_screen == SCR_SAVE || g_screen == SCR_LOAD) && s_pageSeenOnScreen &&
                   (DWORD)(now - g_pageSeenAt) > 3200) {
            int back = TextStoryRecent(800) ? SCR_NOVEL : SCR_PAUSE;
            s_pageSeenOnScreen = 0;
            Logf(L"menu: page counter gone, following the game to screen %d", back);
            MenuSetScreen(back);
        }
    }
    static DWORD s_choiceCheck;
    if ((long)(now - s_choiceCheck) >= 0) {
        s_choiceCheck = now + 250;
        if (g_screen == SCR_NOVEL || g_screen == SCR_CHOICE) {
            if ((long)(now - g_choiceMuteUntil) < 0) {
                // just clicked or just cancelled: do not re-detect yet
            } else if (g_screen == SCR_NOVEL) {
                ChoiceItem items[CHOICE_MAX];
                int n = TextChoices(items, CHOICE_MAX);
                if (n >= 2) {
                    FillChoiceScreen(items, n);
                    g_suppressName = 1;
                    MenuSetScreen(SCR_CHOICE);
                    wchar_t buf[64];
                    wsprintfW(buf, L"选项，共%d项，上下键选择，回车确定", n);
                    SpeakMenu(buf);
                    Announce(0);
                }
            } else {
                ChoiceItem items[CHOICE_MAX];
                int n = TextChoices(items, CHOICE_MAX);
                if (n < 2) {
                    g_suppressName = 1;
                    MenuSetScreen(SCR_NOVEL);
                }
            }
        }
    }
    // The game's modal boxes (save/load/delete/quit) are plain text panels with
    // two buttons.  We cannot hook their creation, but we can see their text, so
    // the dialog decides the screen: text appears -> confirm screen, text gone ->
    // back to where we were.  The game's own wording is what gets read out.
    static DWORD s_dlgCheck;
    if ((long)(now - s_dlgCheck) >= 0) {
        s_dlgCheck = now + 200;
        wchar_t line[256];
        int has = ((long)(GetTickCount() - g_dlgMuteUntil) < 0) ? 0 : TextDialogLine(line, 256);
        if (has && g_screen != SCR_CONFIRM) {
            g_suppressName = 1;
            MenuSetScreen(SCR_CONFIRM);
            // A queued click landing on the box would click 是 or 否 by accident.
            InputClickFlush();
            SpeakMenu(line);
            Logf(L"menu: dialog detected \"%s\"", line);
        } else if (!has && g_screen == SCR_CONFIRM) {
            int back = g_prev;
            g_suppressName = 1;
            MenuSetScreen((back == SCR_CONFIRM) ? SCR_NOVEL : back);
        }
    }
    // A slot click that opened no dialog saved silently (empty slot) or did
    // nothing (empty slot on the load screen).  Say which one it was.
    if (g_slotWaitUntil && (long)(now - g_slotWaitUntil) >= 0) {
        g_slotWaitUntil = 0;
        if (g_screen != SCR_CONFIRM) {
            SpeakMenu(g_slotWasSave ? L"已保存" : L"这个存档位是空的");
            Logf(L"menu: slot click produced no dialog (save=%d)", g_slotWasSave);
        }
    }
    // Wait for the synthetic click to finish before switching screens: one
    // activation takes ~2.2 s (æä½ 2000 ms), and switching earlier made the next
    // key act on a screen the game had not opened yet (save/load misfires).
    if (g_pendingScreen >= 0 && !InputClickBusy() && (long)(now - g_pendingScreenTick) >= 0) {
        MenuSetScreen(g_pendingScreen);
        g_pendingScreen = -1;
    }
}
