// =============================================================================
//  audio_hook.cpp -- Lucy (nkNovel 1.50) 配音判据模块 · GalA11y Forge
//
//  构建（32 位 /MT，编进 d3dx9_40 代理 DLL）：
//      cl /nologo /c /MT /O2 /W3 /utf-8 /EHsc audio_hook.cpp
//      -> audio_hook.obj（把 obj 加进代理 DLL 工程即可）
//  自检构建（生成一个可跑的判定真值表测试）：
//      cl /nologo /MT /O2 /W3 /utf-8 /EHsc /DAUDIO_HOOK_SELFTEST
//         audio_hook.cpp ..\test\judge_selftest.cpp /Fe:judge_selftest.exe
//
//  ============================ 判据语义（本文件实现） ============================
//  事件源（VOICE_PLAY 只由下面两类产生）：
//    L0  Engine.dll RTTI 类 nekoVoiceInstance 的 vtable（Engine+0x1F92AC）
//        slot7 = Engine+0x0CA710 = play()      [实测] ret      (无栈参数)
//        slot8 = Engine+0x0CA740 = stop(bool)  [实测] ret 4    (1 个栈参数)
//        => "某条语音开始/停止播放" 的直接函数，conf=3（最强）
//    L1  audiere IAT：AdrOpenSound 返回的 OutputStream 实例，实例级 vtable 副本
//        slot2=play / slot3=stop；只有资源名/类名证明是语音时才算 voice，conf=2
//    （BGM 走同一条 OutputStream 路径但类/名不同；音效走 AdrOpenSoundEffect/
//      SoundEffect，永远不算 voice。仅靠体积/时长的启发式 conf=1 默认不采信。）
//
//  判定（Audio_WasLineVoiced）：
//    有配音 := ∃ conf>=min_conf 的 VOICE_PLAY，tPlay ∈ [行起点-preroll, 行终点+window]，
//              未被"过早 stop"作废（played<min_played 视为假启动），
//              且未被更早的一行消费（一条语音只归属一行）。
//    窗口未过且无足够证据 => 返回 -1（未定论），绝不提前返回 1。
//    => 失败方向恒为"把有配音的当成旁白"（多念一遍，可忍），
//       绝不"把该念的吞掉"。
//
//  config：<exe目录>\a11y\config.ini（其次 <DLL目录>\a11y\config.ini）
//    voice_window_ms / voice_preroll_ms / voice_min_played_ms / voice_min_dur_ms
//    voice_min_conf / voice_log
//
//  证据链：REPORT.md §2/§3（导出表、IAT、vtable、反汇编）、evidence/verify_constants.py
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "audio_hook.h"

// ------------------------------------------------------------------ 默认配置 --
#define DEF_WINDOW_MS       1200   // 行结束后继续观察多久（真机实测标定后改这里）
#define DEF_PREROLL_MS       150   // 行开始前多久内的语音也算本行
#define DEF_MIN_PLAYED_MS    100   // 短于此就被 stop => 假启动，作废
#define DEF_MIN_DUR_MS       250   // 已知总时长短于此 => 不当台词
#define DEF_MIN_CONF           1   // 采信 conf>=1（含 MUSIC_PLAY 时间窗启发式，见 OnMusicPlay）
#define DEF_LOG                1

#define AUDIO_MAX_EVT         64
#define AUDIO_MAX_REC        256
#define AUDIO_VOICE_MAX_MS 15000
#define AUDIO_VOICE_MAX_BYTES (900*1024)

// 可信度
enum { CONF_NONE = 0, CONF_HEUR = 1, CONF_L1_NAME = 2, CONF_L0 = 3 };

// audiere 槽位（来源：audiere 1.9.4 include/audiere.h，ADR_METHOD=virtual ret __stdcall）
enum { OS_REF=0, OS_UNREF=1, OS_PLAY=2, OS_STOP=3, OS_ISPLAYING=4, OS_RESET=5,
       OS_SETREPEAT=6, OS_GETREPEAT=7, OS_SETVOLUME=8, OS_GETVOLUME=9,
       OS_SETPAN=10, OS_GETPAN=11, OS_SETPITCH=12, OS_GETPITCH=13,
       OS_ISSEEKABLE=14, OS_GETLENGTH=15, OS_SETPOSITION=16, OS_GETPOSITION=17,
       OS_SLOTS=18 };
enum { SE_REF=0, SE_UNREF=1, SE_PLAY=2, SE_STOP=3, SE_SETVOLUME=4, SE_GETVOLUME=5,
       SE_SETPAN=6, SE_GETPAN=7, SE_SETPITCH=8, SE_GETPITCH=9, SE_SLOTS=10 };
enum { SS_REF=0, SS_UNREF=1, SS_GETFORMAT=2, SS_READ=3, SS_RESET=4, SS_ISSEEKABLE=5,
       SS_GETLENGTH=6, SS_SETPOSITION=7, SS_GETPOSITION=8, SS_SLOTS=15 };

// Engine.dll 版本常量（[实测] evidence/verify_constants.py 14/14 PASS）
#define ENGINE_VOICE_VTABLE_RVA  0x1F92ACu
#define ENGINE_VOICE_PLAY_RVA    0x0CA710u
#define ENGINE_VOICE_STOP_RVA    0x0CA740u
#define ENGINE_LOADER_RVA        0x0DB5F0u
#define VS_PLAY 7
#define VS_STOP 8

// nekoHWMusic：三种脚本级音频对象（Voice/BGMusic/Sound）共用的播放包装类。
// 实例字段（[实测] ctor Engine+0xDB460，vtable 写入 0x101FA004，对象 0x84 字节）：
//   +0x2C  std::wstring 资源名（ctor 里 call [0x101E71F8] = wstring::operator=(wchar_t*)）
//   +0x3C  _Mysize（wchar 个数）  +0x40 _Myres（容量）
//   +0x70  audiere::OutputStream*  +0x74 float 音量  +0x78 循环次数
//   +0x7C  byte 待播标志            +0x80 StopCallback*
// Voice/BGM/Sound 实例都在 +0x70 放一个 8 字节智能指针，裸指针在 +0x74，指向 nekoHWMusic。
#define ENGINE_HWMUSIC_VTABLE_RVA 0x1FA004u
#define ENGINE_HWMUSIC_CTOR_RVA   0x0DB460u

// defined later, used earlier (config read / L1 open path)
static int  g_vtSpy = 0;
static void InstallStreamVtSpy(void* os);
#define HWM_NAME_OFF   0x2Cu
#define HWM_SIZE_OFF   0x3Cu
#define HWM_RES_OFF    0x40u
#define HWM_OS_OFF     0x70u
#define HWM_SIZE       0x84u
// 智能指针（Voice/BGMusic/Sound 实例内嵌，vtable=0x101EEF18）：裸指针在 +4
#define SP_RAW_OFF     0x74u

enum Cat { CAT_UNKNOWN=0, CAT_VOICE=1, CAT_BGM=2, CAT_SFX=3 };
static const char* kCatName[] = { "unknown", "voice", "bgm", "sfx" };

// =========================================================== 同步原语 =======
static CRITICAL_SECTION g_cs;        // 判据状态 + 事件表
static CRITICAL_SECTION g_logCs;
static CRITICAL_SECTION g_fileCs;    // File*/SampleSource* 映射
static volatile LONG    g_syncState = 0;   // 0=未初始化 1=正在初始化 2=就绪

static void SyncEnsure(void)
{
    if (g_syncState == 2) return;
    if (InterlockedCompareExchange(&g_syncState, 1, 0) == 0) {
        InitializeCriticalSection(&g_cs);
        InitializeCriticalSection(&g_logCs);
        InitializeCriticalSection(&g_fileCs);
        InterlockedExchange(&g_syncState, 2);
    } else {
        while (g_syncState != 2) Sleep(0);
    }
}

// =========================================================== 时钟 ==========
// 自检时由 Audio__TestSetClock 接管，正式运行时恒为 GetTickCount。
static volatile LONG g_testClockOn = 0;
static volatile LONG g_testClock   = 0;

static unsigned NowMs(void)
{
    if (g_testClockOn) return (unsigned)g_testClock;
    return (unsigned)GetTickCount();
}

// =========================================================== 日志 ==========
static char  g_logPath[MAX_PATH];
static int   g_logEnabled = DEF_LOG;
static int   g_logReady   = 0;

static void LogInit(HMODULE self)
{
    if (g_logReady) return;
    SyncEnsure();
    char p[MAX_PATH] = {0};
    if (self) GetModuleFileNameA(self, p, MAX_PATH);
    else      GetModuleFileNameA(NULL, p, MAX_PATH);
    char* s = strrchr(p, '\\');
    if (s) *(s + 1) = 0; else p[0] = 0;
    _snprintf_s(g_logPath, MAX_PATH, _TRUNCATE, "%saudio_hook.log", p);
    g_logReady = 1;
}

static void Log(const char* fmt, ...)
{
    if (!g_logEnabled) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    OutputDebugStringA("\r\n");
    if (!g_logReady) return;
    EnterCriticalSection(&g_logCs);
    HANDLE h = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, buf, (DWORD)strlen(buf), &wr, NULL);
        WriteFile(h, "\r\n", 2, &wr, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_logCs);
}

// =========================================================== 配置 ==========
static int g_windowMs     = DEF_WINDOW_MS;
static int g_preRollMs    = DEF_PREROLL_MS;
static int g_minPlayedMs  = DEF_MIN_PLAYED_MS;
static int g_minDurMs     = DEF_MIN_DUR_MS;
static int g_minConf      = DEF_MIN_CONF;
static int g_enableL0     = 1;     // voice_l0=0 可关（分阶段验证用）
static int g_enableL1     = 1;     // voice_l1=0 可关（OPEN 记录 + IAT 打补丁；默认开，判据依赖它）
static int g_enableL3     = 1;     // voice_l3=0 可关（改 .text 的内联 hook）
static int g_requireName  = 0;     // voice_require_name=1：L0 还要求资源名以 Voices\ 开头
static int g_l1Vtable     = 0;     // voice_l1_vtable=1：L1 修补实例 vtable（实测在游戏内会 c0000005 崩溃，默认关）
static char g_iniPath[MAX_PATH] = {0};

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 逐行扫描（不依赖 section 名，兼容 [a11y] / [audio] / 根级键）
static int IniInt(const char* key, int def)
{
    if (!g_iniPath[0]) return def;
    FILE* f = NULL;
    if (fopen_s(&f, g_iniPath, "rb") != 0 || !f) return def;
    char line[512];
    int val = def;
    size_t kl = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (_strnicmp(p, key, kl) != 0) continue;
        char* q = p + kl;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q != '=') continue;
        ++q;
        while (*q == ' ' || *q == '\t') ++q;
        val = atoi(q);
        break;
    }
    fclose(f);
    return val;
}

static void LoadConfig(HMODULE self)
{
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* s = strrchr(exe, '\\');
    if (s) *(s + 1) = 0; else exe[0] = 0;
    _snprintf_s(g_iniPath, MAX_PATH, _TRUNCATE, "%sa11y\\config.ini", exe);
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        char dll[MAX_PATH] = {0};
        GetModuleFileNameA(self, dll, MAX_PATH);
        char* t = strrchr(dll, '\\');
        if (t) *(t + 1) = 0; else dll[0] = 0;
        _snprintf_s(g_iniPath, MAX_PATH, _TRUNCATE, "%sa11y\\config.ini", dll);
        if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) g_iniPath[0] = 0;
    }
    g_windowMs    = Clamp(IniInt("voice_window_ms",     DEF_WINDOW_MS),     100, 5000);
    g_preRollMs   = Clamp(IniInt("voice_preroll_ms",    DEF_PREROLL_MS),      0, 1000);
    g_minPlayedMs = Clamp(IniInt("voice_min_played_ms", DEF_MIN_PLAYED_MS),   0, 2000);
    g_minDurMs    = Clamp(IniInt("voice_min_dur_ms",    DEF_MIN_DUR_MS),      0, 15000);
    g_minConf     = Clamp(IniInt("voice_min_conf",      DEF_MIN_CONF),        1, 3);
    g_logEnabled  = IniInt("voice_log", DEF_LOG) ? 1 : 0;
    g_enableL0    = IniInt("voice_l0", 1) ? 1 : 0;
    g_enableL1    = IniInt("voice_l1", 1) ? 1 : 0;
    g_enableL3    = IniInt("voice_l3", 1) ? 1 : 0;
    g_requireName = IniInt("voice_require_name", 0) ? 1 : 0;
g_l1Vtable    = IniInt("voice_l1_vtable", 0) ? 1 : 0;
g_vtSpy       = IniInt("voice_vtspy", 0) ? 1 : 0;
    if (g_iniPath[0])
        Log("[audio] NOTE  config: %s window=%d preroll=%d min_played=%d min_dur=%d min_conf=%d log=%d l0/l1/l3=%d%d%d require_name=%d",
            g_iniPath, g_windowMs, g_preRollMs, g_minPlayedMs, g_minDurMs, g_minConf, g_logEnabled,
            g_enableL0, g_enableL1, g_enableL3, g_requireName);
}

// =========================================================== 判据核心 =======
typedef struct {
    unsigned tPlay;      // 播放开始时刻
    unsigned tStop;      // 停止时刻（0 = 还没停）
    int      durMs;      // 已知总时长（0 = 未知）
    int      conf;       // CONF_*（0 = 空槽）
    unsigned usedGen;    // 已被哪一行消费（0 = 未被消费；0xFFFFFFFF = 无行号）
    char     name[160];
} VoiceEvt;

static VoiceEvt g_evt[AUDIO_MAX_EVT];
static int      g_evtCursor = 0;

static unsigned g_gen = 0;             // 行号，每次 Audio_OnLineStarted() ++
static unsigned g_lineStart = 0;
static int      g_lineStarted = 0;
static unsigned g_lineEnd = 0;
static int      g_lineLaunched = 0;

static int      g_finalVal = -1;       // 当前行的定论（-1 未定）
static unsigned g_finalGen = 0;
static int      g_lastFinalVal = -1;   // 最近一次定论（Backspace 重读用）

static int      g_statVoiced = 0, g_statUnvoiced = 0, g_statPlays = 0;

static unsigned g_lastVoiceTick = 0;
static int      g_lastVoiceValid = 0;
static char     g_lastVoiceName[160] = {0};

static void RecordVoicePlay(int conf, int durMs, const char* name)
{
    if (conf < CONF_HEUR) return;
    SyncEnsure();
    unsigned t = NowMs();
    EnterCriticalSection(&g_cs);
    VoiceEvt* e = &g_evt[g_evtCursor];
    g_evtCursor = (g_evtCursor + 1) % AUDIO_MAX_EVT;
    ZeroMemory(e, sizeof(*e));
    e->tPlay = t;
    e->durMs = durMs;
    e->conf  = conf;
    _snprintf_s(e->name, sizeof(e->name), _TRUNCATE, "%s", name ? name : "");
    g_lastVoiceTick = t;
    g_lastVoiceValid = 1;
    _snprintf_s(g_lastVoiceName, sizeof(g_lastVoiceName), _TRUNCATE, "%s", name ? name : "");
    g_statPlays++;
    LeaveCriticalSection(&g_cs);
    Log("[audio] VOICE_PLAY t=%u conf=%d dur_ms=%d name=\"%s\" gen=%u",
        t, conf, durMs, (name && name[0]) ? name : "?", g_gen);
}

static void RecordVoiceStop(void)
{
    SyncEnsure();
    unsigned t = NowMs();
    EnterCriticalSection(&g_cs);
    // 标记"最近一次仍在播"的事件
    int best = -1; unsigned bestT = 0;
    for (int i = 0; i < AUDIO_MAX_EVT; ++i) {
        VoiceEvt* e = &g_evt[i];
        if (e->conf == 0 || e->tStop != 0) continue;
        if (best < 0 || (unsigned)(e->tPlay - bestT) < 0x80000000u) { best = i; bestT = e->tPlay; }
    }
    if (best >= 0) g_evt[best].tStop = t;
    LeaveCriticalSection(&g_cs);
    Log("[audio] VOICE_STOP t=%u played_ms=%d gen=%u",
        t, best >= 0 ? (int)(t - bestT) : -1, g_gen);
}

// 在 [start-preRoll, end+window] 里找第一个"可用"的语音事件；返回下标或 -1
static int FindVoiceEvt(unsigned start, unsigned end)
{
    unsigned lo = start - (unsigned)g_preRollMs;      // unsigned 回绕安全
    unsigned hi = end + (unsigned)g_windowMs;
    unsigned span = hi - lo;
    for (int i = 0; i < AUDIO_MAX_EVT; ++i) {
        VoiceEvt* e = &g_evt[i];
        if (e->conf == 0) continue;
        if (e->conf < g_minConf) continue;
        if (e->usedGen != 0) continue;
        if (e->tStop && (unsigned)(e->tStop - e->tPlay) < (unsigned)g_minPlayedMs) continue; // 假启动
        if (e->durMs > 0 && e->durMs < g_minDurMs) continue;                                 // 太短
        if ((unsigned)(e->tPlay - lo) > span) continue;                                      // 不在窗口
        return i;
    }
    return -1;
}

static void Finalize(int verdict)
{
    if (g_gen == 0) {
        g_finalVal = verdict; g_finalGen = 0;
    } else {
        g_finalVal = verdict; g_finalGen = g_gen;
    }
    g_lastFinalVal = verdict;
    if (verdict == AUDIO_JUDGE_VOICED) g_statVoiced++; else g_statUnvoiced++;
    Log("[audio] JUDGE gen=%u voiced=%d window_ms=%d", g_gen, verdict, g_windowMs);
}

// =========================================================== 公开 API ======
extern "C" int Audio_SetWindow(int ms)
{
    SyncEnsure();
    EnterCriticalSection(&g_cs);
    g_windowMs = Clamp(ms, 100, 5000);
    int v = g_windowMs;
    LeaveCriticalSection(&g_cs);
    return v;
}

extern "C" int Audio_GetWindow(void)
{
    SyncEnsure();
    EnterCriticalSection(&g_cs);
    int v = g_windowMs;
    LeaveCriticalSection(&g_cs);
    return v;
}

extern "C" int Audio_SetPreRoll(int ms)
{
    SyncEnsure();
    EnterCriticalSection(&g_cs);
    g_preRollMs = Clamp(ms, 0, 1000);
    int v = g_preRollMs;
    LeaveCriticalSection(&g_cs);
    return v;
}

extern "C" void Audio_OnLineStarted(void)
{
    SyncEnsure();
    EnterCriticalSection(&g_cs);
    g_gen++;
    g_lineStart = NowMs();
    g_lineStarted = 1;
    g_lineEnd = 0;
    g_lineLaunched = 0;
    g_finalVal = -1;
    g_finalGen = g_gen;
    LeaveCriticalSection(&g_cs);
    Log("[audio] LINE_START gen=%u t=%u", g_gen, g_lineStart);
}

extern "C" void Audio_OnLineLaunched(void)
{
    SyncEnsure();
    EnterCriticalSection(&g_cs);
    unsigned now = NowMs();
    if (!g_lineStarted) { g_lineStart = now; g_lineStarted = 1; }
    g_lineEnd = now;
    g_lineLaunched = 1;
    LeaveCriticalSection(&g_cs);
    Log("[audio] LINE_END   gen=%u t=%u dur_ms=%u", g_gen, now,
        (unsigned)(now - g_lineStart));
}

extern "C" int Audio_WasLineVoiced(void)
{
    SyncEnsure();
    int ret = AUDIO_JUDGE_UNDECIDED;
    EnterCriticalSection(&g_cs);
    if (!g_lineStarted && !g_lineLaunched) { LeaveCriticalSection(&g_cs); return AUDIO_JUDGE_UNVOICED; }
    if (g_finalVal >= 0 && g_finalGen == g_gen) {
        ret = g_finalVal;
        LeaveCriticalSection(&g_cs);
        return ret;
    }
    unsigned now = NowMs();
    unsigned end = g_lineLaunched ? g_lineEnd : now;
    unsigned start = g_lineStarted ? g_lineStart : end;
    int idx = FindVoiceEvt(start, end);
    if (idx >= 0) {
        VoiceEvt* e = &g_evt[idx];
        unsigned played = (e->tStop ? e->tStop : now) - e->tPlay;
        int final = 0;
        if ((unsigned)(now - end) >= (unsigned)g_windowMs) final = 1;   // 窗口已过
        if (played >= (unsigned)g_minPlayedMs) final = 1;              // 已确认播够长
        if (final) {
            e->usedGen = g_gen ? g_gen : 0xFFFFFFFFu;                  // 一条语音只归属一行
            Finalize(AUDIO_JUDGE_VOICED);
            ret = AUDIO_JUDGE_VOICED;
        } else {
            ret = AUDIO_JUDGE_UNDECIDED;   // 刚开播，等 min_played 再定
        }
    } else if (g_lineLaunched && (unsigned)(now - end) >= (unsigned)g_windowMs) {
        Finalize(AUDIO_JUDGE_UNVOICED);
        ret = AUDIO_JUDGE_UNVOICED;
    } else {
        ret = AUDIO_JUDGE_UNDECIDED;
    }
    LeaveCriticalSection(&g_cs);
    return ret;
}

extern "C" int Audio_GetLastFinalVerdict(void)
{
    SyncEnsure();
    EnterCriticalSection(&g_cs);
    int v = g_lastFinalVal;
    LeaveCriticalSection(&g_cs);
    return v;
}

extern "C" int Audio_GetLastTick(void)
{
    SyncEnsure();
    EnterCriticalSection(&g_cs);
    int v = g_lastVoiceValid ? (int)g_lastVoiceTick : 0;
    LeaveCriticalSection(&g_cs);
    return v;
}

extern "C" int Audio_GetLastVoiceName(char* out, int cap)
{
    if (!out || cap <= 0) return 0;
    SyncEnsure();
    EnterCriticalSection(&g_cs);
    _snprintf_s(out, (size_t)cap, _TRUNCATE, "%s", g_lastVoiceName);
    int n = (int)strlen(out);
    LeaveCriticalSection(&g_cs);
    return n;
}

extern "C" int Audio_IsVoicePlaying(void)
{
    SyncEnsure();
    int playing = 0;
    EnterCriticalSection(&g_cs);
    unsigned now = NowMs();
    for (int i = 0; i < AUDIO_MAX_EVT; ++i) {
        VoiceEvt* e = &g_evt[i];
        if (e->conf == 0 || e->conf < g_minConf) continue;
        unsigned limit = e->durMs > 0 ? (unsigned)(e->durMs + 1500) : 60000u;
        if (limit > 60000u) limit = 60000u;
        if (e->tStop == 0 && (unsigned)(now - e->tPlay) < limit) { playing = 1; break; }
    }
    LeaveCriticalSection(&g_cs);
    return playing;
}

extern "C" int Audio_RecommendDelayMs(void)
{
    SyncEnsure();
    int rem = 0;
    EnterCriticalSection(&g_cs);
    if (!g_lineLaunched) {
        rem = g_windowMs;
    } else {
        unsigned now = NowMs();
        int left = (int)g_windowMs - (int)(unsigned)(now - g_lineEnd);
        if (left < 0) left = 0;
        rem = left;
        int idx = FindVoiceEvt(g_lineStarted ? g_lineStart : g_lineEnd, g_lineEnd);
        if (idx >= 0) {
            VoiceEvt* e = &g_evt[idx];
            unsigned played = (e->tStop ? e->tStop : now) - e->tPlay;
            int need = g_minPlayedMs - (int)played;
            if (need < 0) need = 0;
            if (need < rem) rem = need;
        }
        if (g_finalVal >= 0 && g_finalGen == g_gen) rem = 0;
    }
    LeaveCriticalSection(&g_cs);
    return rem;
}

extern "C" void Audio_GetStats(int* voiced, int* unvoiced, int* plays)
{
    SyncEnsure();
    EnterCriticalSection(&g_cs);
    if (voiced)   *voiced   = g_statVoiced;
    if (unvoiced) *unvoiced = g_statUnvoiced;
    if (plays)    *plays    = g_statPlays;
    LeaveCriticalSection(&g_cs);
}

extern "C" int Audio_GetLogPath(char* out, int cap)
{
    if (!out || cap <= 0) return -1;
    if (!g_logReady) LogInit(NULL);
    int n = (int)strlen(g_logPath);
    if (n >= cap) return -1;
    memcpy(out, g_logPath, n + 1);
    return n;
}

// ================================================= 内存可读性 / RTTI =======
static bool Readable(const void* p, size_t n)
{
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const uint8_t* end = (const uint8_t*)p + n;
    const uint8_t* regEnd = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    return end <= regEnd;
}

static uint32_t PtrToRva(uint32_t v, HMODULE base)
{
    if (v >= (uint32_t)base) return v - (uint32_t)base;
    return v;
}

// vtable[-1] -> COL -> TypeDescriptor.name（本工程 Engine.dll 用绝对 VA 填 COL）
static void ClassNameOf(void* obj, char* out, int cap)
{
    out[0] = 0;
    if (!Readable(obj, 4)) return;
    void** vt = *(void***)obj;
    if (!vt || !Readable(vt - 1, 4)) return;
    uint32_t* col = (uint32_t*)(vt[-1]);
    if (!Readable(col, 20)) return;
    if (col[0] != 0) return;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(col, &mbi, sizeof(mbi))) return;
    HMODULE base = (HMODULE)mbi.AllocationBase;
    if (!base) return;
    const char* td = (const char*)((const uint8_t*)base + PtrToRva(col[3], base));
    if (!Readable(td, 16)) return;
    const char* name = td + 8;
    if (!Readable(name, 8)) return;
    if (name[0] != '.' || name[1] != '?' || name[2] != 'A') return;
    _snprintf_s(out, (size_t)cap, _TRUNCATE, "%s", name);
}

static void ShortName(const char* mangled, char* out, int cap)
{
    out[0] = 0;
    if (!mangled || !mangled[0]) return;
    const char* p = mangled + 4;                 // skip ".?AV"
    const char* e = strchr(p, '@');
    int n = e ? (int)(e - p) : (int)strlen(p);
    if (n <= 0 || n >= cap) return;
    memcpy(out, p, n); out[n] = 0;
}

// --------------------------------------------------------- 资源名解码 ------
// nekoHWMusic+0x2C 是 std::wstring（[实测] ctor Engine+0xDB460：
//   lea ecx,[this+0x2C]; push <wchar_t*>; call dword ptr [0x101E71F8]，
//   0x101E71F8 = ??4?$basic_string@_W...QAEAAV01@PB_W@Z（wstring::operator=(wchar_t*)），
//   0x101E71F0 = ?c_str@?$basic_string@_W...（wstring::c_str），
//   0x101E71E0 = ??1?$basic_string@_W...（wstring::~wstring））。
// 32 位 VS2008/VS2022 wstring 布局一致：_Bx[16B] 起于 +0x2C，size<=7 时内联 8 个 wchar，
// 否则 _Bx 存堆指针；_Mysize 在 +0x3C，_Myres 在 +0x40。
// 只接受纯可打印 ASCII（游戏内资源名都是 ASCII）；任何异常一律判失败 → 输出空串，
// 绝不让"解错的乱码"参与判据（误判≈0 优先）。
// 尝试把一段 wchar 解码成可打印 ASCII；失败返回 0（out 置空）。
static int TryDecodeW(const wchar_t* w, uint32_t size, char* out, int cap)
{
    out[0] = 0;
    if (!w || size == 0) return 0;
    int n = (int)size;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; ++i) {
        unsigned c = (unsigned)w[i];
        if (c < 0x20 || c > 0x7E) { out[0] = 0; return 0; }
        out[i] = (char)c;
    }
    out[n] = 0;
    return n;
}

// 资源名解码（nekoHWMusic+0x2C 是 std::wstring）。
// 注意：SSO 阈值是“8 个 wchar 的内联缓冲”，但 32 位 MSVC 在 _Myres==0 的对象上
// 赋一个 7 字符名字时会退化成“容量 7 的堆串”（[实测] tmp\wtest 探针），所以
// size<=7 时两种布局都要试；只有全 ASCII 才算成功，解不出就返回 0。
static int DecodeWNameField(void* hw, char* out, int cap)
{
    if (!out || cap < 8) return 0;
    out[0] = 0;
    if (!hw || !Readable(hw, HWM_RES_OFF + 4)) return 0;
    const uint8_t* b = (const uint8_t*)hw;
    uint32_t size = *(const uint32_t*)(b + HWM_SIZE_OFF);
    uint32_t res  = *(const uint32_t*)(b + HWM_RES_OFF);
    if (size == 0 || size > 200) return 0;      // 资源名不会超过 200 个 wchar
    if (res < size || res > 4096) return 0;
    if (size <= 7) {
        const wchar_t* inl = (const wchar_t*)(b + HWM_NAME_OFF);
        if (Readable((void*)inl, (int)(size * 2 + 2))) {
            int n = TryDecodeW(inl, size, out, cap);
            if (n) return n;
        }
        uint32_t p = *(const uint32_t*)(b + HWM_NAME_OFF);
        if (p && Readable((void*)p, (int)(size * 2))) {
            int n = TryDecodeW((const wchar_t*)p, size, out, cap);
            if (n) return n;
        }
        return 0;
    }
    uint32_t ptr = *(const uint32_t*)(b + HWM_NAME_OFF);
    if (!ptr || !Readable((void*)ptr, (int)(size * 2))) return 0;
    return TryDecodeW((const wchar_t*)ptr, size, out, cap);
}

// 兼容旧调用点（L3 载入器）：this 即 nekoHWMusic 实例。
static void ReadNameField(void* self, char* out, int cap)
{
    char tmp[256];
    if (DecodeWNameField(self, tmp, (int)sizeof(tmp))) {
        _snprintf_s(out, (size_t)cap, _TRUNCATE, "%s", tmp);
        return;
    }
    out[0] = 0;
    if (!Readable(self, HWM_RES_OFF + 4)) return;
    const uint8_t* b = (const uint8_t*)self;
    _snprintf_s(out, (size_t)cap, _TRUNCATE, "<raw:%08x %08x %08x>",
                *(const uint32_t*)(b + HWM_NAME_OFF),
                *(const uint32_t*)(b + HWM_SIZE_OFF),
                *(const uint32_t*)(b + HWM_RES_OFF));
}

// 资源名前缀 → 类别。
// [实测] nkpack 明文名：Voice.nkpack 2118 条全为 Voices\lucyNNNN[_M]_jp.mp3 / _kr.mp3；
//        BGM.nkpack 样例 BGMs\c17.mp3 / BGMs\c2.mp3 / BGMs\community.mp3；
//        FX.nkpack  样例 FXs\airport.mp3 / FXs\alarmclock.mp3 / FXs\bell.mp3。
static int CatFromName(const char* src)
{
    if (!src || !src[0]) return CAT_UNKNOWN;
    if (_strnicmp(src, "Voices\\", 7) == 0) return CAT_VOICE;
    if (_strnicmp(src, "BGMs\\",   5) == 0) return CAT_BGM;
    if (_strnicmp(src, "FXs\\",    4) == 0) return CAT_SFX;
    if (strstr(src, "Voices\\") || strstr(src, "Voices/")) return CAT_VOICE;
    if (strstr(src, "BGMs\\")   || strstr(src, "BGMs/"))   return CAT_BGM;
    if (strstr(src, "FXs\\")    || strstr(src, "FXs/"))    return CAT_SFX;
    return CAT_UNKNOWN;
}

// ===================================================== 分类（L1 用） =======
static int ClassifyName(const char* cls, const char* src)
{
    int c = CatFromName(src);
    if (c == CAT_VOICE) return CONF_L1_NAME;
    if (c == CAT_BGM || c == CAT_SFX) return CONF_NONE;
    if (cls && cls[0]) {
        if (strstr(cls, "Voice")) return CONF_L1_NAME;
        if (strstr(cls, "BGMusic") || strstr(cls, "Music") || strstr(cls, "Sound")) return CONF_NONE;
    }
    return CONF_NONE;   // 认不出 = 不算语音（误判≈0 优先）
}

// ===================================================== L0 语音虚表 hook ====
static void* g_realVoicePlay = NULL;
static void* g_realVoiceStop = NULL;
static int   g_l0Installed = 0;

static void __cdecl OnVoicePlayL0(void* self)
{
    char nm[256] = {0};
    if (Readable(self, SP_RAW_OFF + 4)) {
        // 语音实例 +0x70 是内嵌智能指针（vtable 0x101EEF18），裸指针在 +0x74；
        // play 自己也是先 call 0x5FC00(取 [ecx+4]) 再 call 0xDBB80，所以这里读同一个字段。
        void* hw = *(void**)((uint8_t*)self + SP_RAW_OFF);
        HMODULE eng = GetModuleHandleA("Engine.dll");
        if (hw && Readable(hw, HWM_SIZE)) {
            void** vt = *(void***)hw;
            // 正式运行时严格校验 nekoHWMusic 虚表；自检环境没有 Engine.dll，跳过校验
            int isHw = eng ? (vt == (void**)((uint8_t*)eng + ENGINE_HWMUSIC_VTABLE_RVA)) : 1;
            if (isHw)
                DecodeWNameField(hw, nm, sizeof(nm));
            else
                _snprintf_s(nm, sizeof(nm), _TRUNCATE, "<not-hwmusic vt=%p>", (void*)vt);
        }
    }
    // 名字能读出来时，前缀是最后一道保险：BGMs\ / FXs\ 绝不记成语音
    int c = CatFromName(nm);
    if (c == CAT_BGM || c == CAT_SFX) {
        Log("[audio] NOTE  L0 play 名字非语音，忽略 name=\"%s\"", nm);
        return;
    }
    if (g_requireName && c != CAT_VOICE) {
        Log("[audio] NOTE  L0 play 名字未确认语音，require_name=1 忽略 name=\"%s\"", nm);
        return;
    }
    RecordVoicePlay(CONF_L0, 0, nm);
}

static void __cdecl OnVoiceStopL0(void* self, int immediate)
{
    (void)self; (void)immediate;
    RecordVoiceStop();
}

// play(): thiscall, ret      => 保存 ecx / 调 C 助手 / 尾跳原函数
static void __declspec(naked) Thunk_VoicePlay(void)
{
    __asm {
        push ecx
        push ecx
        call OnVoicePlayL0
        add  esp, 4
        pop  ecx
        jmp  dword ptr [g_realVoicePlay]
    }
}
// stop(bool): thiscall, ret 4 => 同上，但保留栈上的参数
static void __declspec(naked) Thunk_VoiceStop(void)
{
    __asm {
        push ecx
        push dword ptr [esp + 8]
        push ecx
        call OnVoiceStopL0
        add  esp, 8
        pop  ecx
        jmp  dword ptr [g_realVoiceStop]
    }
}

static int InstallVoiceVtableHook(void)
{
    HMODULE eng = GetModuleHandleA("Engine.dll");
    if (!eng) return 0;
    void** vt = (void**)((uint8_t*)eng + ENGINE_VOICE_VTABLE_RVA);
    if (!Readable(vt, 12 * 4)) return 0;
    void* p7 = vt[VS_PLAY];
    void* p8 = vt[VS_STOP];
    if ((uint32_t)p7 - (uint32_t)eng != ENGINE_VOICE_PLAY_RVA ||
        (uint32_t)p8 - (uint32_t)eng != ENGINE_VOICE_STOP_RVA) {
        Log("[audio] NOTE  L0 版本不符 (slot7=%p slot8=%p)，跳过（不影响 L1）", p7, p8);
        return 0;
    }
    DWORD old = 0;
    if (!VirtualProtect(vt, 12 * 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    g_realVoicePlay = p7;
    g_realVoiceStop = p8;
    vt[VS_PLAY] = (void*)Thunk_VoicePlay;
    vt[VS_STOP] = (void*)Thunk_VoiceStop;
    VirtualProtect(vt, 12 * 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), vt, 12 * 4);
    g_l0Installed = 1;
    Log("[audio] NOTE  L0 已装：nekoVoiceInstance vtable slot7/8 (Engine+0x%X)", ENGINE_VOICE_VTABLE_RVA);
    return 1;
}

// ===================================================== L1 audiere IAT =====
typedef void* (__stdcall *PFN_CreateMemoryFile)(const void*, int);
typedef void* (__stdcall *PFN_OpenSampleSourceFromFile)(void*, int);
typedef void* (__stdcall *PFN_OpenSound)(void*, void*, int);
typedef void* (__stdcall *PFN_OpenSoundEffect)(void*, void*, int);
typedef void* (__stdcall *PFN_OpenDevice)(const char*, const char*);

static PFN_CreateMemoryFile         real_CreateMemoryFile = NULL;
static PFN_OpenSampleSourceFromFile real_OpenSampleSourceFromFile = NULL;
static PFN_OpenSound                real_OpenSound = NULL;
static PFN_OpenSoundEffect          real_OpenSoundEffect = NULL;
static PFN_OpenDevice               real_OpenDevice = NULL;
static int g_l1Installed = 0;

struct FileRec { void* file; int size; char magic[8]; int inUse; };
#define FILE_MAX 64
static FileRec g_file[FILE_MAX];
static volatile LONG g_fileNext;   // FIFO ring: never exhausts on long sessions

struct SrcRec { void* src; int size; int rate; int ch; int durMs; char magic[8]; int fmt; int inUse; };
static SrcRec g_src[64];

struct Rec {
    void*  obj;
    void** origVt;
    void** ourVt;
    int    slots;
    int    cat;
    int    conf;
    char   cls[96];
    char   src[256];
    int    size;
    int    rate;
    int    ch;
    int    durMs;
    int    streaming;
    int    fmt;
    char   magic[8];
    int    inUse;
    DWORD  usedTick;   // for recycling when the table is full (long sessions)
};
static Rec g_rec[AUDIO_MAX_REC];
static volatile LONG g_idSeq = 0;

__declspec(thread) char t_src[256];
__declspec(thread) char t_cls[96];

static void SniffMagic(const void* buf, int size, char out[8])
{
    out[0] = 0;
    if (!buf || size < 4 || !Readable(buf, 4)) return;
    const uint8_t* b = (const uint8_t*)buf;
    if (b[0]=='I'&&b[1]=='D'&&b[2]=='3') { strcpy_s(out, 8, "mp3"); return; }
    if (b[0]==0xFF&&(b[1]&0xE0)==0xE0)   { strcpy_s(out, 8, "mp3f"); return; }
    if (b[0]=='O'&&b[1]=='g'&&b[2]=='g'&&b[3]=='S') { strcpy_s(out, 8, "ogg"); return; }
    if (b[0]=='R'&&b[1]=='I'&&b[2]=='F'&&b[3]=='F') { strcpy_s(out, 8, "wav"); return; }
    if (b[0]=='f'&&b[1]=='L'&&b[2]=='a'&&b[3]=='C') { strcpy_s(out, 8, "flac"); return; }
    if (b[0]=='F'&&b[1]=='O'&&b[2]=='R'&&b[3]=='M') { strcpy_s(out, 8, "aiff"); return; }
    _snprintf_s(out, 8, _TRUNCATE, "%02x%02x%02x%02x", b[0], b[1], b[2], b[3]);
}

static Rec* RecAlloc(void)
{
    Rec* r = NULL;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < AUDIO_MAX_REC; ++i)
        if (!g_rec[i].inUse) { r = &g_rec[i]; break; }
    if (!r) {
        // Table full (long session): recycle the least recently allocated slot.
        // Without this the voice judge silently stops working after 256 loads.
        DWORD oldest = g_rec[0].usedTick;
        r = &g_rec[0];
        for (int i = 1; i < AUDIO_MAX_REC; ++i)
            if ((LONG)(g_rec[i].usedTick - oldest) < 0) { oldest = g_rec[i].usedTick; r = &g_rec[i]; }
        static int s_warned;
        if (!s_warned) { s_warned = 1; LeaveCriticalSection(&g_cs); Log("[audio] NOTE rec table full (%d), recycling oldest slot", AUDIO_MAX_REC); EnterCriticalSection(&g_cs); }
    }
    ZeroMemory(r, sizeof(*r));
    r->inUse = 1;
    r->usedTick = GetTickCount();
    LeaveCriticalSection(&g_cs);
    return r;
}

static Rec* RecFindObj(void* obj)
{
    if (!obj) return NULL;
    Rec* r = NULL;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < AUDIO_MAX_REC; ++i)
        if (g_rec[i].inUse && g_rec[i].obj == obj) { r = &g_rec[i]; break; }
    LeaveCriticalSection(&g_cs);
    return r;
}

static void* __stdcall Hook_CreateMemoryFile(const void* buf, int size)
{
    void* f = real_CreateMemoryFile(buf, size);
    if (f) {
        EnterCriticalSection(&g_fileCs);
        LONG slot = (g_fileNext + 1) % FILE_MAX;
        g_fileNext = slot;
        g_file[slot].inUse = 1; g_file[slot].file = f; g_file[slot].size = size;
        SniffMagic(buf, size, g_file[slot].magic);
        LeaveCriticalSection(&g_fileCs);
    }
    return f;
}

static void* __stdcall Hook_OpenSampleSourceFromFile(void* file, int fmt)
{
    void* src = real_OpenSampleSourceFromFile(file, fmt);
    if (!src) return src;
    SrcRec* sr = NULL;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < 64; ++i) if (!g_src[i].inUse) { sr = &g_src[i]; ZeroMemory(sr, sizeof(*sr)); sr->inUse = 1; break; }
    LeaveCriticalSection(&g_cs);
    if (!sr) return src;
    sr->src = src; sr->fmt = fmt;
    EnterCriticalSection(&g_fileCs);
    for (int i = 0; i < 64; ++i) if (g_file[i].inUse && g_file[i].file == file) {
        sr->size = g_file[i].size;
        strcpy_s(sr->magic, 8, g_file[i].magic);
        break;
    }
    LeaveCriticalSection(&g_fileCs);
    if (Readable(src, 4)) {
        void** vt = *(void***)src;
        if (vt && Readable(vt, SS_SLOTS * 4)) {
            typedef void (__stdcall *PF_GetFormat)(void*, int*, int*, int*);
            typedef int  (__stdcall *PF_GetLength)(void*);
            int ch = 0, rate = 0, sf = 0;
            PF_GetFormat gf = (PF_GetFormat)vt[SS_GETFORMAT];
            if (gf) gf(src, &ch, &rate, &sf);
            sr->ch = ch; sr->rate = rate;
            PF_GetLength gl = (PF_GetLength)vt[SS_GETLENGTH];
            if (gl) {
                int frames = gl(src);
                if (frames > 0 && rate > 0) sr->durMs = (int)((double)frames * 1000.0 / rate);
            }
        }
    }
    return src;
}

static void PatchInstanceVtable(void* obj, Rec* r, int slots)
{
    if (!obj || !Readable(obj, 4)) return;
    void** vt = *(void***)obj;
    if (!vt || !Readable(vt, slots * 4)) return;
    void** nv = (void**)HeapAlloc(GetProcessHeap(), 0, slots * sizeof(void*));
    if (!nv) return;
    memcpy(nv, vt, slots * sizeof(void*));
    r->origVt = vt;
    r->ourVt = nv;
    r->slots = slots;
    *(void***)obj = nv;
}

static void __stdcall Thunk_OSPlay(void* self);
static void __stdcall Thunk_OSStop(void* self);
static void __stdcall Thunk_SEPlay(void* self);

static void* __stdcall Hook_OpenSound(void* dev, void* src, int streaming)
{
    void* os = real_OpenSound(dev, src, streaming);
    if (!os) return os;
    Rec* r = RecAlloc();
    if (!r) return os;
    r->obj = os; r->streaming = streaming ? 1 : 0;
    strcpy_s(r->src, sizeof(r->src), t_src);
    strcpy_s(r->cls, sizeof(r->cls), t_cls);
    t_src[0] = 0; t_cls[0] = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < 64; ++i) if (g_src[i].inUse && g_src[i].src == src) {
        r->size = g_src[i].size; r->rate = g_src[i].rate; r->ch = g_src[i].ch;
        r->durMs = g_src[i].durMs; r->fmt = g_src[i].fmt;
        strcpy_s(r->magic, 8, g_src[i].magic);
        g_src[i].inUse = 0;
        break;
    }
    LeaveCriticalSection(&g_cs);
    InstallStreamVtSpy(os);
    r->conf = ClassifyName(r->cls, r->src);
    int nc = CatFromName(r->src);
    r->cat = (nc != CAT_UNKNOWN) ? nc : (r->conf >= CONF_L1_NAME ? CAT_VOICE : CAT_BGM);
    if (g_l1Vtable) {
        PatchInstanceVtable(os, r, OS_SLOTS);
        if (r->ourVt) { r->ourVt[OS_PLAY] = (void*)Thunk_OSPlay; r->ourVt[OS_STOP] = (void*)Thunk_OSStop; }
    }
    Log("[audio] OPEN  id=%ld t=%u cat=%s conf=%d class=%s src=\"%s\" size=%d magic=%s rate=%d ch=%d dur_ms=%d streaming=%d",
        (long)InterlockedIncrement(&g_idSeq), NowMs(), kCatName[r->cat], r->conf, r->cls, r->src,
        r->size, r->magic, r->rate, r->ch, r->durMs, r->streaming);
    return os;
}

static void* __stdcall Hook_OpenSoundEffect(void* dev, void* src, int type)
{
    void* se = real_OpenSoundEffect(dev, src, type);
    if (!se) return se;
    Rec* r = RecAlloc();
    if (!r) return se;
    r->obj = se; r->streaming = -1; r->cat = CAT_SFX; r->conf = CONF_NONE;
    strcpy_s(r->src, sizeof(r->src), t_src);
    strcpy_s(r->cls, sizeof(r->cls), t_cls);
    t_src[0] = 0; t_cls[0] = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < 64; ++i) if (g_src[i].inUse && g_src[i].src == src) {
        r->size = g_src[i].size; r->durMs = g_src[i].durMs;
        strcpy_s(r->magic, 8, g_src[i].magic);
        g_src[i].inUse = 0;
        break;
    }
    LeaveCriticalSection(&g_cs);
    if (g_l1Vtable) {
        PatchInstanceVtable(se, r, SE_SLOTS);
        if (r->ourVt) { r->ourVt[SE_PLAY] = (void*)Thunk_SEPlay; }
    }
    Log("[audio] OPEN  id=%ld t=%u cat=sfx class=%s src=\"%s\" size=%d magic=%s dur_ms=%d type=%d",
        (long)InterlockedIncrement(&g_idSeq), NowMs(), r->cls, r->src, r->size, r->magic, r->durMs, type);
    return se;
}

static void __stdcall Thunk_OSPlay(void* self)
{
    Rec* r = RecFindObj(self);
    if (r && r->conf >= CONF_L1_NAME) RecordVoicePlay(r->conf, r->durMs, r->src);
    if (r && r->origVt && Readable(r->origVt, r->slots * 4)) {
        typedef void (__stdcall *PF)(void*);
        PF p = (PF)r->origVt[OS_PLAY];
        if (p) p(self);
    }
}

static void __stdcall Thunk_OSStop(void* self)
{
    Rec* r = RecFindObj(self);
    if (r && r->conf >= CONF_L1_NAME) RecordVoiceStop();
    if (r && r->origVt && Readable(r->origVt, r->slots * 4)) {
        typedef void (__stdcall *PF)(void*);
        PF p = (PF)r->origVt[OS_STOP];
        if (p) p(self);
    }
}

static void __stdcall Thunk_SEPlay(void* self)
{
    Rec* r = RecFindObj(self);
    if (r && r->origVt && Readable(r->origVt, r->slots * 4)) {
        typedef void (__stdcall *PF)(void*);
        PF p = (PF)r->origVt[SE_PLAY];
        if (p) p(self);
    }
}

// ===================================================== L3 载入器内联 ======
static uint8_t* g_tramp = NULL;
static void*    g_engineTarget = NULL;
static int      g_l3Installed = 0;

static int __fastcall Hook_EngineLoader(void* self, void* edx);

static int __fastcall Hook_EngineLoader(void* self, void* edx)
{
    char nm[256] = {0}, clsRaw[160] = {0}, cls[96] = {0};
    ReadNameField(self, nm, sizeof(nm));
    ClassNameOf(self, clsRaw, sizeof(clsRaw));
    ShortName(clsRaw, cls, sizeof(cls));
    strcpy_s(t_src, sizeof(t_src), nm);
    strcpy_s(t_cls, sizeof(t_cls), cls);
    Log("[audio] LOAD  t=%u class=%s cat=%s src=\"%s\"",
        NowMs(), cls, kCatName[CatFromName(nm)], nm);
    int r = ((int (__fastcall*)(void*, void*))g_tramp)(self, edx);
    return r;
}

static int InstallEngineLoaderHook(void)
{
    HMODULE eng = GetModuleHandleA("Engine.dll");
    if (!eng) return 0;
    uint8_t* t = (uint8_t*)eng + ENGINE_LOADER_RVA;
    if (!Readable(t, 16)) return 0;
    if (!(t[0]==0x55 && t[1]==0x8B && t[2]==0xEC && t[3]==0x6A && t[4]==0xFF)) {
        Log("[audio] NOTE  L3 序言不符(%02x %02x %02x %02x %02x)，跳过", t[0],t[1],t[2],t[3],t[4]);
        return 0;
    }
    g_tramp = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_tramp) return 0;
    memcpy(g_tramp, t, 5);
    g_tramp[5] = 0xE9;
    *(int32_t*)(g_tramp + 6) = (int32_t)((t + 5) - (g_tramp + 10));
    DWORD old = 0;
    if (!VirtualProtect(t, 8, PAGE_EXECUTE_READWRITE, &old)) return 0;
    t[0] = 0xE9;
    *(int32_t*)(t + 1) = (int32_t)((uint8_t*)&Hook_EngineLoader - (t + 5));
    VirtualProtect(t, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 8);
    g_engineTarget = t;
    g_l3Installed = 1;
    Log("[audio] NOTE  L3 已装：Engine+0x%X 载入器内联 hook", ENGINE_LOADER_RVA);
    return 1;
}

// ===================================================== IAT patch =========
// Engine.dll imports the audiere entry points under their stdcall-decorated
// names ("_AdrOpenSound@12"), so an exact match against the plain name never
// hits and the whole L1 layer silently installed 0/5 hooks.
static bool ImportNameMatches(const char* imported, const char* plain)
{
    if (strcmp(imported, plain) == 0) { return true; }
    size_t n = strlen(plain);
    return imported[0] == '_' && strncmp(imported + 1, plain, n) == 0 &&
           imported[1 + n] == '@';
}

static bool PatchIATOne(HMODULE mod, const char* dllName, const char* fnName,
                        void* hook, void** origOut)
{
    uint8_t* base = (uint8_t*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* dll = (const char*)(base + imp->Name);
        if (_stricmp(dll, dllName) != 0) continue;
        IMAGE_THUNK_DATA32* oft = (IMAGE_THUNK_DATA32*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA32* ft  = (IMAGE_THUNK_DATA32*)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, fnName) != 0 &&
                !ImportNameMatches((const char*)ibn->Name, fnName)) continue;
            if (origOut) *origOut = (void*)ft->u1.Function;
            if (hook == NULL) return true;      // 只取原指针
            DWORD old = 0;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
            ft->u1.Function = (uint32_t)hook;
            VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// ===================================================== 安装 / 卸载 =======
// ============================= stream vtable spy (diagnostic, opt-in) =====
// audiere hands us its OutputStream objects; they all share one class vtable in
// audiere.dll.  Copying it per instance crashed the game, but patching the
// shared table once (and jumping through to the original) is a plain function
// pointer swap.  Slot numbers are unknown for this build, so spy on 3..10 and
// let the log tell us which one is play().
static void* g_vtSpyOrig[11];
static void* g_vtSpyTable = NULL;
static LONG  g_vtSlotHits[11];

static void __cdecl OnVtSlot(int slot, void* self)
{
    if (InterlockedIncrement(&g_vtSlotHits[slot]) <= 6)
        Log("[audio] VTSLOT %d t=%u obj=%p", slot, NowMs(), self);
}

#define VTSPY_THUNK(N)                                                        \
    static void __declspec(naked) Thunk_VtSpy##N(void)                        \
    {                                                                         \
        __asm { pushad }                                                      \
        __asm { push [esp + 24] }        /* self = ECX saved by pushad (thiscall) */  \
        __asm { push N }                                                      \
        __asm { call OnVtSlot }                                               \
        __asm { add  esp, 8 }                                                 \
        __asm { popad }                                                       \
        __asm { jmp  dword ptr [g_vtSpyOrig + N * 4] }                         \
    }

VTSPY_THUNK(3)
VTSPY_THUNK(4)
VTSPY_THUNK(5)
VTSPY_THUNK(6)
VTSPY_THUNK(7)
VTSPY_THUNK(8)
VTSPY_THUNK(9)
VTSPY_THUNK(10)

static void* const g_vtSpyThunk[11] = {
    NULL, NULL, NULL,
    (void*)Thunk_VtSpy3, (void*)Thunk_VtSpy4, (void*)Thunk_VtSpy5,
    (void*)Thunk_VtSpy6, (void*)Thunk_VtSpy7, (void*)Thunk_VtSpy8,
    (void*)Thunk_VtSpy9, (void*)Thunk_VtSpy10
};

static void InstallStreamVtSpy(void* os)
{
    if (!g_vtSpy || g_vtSpyTable || !os) return;
    void** vt = *(void***)os;
    if (!Readable(vt, 16 * 4)) return;
    HMODULE aud = GetModuleHandleA("audiere.dll");
    if (aud) {
        uint32_t base = (uint32_t)aud, here = (uint32_t)vt;
        if (here < base || here > base + 0x200000u) {
            Log("[audio] VTSLOT 表不在 audiere.dll 内 (%p)，跳过", vt);
            return;
        }
    }
    DWORD old = 0;
    if (!VirtualProtect(vt, 16 * 4, PAGE_EXECUTE_READWRITE, &old)) return;
    for (int i = 3; i <= 10; ++i) {
        g_vtSpyOrig[i] = vt[i];
        vt[i] = g_vtSpyThunk[i];
    }
    VirtualProtect(vt, 16 * 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), vt, 16 * 4);
    g_vtSpyTable = vt;
    Log("[audio] NOTE  VTSLOT 已装：audiere OutputStream vtable %p 槽 3..10", vt);
}

// ======================================= L0b nekoHWMusic static vtable =====
// Every sound the game loads (music, ambience and - measured - the heroine's
// voice clips) goes through nekoHWMusic::play.  Patching that one static table
// entry costs nothing and cannot corrupt an instance, unlike copying instance
// vtables (which crashed the game, see voice_l1_vtable).
static void* g_realMusicPlay = NULL;
static void* g_realMusicStop = NULL;
static int   g_l0bInstalled  = 0;

// The object that reaches nekoHWMusic::play is the engine's music object, not
// audiere's OutputStream we recorded at load time; the stream pointer sits in
// its first fields.  Scan a few dwords for one of our recorded streams.
static Rec* RecFindByEmbedded(void* obj)
{
    unsigned char* p = (unsigned char*)obj;
    if (!Readable(p, 0x80)) return NULL;
    for (int off = 0; off < 0x80; off += 4) {
        void* v = *(void**)(p + off);
        if (!v) continue;
        Rec* r = RecFindObj(v);
        if (r) return r;
    }
    return NULL;
}

static void __cdecl OnMusicPlay(void* self)
{
    Rec* r = RecFindObj(self);
    if (!r) r = RecFindByEmbedded(self);
    if (!r) { Log("[audio] MUSIC_PLAY t=%u obj=%p (no record)", NowMs(), self); return; }
    Log("[audio] MUSIC_PLAY t=%u obj=%p size=%d dur_ms=%d rate=%d ch=%d cat=%s magic=%s",
        NowMs(), self, r->size, r->durMs, r->rate, r->ch, kCatName[r->cat], r->magic);

    // 配音判据（启发式，实测标定）：台词出现后短时间内出现"中等体量、持续 >=1.5 s"的播放
    //   带配音台词 = LINE_START 后 0~2500 ms 内出现一次 MUSIC_PLAY，且 dur_ms >= 1500、size >= 15000
    //   打字机音效 size 4~12 KB（dur 0.3~1.3 s）被 size/dur 双阈值排除；BGM 换曲很少与台词起点同刻
    //   （2）排除"预载爆发"：引擎换场景时会在同一瞬间对一批音源调 play（实测 8 条/2 ms，
    //        体积从 25 KB 到 1.9 MB 不等），那不是配音；配音是孤立的单次播放。
    if (g_lineStarted && r->size >= 15000 && r->durMs >= 1500) {
        static unsigned s_playT[8] = {0};
        static int      s_playIdx = 0;
        unsigned now = NowMs();
        int burst = 0;
        for (int i = 0; i < 8; ++i) {
            if (s_playT[i] && (unsigned)(now - s_playT[i]) <= 120) { burst = 1; break; }
        }
        s_playT[s_playIdx] = now;
        s_playIdx = (s_playIdx + 1) & 7;
        unsigned lag = now - g_lineStart;
        if (!burst && lag <= 900) {
            RecordVoicePlay(CONF_HEUR, r->durMs, "");
            Log("[audio] HEUR_VOICE gen=%u lag_ms=%u size=%d dur_ms=%d",
                g_gen, lag, r->size, r->durMs);
        } else if (burst) {
            Log("[audio] HEUR_SKIP burst gen=%u size=%d dur_ms=%d", g_gen, r->size, r->durMs);
        }
    }
}

static void __cdecl OnMusicStop(void* self)
{
    (void)self;
}

static void __declspec(naked) Thunk_MusicPlay(void)
{
    __asm {
        push ecx
        push ecx
        call OnMusicPlay
        add  esp, 4
        pop  ecx
        jmp  dword ptr [g_realMusicPlay]
    }
}

static void __declspec(naked) Thunk_MusicStop(void)
{
    __asm {
        push ecx
        push ecx
        call OnMusicStop
        add  esp, 4
        pop  ecx
        jmp  dword ptr [g_realMusicStop]
    }
}

static int InstallMusicVtableHook(void)
{
    HMODULE eng = GetModuleHandleA("Engine.dll");
    if (!eng) return 0;
    void** vt = (void**)((uint8_t*)eng + ENGINE_HWMUSIC_VTABLE_RVA);
    if (!Readable(vt, 16 * 4)) return 0;
    void* p7 = vt[VS_PLAY];
    void* p8 = vt[VS_STOP];
    // The exact entry point differs between engine builds (1.50 shows play at
    // Engine+0xDB860); only require that both slots point into Engine.dll text.
    uint32_t r7 = (uint32_t)p7 - (uint32_t)eng;
    uint32_t r8 = (uint32_t)p8 - (uint32_t)eng;
    if (r7 < 0x1000u || r7 >= 0x1E5B60u || r8 < 0x1000u || r8 >= 0x1E5B60u) {
        Log("[audio] NOTE  L0b 版本不符 (slot7=%p rva=0x%X)，跳过", p7, r7);
        return 0;
    }
    Log("[audio] NOTE  L0b 入口 play=Engine+0x%X stop=Engine+0x%X", r7, r8);
    DWORD old = 0;
    if (!VirtualProtect(vt, 16 * 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    g_realMusicPlay = p7;
    g_realMusicStop = p8;
    vt[VS_PLAY] = (void*)Thunk_MusicPlay;
    vt[VS_STOP] = (void*)Thunk_MusicStop;
    VirtualProtect(vt, 16 * 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), vt, 16 * 4);
    g_l0bInstalled = 1;
    Log("[audio] NOTE  L0b 已装：nekoHWMusic vtable slot7/8 (Engine+0x%X)", ENGINE_HWMUSIC_VTABLE_RVA);
    return 1;
}

extern "C" int Audio_Init(void)
{
    SyncEnsure();
    LogInit(NULL);
    static volatile LONG inited = 0;
    if (inited) return 1;

    LoadConfig(NULL);

    HMODULE eng = GetModuleHandleA("Engine.dll");
    HMODULE aud = GetModuleHandleA("audiere.dll");
    if (!eng || !aud) {
        Log("[audio] NOTE  等待 Engine.dll/audiere.dll (eng=%p aud=%p)", eng, aud);
        return 0;
    }

    int n = 0;
    if (g_enableL1) {
        if (PatchIATOne(eng, "audiere.dll", "AdrCreateMemoryFile",
                        (void*)Hook_CreateMemoryFile, (void**)&real_CreateMemoryFile)) n++;
        if (PatchIATOne(eng, "audiere.dll", "AdrOpenSampleSourceFromFile",
                        (void*)Hook_OpenSampleSourceFromFile, (void**)&real_OpenSampleSourceFromFile)) n++;
        if (PatchIATOne(eng, "audiere.dll", "AdrOpenSound",
                        (void*)Hook_OpenSound, (void**)&real_OpenSound)) n++;
        if (PatchIATOne(eng, "audiere.dll", "AdrOpenSoundEffect",
                        (void*)Hook_OpenSoundEffect, (void**)&real_OpenSoundEffect)) n++;
        if (PatchIATOne(eng, "audiere.dll", "AdrOpenDevice",
                        (void*)NULL, (void**)&real_OpenDevice)) n++;
        if (n) { g_l1Installed = 1; }
        Log("[audio] NOTE  L1 IAT hooked %d/5 (Engine=%p audiere=%p)", n, eng, aud);
    } else {
        Log("[audio] NOTE  L1 被配置关闭 (voice_l1=0)");
    }

    if (g_enableL3) InstallEngineLoaderHook();     // L3：名字上下文（可选）
    if (g_enableL0) InstallVoiceVtableHook();      // L0：最强判据
    if (g_enableL0) InstallMusicVtableHook();      // L0b：通用播放记录（实测音乐/环境音/语音都走这里）
    else            Log("[audio] NOTE  L0 被配置关闭 (voice_l0=0)");

    if (n == 0 && !g_l0Installed) {
        Log("[audio] NOTE  安装失败：L0/L1 都没装上");
        return 0;
    }
    InterlockedExchange(&inited, 1);
    Log("[audio] NOTE  判据就绪：window=%dms preroll=%dms min_played=%dms min_conf=%d (L0=%d L1=%d L3=%d)",
        g_windowMs, g_preRollMs, g_minPlayedMs, g_minConf,
        g_l0Installed, g_l1Installed, g_l3Installed);
    return 1;
}

static DWORD WINAPI InitThread(LPVOID)
{
    for (int i = 0; i < 600; ++i) {          // 最多等 60s
        if (Audio_Init()) return 0;
        Sleep(100);
    }
    Log("[audio] NOTE  Audio_Init 超时（60s）");
    return 0;
}

extern "C" void Audio_InitAsync(void)
{
    SyncEnsure();
    LogInit(NULL);
    CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
}

extern "C" void Audio_Shutdown(void)
{
    SyncEnsure();
    if (g_l0Installed) {
        HMODULE eng = GetModuleHandleA("Engine.dll");
        if (eng) {
            void** vt = (void**)((uint8_t*)eng + ENGINE_VOICE_VTABLE_RVA);
            if (Readable(vt, 12 * 4)) {
                DWORD old = 0;
                if (VirtualProtect(vt, 12 * 4, PAGE_EXECUTE_READWRITE, &old)) {
                    vt[VS_PLAY] = g_realVoicePlay;
                    vt[VS_STOP] = g_realVoiceStop;
                    VirtualProtect(vt, 12 * 4, old, &old);
                    FlushInstructionCache(GetCurrentProcess(), vt, 12 * 4);
                }
            }
        }
        g_l0Installed = 0;
    }
    if (g_l3Installed && g_engineTarget) {
        uint8_t* t = (uint8_t*)g_engineTarget;
        DWORD old = 0;
        if (VirtualProtect(t, 8, PAGE_EXECUTE_READWRITE, &old)) {
            // 恢复原 5 字节（从 trampoline 拷回）
            if (g_tramp) memcpy(t, g_tramp, 5);
            VirtualProtect(t, 8, old, &old);
            FlushInstructionCache(GetCurrentProcess(), t, 8);
        }
        g_l3Installed = 0;
    }
    Log("[audio] NOTE  Audio_Shutdown 完成");
}

// ===================================================== 自检接口 ==========
#ifdef AUDIO_HOOK_SELFTEST
extern "C" void Audio__TestReset(void)
{
    SyncEnsure();
    EnterCriticalSection(&g_cs);
    ZeroMemory(g_evt, sizeof(g_evt));
    g_evtCursor = 0;
    g_gen = 0; g_lineStart = 0; g_lineStarted = 0; g_lineEnd = 0; g_lineLaunched = 0;
    g_finalVal = -1; g_finalGen = 0; g_lastFinalVal = -1;
    g_statVoiced = g_statUnvoiced = g_statPlays = 0;
    g_lastVoiceTick = 0; g_lastVoiceValid = 0; g_lastVoiceName[0] = 0;
    LeaveCriticalSection(&g_cs);
}

extern "C" void Audio__TestSetClock(unsigned t)
{
    InterlockedExchange(&g_testClock, (LONG)t);
    InterlockedExchange(&g_testClockOn, 1);
}

extern "C" unsigned Audio__TestNow(void) { return NowMs(); }

extern "C" void Audio__TestVoicePlay(int conf, int durMs, const char* name)
{
    RecordVoicePlay(conf, durMs, name);
}

extern "C" void Audio__TestVoiceStop(void) { RecordVoiceStop(); }

extern "C" int Audio__TestEventCount(void)
{
    int n = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < AUDIO_MAX_EVT; ++i) if (g_evt[i].conf != 0) n++;
    LeaveCriticalSection(&g_cs);
    return n;
}

extern "C" int Audio__TestDecodeName(const void* p, char* out, int cap)
{
    return DecodeWNameField((void*)p, out, cap);
}

extern "C" int Audio__TestCatFromName(const char* src)
{
    return CatFromName(src);
}

extern "C" int Audio__TestL0Play(const void* self)
{
    int before = Audio__TestEventCount();
    OnVoicePlayL0((void*)self);
    return Audio__TestEventCount() > before ? 1 : 0;
}
#endif
