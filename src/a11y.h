// a11y.h -- shared declarations for the Lucy accessibility patch.
//
// This DLL is a proxy for the game's own d3dx9_40.dll.  It has three jobs:
//   1. forward all 336 D3DX entry points to the original DLL,
//   2. capture the text the engine paints (ID3DXFont::DrawTextW),
//   3. turn that text into speech, and add keyboard access for the mouse-only UI.
//
// Design contract (from the project's docs, do not weaken):
//   * no "navigation mode" switch: every key is a native-looking key,
//   * voiced story lines are NOT read automatically (their audio already speaks
//     them, character names included), but they can be re-read on demand,
//   * the re-read buffer holds story lines and choices only.
#pragma once

#include <windows.h>

#define A11Y_VERSION      L"0.2.4.0"
#define A11Y_PRODUCT      L"露西 -她所期望的一切- 无障碍补丁"
#define A11Y_LOG_NAME     L"a11y.log"
#define A11Y_INI_NAME     L"config.ini"

struct A11yConfig {
    // speech
    wchar_t backend[32];      // auto|nvda|zdsr|sapi|tolk
    int  rate;                // 0..100
    int  volume;              // 0..100
    // text pipeline
    int  voiceWindowMs;       // how long after the line appears a voice clip may start
    int  speakVoicedLines;    // 0 = a voiced line is never read on its own (design rule)
    int  minStableMs;         // a line is complete once it stopped growing this long
    int  logDraws;            // 1 = log every DrawTextW call (diagnostics only)
    // keys (virtual-key codes; 8 = Backspace)
    int  keyRepeat;           // re-read the current / last line
    int  keyPrev;             // re-read the previous line
    int  keyBackendMod;       // modifier, e.g. VK_MENU
    int  keyBackend;          // F9
};

extern A11yConfig g_cfg;

// ---- logging -------------------------------------------------------------
void LogInit(const wchar_t* baseDir);
void Logf(const wchar_t* fmt, ...);
const wchar_t* DataDir(void);

// ---- config --------------------------------------------------------------
void ConfigLoad(void);
void ConfigSave(void);

// ---- speech (implemented in speech/speech.cpp, shipped with C linkage) -----
#include "speech/speech.h"

// The only places that may call the speech backend.
void Say(const wchar_t* text);          // interrupt-and-speak (Repeat.Say semantics)
void SayReplay(void);                   // re-read the current line

// ---- repeat buffer: story text + choices only ----------------------------
void RepeatPush(const wchar_t* text);
const wchar_t* RepeatLast(void);
void RepeatPrev(void);

// ---- text pipeline (fonthook.cpp) ----------------------------------------
void TextInit(void);
void TextOnDraw(const wchar_t* text, unsigned fmt, unsigned width,
                unsigned fontPtr, unsigned color, int count);
void TextTick(DWORD now);               // called from the worker thread (~every 30 ms)
void TextSetStoryActive(int on);
// Text of the most recent draw whose box sits on the given row (used to read a
// setting's value aloud; the game paints values, it does not expose them).
int  TextValueNear(int y, int h, wchar_t* out, int cap);
int  TextDialogLine(wchar_t* out, int cap);
// 1 when story text was drawn in the last maxAgeMs: the game may move on its
// own (mouse, auto mode, a loaded save), so the screen has to follow it.
int  TextStoryRecent(unsigned maxAgeMs);

// ---- story choices --------------------------------------------------------
// The game's branch options are mouse-only bars; this is how the patch learns
// their rectangles and wording so the keyboard can pick one.
#define CHOICE_MAX 6
#define CHOICE_LEN 192
struct ChoiceItem {
    int x, y, w, h;
    unsigned tick;          // draw time; the two buttons appear in the same frame
    wchar_t text[CHOICE_LEN];
};
int  TextChoices(ChoiceItem* out, int cap);

// ---- voice detection (audio/audio_hook.h, written from the audiere analysis) --
// Audio_WasLineVoiced() is three-state: 1 voiced, 0 not voiced, -1 undecided;
// -1 means "ask again after Audio_RecommendDelayMs()".
#include "audio/audio_hook.h"

// ---- keyboard-first UI (menus.cpp) ---------------------------------------
void MenuInit(HWND gameWnd);
int  MenuScreen(void);                  // 0 = title, 1 = novel, 2 = pause menu, 3 = settings
void MenuSetScreen(int screen);
void MenuSetGameWnd(HWND w);
void MenuOnKey(int vk, int isDown);
int  MenuConsumedKey(void);             // 1 = the last key must be swallowed
void MenuTick(DWORD now);

// ---- font hook ------------------------------------------------------------
void FontHookSetOriginal(void* origDrawText);
void FontHookOnCreated(void* font);

// ---- lifecycle ------------------------------------------------------------
void StartA11y(HMODULE self);
void ProxyInitLate(void);

// ---- misc ----------------------------------------------------------------
HWND FindGameWindow(void);
void ProxyNoteFontsCaptured(void);
