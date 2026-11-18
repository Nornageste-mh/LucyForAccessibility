// menus.cpp -- keyboard access for a mouse-only UI.
//
// The game's title screen and pause menu ignore the keyboard completely, so the
// patch synthesises the mouse: the arrow keys move a virtual cursor from item to
// item, Enter performs a real hover + click at the item's centre.  The item
// rectangles were measured by hand (see docs/操作约定.md) and are given in
// client coordinates of the 1280x720 game window.
#include "a11y.h"

#include <string.h>

enum { SCR_TITLE = 0, SCR_NOVEL = 1, SCR_PAUSE = 2, SCR_SETTINGS = 3 };

struct MenuItem {
    const wchar_t* label;
    int x, y, w, h;      // client rect
};

// Title screen: the four entries on the right edge (measured 2026-11-18).
static const MenuItem kTitle[] = {
    { L"开始", 1045, 385, 130, 55 },
    { L"特典", 1045, 469, 130, 55 },
    { L"读取", 1045, 551, 130, 55 },
    { L"选项", 1045, 634, 130, 55 },
};
#define TITLE_COUNT (int)(sizeof(kTitle) / sizeof(kTitle[0]))

// Pause menu: seven icons (帮助/跳过/历史/保存/读取/选项/标题).  The rectangles
// are still being measured by recon-keys; until they are known the patch only
// handles Escape here instead of pretending to navigate.
static const MenuItem kPause[] = {
    { L"帮助", 0, 0, 0, 0 },
};
#define PAUSE_COUNT 0

static HWND g_wnd;
static int g_screen = SCR_TITLE;
static int g_sel;
static int g_consumed;
static int g_pendingScreen = -1;
static DWORD g_pendingScreenTick;

// click state machine
static int g_clickStage;
static DWORD g_clickTick;
static POINT g_clickPt;      // screen coordinates
static int g_clickTwice;

static const MenuItem* Items(int* count)
{
    if (g_screen == SCR_TITLE) { *count = TITLE_COUNT; return kTitle; }
    if (g_screen == SCR_PAUSE) { *count = PAUSE_COUNT; return kPause; }
    *count = 0;
    return 0;
}

static void ToScreen(int cx, int cy, POINT* out)
{
    out->x = cx;
    out->y = cy;
    if (g_wnd) { ClientToScreen(g_wnd, out); }
}

static void ClickAt(POINT screenPt, int twice)
{
    g_clickPt = screenPt;
    g_clickTwice = twice;
    g_clickStage = 1;
    g_clickTick = GetTickCount();
    SetCursorPos(screenPt.x, screenPt.y);
}

static void Announce(int idx, const MenuItem* items, int count)
{
    wchar_t buf[128];
    wsprintfW(buf, L"%s，%d之%d", items[idx].label, idx + 1, count);
    Speech_Speak(buf);
}

static void Activate(void)
{
    int count = 0;
    const MenuItem* items = Items(&count);
    if (!items || g_sel < 0 || g_sel >= count) { return; }
    const MenuItem* it = &items[g_sel];
    POINT pt;
    ToScreen(it->x + it->w / 2, it->y + it->h / 2, &pt);
    Logf(L"menu: activate \"%s\" at client %d,%d -> screen %ld,%ld", it->label,
         it->x + it->w / 2, it->y + it->h / 2, pt.x, pt.y);
    ClickAt(pt, g_screen == SCR_TITLE ? 1 : 0);

    if (g_screen == SCR_TITLE && !wcscmp(it->label, L"开始")) {
        // the story starts with the built-in help pages; text capture takes over
        g_screen = SCR_NOVEL;
        g_sel = 0;
    } else if (g_screen == SCR_PAUSE && !wcscmp(it->label, L"标题")) {
        g_screen = SCR_TITLE;
        g_sel = 0;
    }
}

// Pick the item nearest to (dx,dy) direction from the current selection.
static void Move(int dx, int dy)
{
    int count = 0;
    const MenuItem* items = Items(&count);
    if (!items || count <= 1 || g_sel < 0 || g_sel >= count) { return; }
    int cx = items[g_sel].x + items[g_sel].w / 2;
    int cy = items[g_sel].y + items[g_sel].h / 2;
    int best = -1;
    long bestScore = 0;
    for (int i = 0; i < count; i++) {
        if (i == g_sel) { continue; }
        int ix = items[i].x + items[i].w / 2;
        int iy = items[i].y + items[i].h / 2;
        int vx = ix - cx, vy = iy - cy;
        if (dx && vx * dx <= 0) { continue; }
        if (dy && vy * dy <= 0) { continue; }
        long along = dx ? vx * dx : vy * dy;
        long across = dx ? (vy < 0 ? -vy : vy) : (vx < 0 ? -vx : vx);
        long score = along + across * 3;
        if (best < 0 || score < bestScore) { best = i; bestScore = score; }
    }
    if (best >= 0) {
        g_sel = best;
        Logf(L"menu: selected %d \"%s\"", g_sel, items[g_sel].label);
        Announce(g_sel, items, count);
    }
}

void MenuInit(HWND w)
{
    g_wnd = w;
    g_screen = SCR_TITLE;
    g_sel = 0;
    Logf(L"menu: init, window=%p", (void*)w);
}

void MenuSetGameWnd(HWND w) { g_wnd = w; }
int MenuScreen(void) { return g_screen; }

void MenuSetScreen(int screen)
{
    if (screen == g_screen) { return; }
    Logf(L"menu: screen %d -> %d", g_screen, screen);
    g_screen = screen;
    g_sel = 0;
    if (screen == SCR_PAUSE && PAUSE_COUNT == 0) {
        Logf(L"menu: pause menu rectangles not measured yet; only Escape is handled");
    } else if (screen == SCR_TITLE) {
        Speech_Speak(L"标题画面");
    } else if (screen == SCR_NOVEL) {
        // nothing to announce: the story text itself is read
    }
}

int MenuConsumedKey(void) { return g_consumed; }

void MenuOnKey(int vk, int isDown)
{
    int count = 0;
    const MenuItem* items = Items(&count);
    if (!isDown) { return; }
    g_consumed = 0;

    if (vk == VK_ESCAPE) {
        if (g_screen == SCR_PAUSE) {
            // the game closes the menu itself; just follow it
            MenuSetScreen(SCR_NOVEL);
            return;
        }
        if (g_screen == SCR_NOVEL) {
            g_pendingScreen = SCR_PAUSE;
            g_pendingScreenTick = GetTickCount() + 350;
            return;
        }
        return;
    }

    if (!items || count == 0) { return; }

    switch (vk) {
    case VK_UP:    if (g_screen == SCR_TITLE) { Move(0, -1); g_consumed = 1; } break;
    case VK_DOWN:  if (g_screen == SCR_TITLE) { Move(0, 1); g_consumed = 1; } break;
    case VK_LEFT:  Move(-1, 0); g_consumed = 1; break;
    case VK_RIGHT: Move(1, 0); g_consumed = 1; break;
    case VK_RETURN:
    case VK_SPACE:
        // In the novel the game needs Enter/Space; only swallow them in a menu.
        if (g_screen == SCR_TITLE || g_screen == SCR_PAUSE) {
            Activate();
            g_consumed = 1;
        }
        break;
    case VK_HOME:
        if (g_screen != SCR_NOVEL) { g_sel = 0; Announce(0, items, count); g_consumed = 1; }
        break;
    case VK_END:
        if (g_screen != SCR_NOVEL) {
            g_sel = count - 1;
            Announce(g_sel, items, count);
            g_consumed = 1;
        }
        break;
    default:
        break;
    }
}

void MenuTick(DWORD now)
{
    if (g_pendingScreen >= 0 && (long)(now - g_pendingScreenTick) >= 0) {
        MenuSetScreen(g_pendingScreen);
        g_pendingScreen = -1;
    }
    if (g_clickStage == 0) { return; }

    DWORD dt = now - g_clickTick;
    if (g_clickStage == 1) {
        if (dt >= 120) {
            SetCursorPos(g_clickPt.x, g_clickPt.y);
            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            Sleep(15);
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            Logf(L"menu: clicked at %ld,%ld", g_clickPt.x, g_clickPt.y);
            g_clickStage = 2;
            g_clickTick = now;
        }
    } else if (g_clickStage == 2) {
        if (dt >= 220) {
            if (g_clickTwice) {
                SetCursorPos(g_clickPt.x, g_clickPt.y);
                mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                Sleep(15);
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                Logf(L"menu: second click (first one is often swallowed)");
            }
            g_clickStage = 0;
        }
    }
}
