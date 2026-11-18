// inputhook.h -- synthetic mouse input that the engine actually accepts.
//
// Facts established on the retail build (nkNovel 1.50, Engine.dll 2,486,272 B):
//   * Engine.dll imports GetAsyncKeyState/GetKeyState but never reads the mouse
//     position from the API -- it takes the pointer position from WM_MOUSEMOVE.
//   * Hardware input injected from inside the game process (mouse_event/SendInput)
//     is ignored by the UI, while the same call from another process works, so we
//     cannot fake the hardware path.
//   * Posted WM_LBUTTONDOWN/UP alone are not enough either: the engine only
//     registers a click when the button stays "down" for well over a second
//     (700 ms fails, 2000 ms works -- measured).
//
// So a click is: move away, move onto the target, hold the button down through
// the patched imports for ~2 s, then release.  The sequence is driven from the
// tick thread so the low level keyboard hook never blocks (Windows drops hooks
// that take longer than ~300 ms).
#ifndef LUCY_INPUTHOOK_H
#define LUCY_INPUTHOOK_H

#include <windows.h>

void InputHookInit(HWND gameWnd);
int  InputHookReady(void);

// Queue a click at client coordinates.  Returns 1 when the request was queued.
int  InputClickAt(int clientX, int clientY);
int  InputClickHoldAt(int clientX, int clientY, int holdMs);

// 1 while a queued click is still being played out.
int  InputClickBusy(void);

// Drive the queue; call from the tick thread (every ~20-40 ms).
void InputTick(void);

#endif
