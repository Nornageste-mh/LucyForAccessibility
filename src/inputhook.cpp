// inputhook.cpp -- see inputhook.h for the measurements behind this design.
#include <windows.h>
#include "a11y.h"
#include "inputhook.h"

// ---- state shared with the hooked imports --------------------------------
static volatile LONG g_fakeButton;      // tick until which the button reads down
static volatile LONG g_ready;
static HWND g_wnd;

static SHORT WINAPI FakeGetAsyncKeyState(int vk);
static SHORT WINAPI FakeGetKeyState(int vk);

typedef SHORT(WINAPI* PFN_GetAsyncKeyState)(int);
typedef SHORT(WINAPI* PFN_GetKeyState)(int);
static PFN_GetAsyncKeyState g_origAsync;
static PFN_GetKeyState g_origKey;

static int ButtonDown(void)
{
    LONG until = g_fakeButton;
    if (!until) { return 0; }
    return (LONG)(GetTickCount() - (DWORD)until) < 0;
}

static SHORT WINAPI FakeGetAsyncKeyState(int vk)
{
    if ((vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) && ButtonDown()) {
        // 0x8000 = currently down, 0x0001 = pressed since the last call.  Both
        // bits are set so a "was pressed" poll sees the click too.
        static int logged;
        if (logged < 6) { logged++; Logf(L"input: engine polled vk=%d while faked", vk); }
        return (SHORT)0x8001;
    }
    if (g_origAsync) { return g_origAsync(vk); }
    return (SHORT)GetAsyncKeyState(vk);
}

static SHORT WINAPI FakeGetKeyState(int vk)
{
    if ((vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) && ButtonDown()) {
        return (SHORT)0x8001;
    }
    if (g_origKey) { return g_origKey(vk); }
    return (SHORT)GetKeyState(vk);
}

// ---- import table patching ----------------------------------------------
// Replace the IAT slot of a module that the engine calls into.  Only the import
// descriptors of Engine.dll are touched; every other module keeps the real
// function.
static int PatchIat(HMODULE mod, const char* dllName, const char* funcName, void* replacement, void** original)
{
    if (!mod) { return 0; }
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { return 0; }
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { return 0; }
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) { return 0; }
    int patched = 0;
    IMAGE_IMPORT_DESCRIPTOR* imp =
        (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; imp->Name; imp++) {
        const char* dll = (const char*)(base + imp->Name);
        if (_stricmp(dll, dllName) != 0) { continue; }
        IMAGE_THUNK_DATA* nameThunk = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* addrThunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        if (!imp->OriginalFirstThunk) { nameThunk = addrThunk; }
        for (; nameThunk->u1.AddressOfData && addrThunk->u1.Function; nameThunk++, addrThunk++) {
            if (nameThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) { continue; }
            IMAGE_IMPORT_BY_NAME* ibn =
                (IMAGE_IMPORT_BY_NAME*)(base + nameThunk->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, funcName) != 0) { continue; }
            void** slot = (void**)&addrThunk->u1.Function;
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) { return 0; }
            if (original && !*original) { *original = *slot; }
            *slot = replacement;
            VirtualProtect(slot, sizeof(void*), old, &old);
            patched++;
        }
    }
    return patched;
}

// ---- click queue ---------------------------------------------------------
// Timing measured on the retail build: 120 ms and 700 ms holds are ignored,
// 2000 ms is accepted every time.  A double click needs the two clicks far apart
// (the first one of a pair is often swallowed by the engine).
#define CLICK_QUEUE_MAX  4
#define CLICK_MOVE_OUT   40     // ms before aiming at the target
#define CLICK_MOVE_IN    100    // ms the pointer rests on the target before press
#define CLICK_DEFAULT_HOLD 2000 // button-down time the engine needs to see
#define CLICK_DOUBLE_GAP 260    // ms between the two clicks of a double click

struct ClickReq
{
    int x;
    int y;
    int hold;
};

static ClickReq g_queue[CLICK_QUEUE_MAX];
static int g_queueCount;
static int g_queueHead;
static ClickReq g_cur;
static int g_stage;         // 0 idle, 1 moving out, 2 moving in, 3 pressed, 4 gap
static DWORD g_stageAt;

static void PostMove(int clientX, int clientY)
{
    if (!g_wnd) { return; }
    PostMessageW(g_wnd, WM_MOUSEMOVE, 0, MAKELPARAM(clientX, clientY));
}

// 真实光标也要移过去：标题画面用 GetCursorPos 判"指针是否在按钮上"，
// 只投递 WM_MOUSEMOVE 不足以让标题按钮进入 hover 态。
static void MoveRealCursor(int clientX, int clientY)
{
    if (!g_wnd) { return; }
    POINT p = { clientX, clientY };
    if (ClientToScreen(g_wnd, &p)) { SetCursorPos(p.x, p.y); }
}

void InputHookInit(HWND gameWnd)
{
    g_wnd = gameWnd;
    HMODULE engine = GetModuleHandleW(L"Engine.dll");
    if (!engine) {
        Logf(L"input: Engine.dll not loaded yet, input hook skipped");
        return;
    }
    int a = PatchIat(engine, "USER32.dll", "GetAsyncKeyState", (void*)&FakeGetAsyncKeyState,
                     (void**)&g_origAsync);
    int b = PatchIat(engine, "USER32.dll", "GetKeyState", (void*)&FakeGetKeyState,
                     (void**)&g_origKey);
    g_ready = (a > 0) ? 1 : 0;
    Logf(L"input: GetAsyncKeyState patched=%d GetKeyState patched=%d ready=%d", a, b, (int)g_ready);
}

int InputHookReady(void) { return g_ready ? 1 : 0; }

int InputClickHoldAt(int clientX, int clientY, int holdMs)
{
    if (!g_wnd) { return 0; }
    if (g_queueCount >= CLICK_QUEUE_MAX) { return 0; }
    int slot = (g_queueHead + g_queueCount) % CLICK_QUEUE_MAX;
    g_queue[slot].x = clientX;
    g_queue[slot].y = clientY;
    g_queue[slot].hold = holdMs > 0 ? holdMs : CLICK_DEFAULT_HOLD;
    g_queueCount++;
    return 1;
}

int InputClickAt(int clientX, int clientY)
{
    return InputClickHoldAt(clientX, clientY, CLICK_DEFAULT_HOLD);
}

int InputClickBusy(void)
{
    return (g_stage != 0 || g_queueCount > 0) ? 1 : 0;
}

static void StartNext()
{
    if (!g_queueCount) { return; }
    g_cur = g_queue[g_queueHead];
    g_queueHead = (g_queueHead + 1) % CLICK_QUEUE_MAX;
    g_queueCount--;
    // Move away first: the engine only reacts when the pointer enters the widget
    // on a frame of its own.
    PostMove(g_cur.x - CLICK_MOVE_OUT < 0 ? 0 : g_cur.x - CLICK_MOVE_OUT,
             g_cur.y - CLICK_MOVE_OUT < 0 ? 0 : g_cur.y - CLICK_MOVE_OUT);
    MoveRealCursor(g_cur.x - CLICK_MOVE_OUT < 0 ? 0 : g_cur.x - CLICK_MOVE_OUT,
                   g_cur.y - CLICK_MOVE_OUT < 0 ? 0 : g_cur.y - CLICK_MOVE_OUT);
    g_stage = 1;
    g_stageAt = GetTickCount();
}

void InputTick(void)
{
    if (!g_wnd) { return; }
    DWORD now = GetTickCount();
    switch (g_stage) {
    case 1:
        if (now - g_stageAt >= CLICK_MOVE_OUT) {
            PostMove(g_cur.x, g_cur.y);
            MoveRealCursor(g_cur.x, g_cur.y);
            g_stage = 2;
            g_stageAt = now;
        }
        break;
    case 2:
        if (now - g_stageAt >= CLICK_MOVE_IN) {
            LPARAM lp = MAKELPARAM(g_cur.x, g_cur.y);
            g_fakeButton = (LONG)(now + (DWORD)g_cur.hold);
            PostMessageW(g_wnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
            PostMessageW(g_wnd, WM_MOUSEMOVE, MK_LBUTTON, lp);
            g_stage = 3;
            g_stageAt = now;
        }
        break;
    case 3:
        if ((LONG)(now - g_stageAt) >= g_cur.hold) {
            LPARAM lp = MAKELPARAM(g_cur.x, g_cur.y);
            PostMessageW(g_wnd, WM_LBUTTONUP, 0, lp);
            PostMove(g_cur.x, g_cur.y);
            MoveRealCursor(g_cur.x, g_cur.y);
            g_fakeButton = 0;
            Logf(L"input: click client=%d,%d hold=%d", g_cur.x, g_cur.y, g_cur.hold);
            g_stage = 4;
            g_stageAt = now;
        }
        break;
    case 4:
        if (now - g_stageAt >= CLICK_DOUBLE_GAP) { g_stage = 0; }
        break;
    default:
        break;
    }
    if (g_stage == 0 && g_queueCount > 0) { StartNext(); }
}

