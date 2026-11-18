// ============================================================================
//  speech.h —— GalA11y Forge 语音后端层（x86 / 静态链接 / 纯 Win32+COM）
//
//  这一版是「直接编进游戏补丁 DLL」的形态：
//    * 没有 __declspec(dllexport)，也不需要 .def / .lib —— 和补丁源码一起编译即可；
//    * /MT 静态 CRT，不跨 DLL 边界抛 C++ 异常；
//    * 外部依赖只有 USER32 / ole32 / ADVAPI32 / KERNEL32（SAPI 走进程内 COM）；
//    * 第三方读屏 DLL（nvdaControllerClient / ZDSRAPI / Tolk）全部运行时按绝对路径
//      LoadLibrary，找不到就安静降级，不产生"少一个文件就崩"的硬依赖。
//
//  【绝对不要在 DllMain 里调 Speech_Init】
//     内部要 CreateThread / LoadLibrary / CoCreateInstance —— 加载器锁下做这些会死锁。
//     正确做法：DllMain 只保存 hModule 直接 return TRUE；另起一个线程（或等注入器
//     LoadLibrary 返回之后）再调 Speech_Init。
//
//  线程模型：
//     Speech_Init 起一个内部 worker 线程，worker 负责 STA(CoInitializeEx)、后端探测、
//     所有第三方调用和 SAPI 朗读；游戏线程调 Speak/Stop/Cycle 只是投递请求，永不阻塞
//     （Cycle 会等 worker 把新后端名念完，最多等 3 秒，按键场景可接受）。
//
//  后端环（cycle 顺序 = 探测顺序 = 自动选择顺序）：
//     NVDA → 争渡(ZDSR) → SAPI → Tolk
//     Tolk 属可选组件（需玩家自行部署 Tolk.dll），默认排最后；没部署就跳过。
//
//  配置：<游戏目录>\a11y\config.ini（不存在则用默认值并写出）
//        backend=auto|nvda|zdsr|sapi|tolk   rate=0..100   volume=0..100
//        re_read_key=8 (Backspace 的 VK 码) backend_key=Alt+Shift+F9
//  日志：<游戏目录>\a11y\a11y.log（追加、毫秒时间戳、UTF-8 带 BOM）
//
//  并发（重要，v3 加固）：
//     Speech_Speak / Speech_SpeakImmediate / Speech_Stop 会被多个线程同时调用
//     （低层键盘钩子线程念"重读/切后端"、TickThread 念剧情、菜单朗读）。内部保证：
//       * 剧情朗读（Speech_Speak）走 FIFO 队列（32 句），一次只投一句，两句剧情永不重叠；
//         队列满时丢最旧的一句（统计里记 dropped，日志节流记录）。
//       * 短播报（Speech_SpeakImmediate，菜单/热键反馈）只保留最新一条，允许打断剧情。
//       * Speech_Stop 全局生效：清空队列 + 打断当前这句。
//     所有后端调用都只发生在内部 worker 线程，第三方 API 天然串行（不会两句话
//     同时喂给同一个读屏器）。调用方只需保证不在 DllMain 里调。
//
//  朗读节奏（v4，直投语义）：
//     * NVDA / Tolk = 直投（direct feed）：speakText / Tolk_Speak 异步投递、读屏器自己排队，
//       投递成功即视为本句完成，立刻取下一句 —— 不按字数估算等待，台词密集时不丢句。
//     * ZDSR / SAPI = 状态查询：等 GetSpeakState()∈{3,4} / ISpVoice::GetStatus 显示念完再取下句。
//     * 打断边界：Speech_SpeakImmediate 对直投后端先 cancelSpeech / Tolk_Silence 再投，
//       因此菜单/热键播报会立刻盖掉正在念的剧情（这是设计意图）；剧情之间不互相打断。
//     * 例外：自愈降级时"已切换到 XX"走得是 announce 通道，会打断当前剧情句，然后
//       失败的那句会重新排队念一遍 —— 玩家最多重听一句，不会丢句。
//     a11y.log 里有一行 `speech: backend=<名字> direct_feed=0/1`，可用来复盘当次走的是哪种节奏。
//
//  语义：
//     Speech_Speak            = 剧情台词（排队、不重叠）
//     Speech_SpeakImmediate   = 界面/按键反馈（打断式，允许插话）
// ============================================================================
#ifndef GAL_A11Y_SPEECH_MODULE_H
#define GAL_A11Y_SPEECH_MODULE_H

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

// 后端编号（Speech_CycleBackend / Speech_BackendId 的返回值；稳定，别改）
#define SPEECH_BACKEND_NONE 0
#define SPEECH_BACKEND_NVDA 1
#define SPEECH_BACKEND_ZDSR 2
#define SPEECH_BACKEND_SAPI 3
#define SPEECH_BACKEND_TOLK 4

// ---------------------------------------------------------------------------
//  主接口（补丁只需要这 6 个）
// ---------------------------------------------------------------------------

// 初始化：读配置、建 <游戏目录>\a11y\、开日志、起 worker 线程并探测后端。
// 可在任意线程调用（不要在 DllMain 里）；幂等；最多阻塞 ~5 秒（典型 < 100 ms）。
// 返回 1=至少有一个可用后端，0=没有（此时朗读调用是安全空操作）。
int Speech_Init(void);

// 朗读一段 UTF-16 剧情台词。非阻塞。
// 进入内部 FIFO 队列：上一句念完才念下一句（两句剧情不会互相打断）；
// 队列满（16 句）时丢弃最旧的一句。NULL/空串=什么都不做。
// 首次调用时若还没 Init 会自动 Init（仍然不要在 DllMain 里调）。
void Speech_Speak(const wchar_t *utf16);

// 朗读一段"短播报"（菜单项、按键反馈、"已切换到 XX"这类）。
// 非阻塞；只保留最新一条（覆盖式），允许打断正在念的剧情。NULL/空串=什么都不做。
void Speech_SpeakImmediate(const wchar_t *utf16);

// 立刻停止当前朗读（非阻塞，worker 在下一次循环里执行，延迟通常 < 10 ms）。
void Speech_Stop(void);

// 切到下一个可用后端，并把新后端名念出来（产品要求）。返回新后端编号（SPEECH_BACKEND_*）。
// 会重探之前判不可用的后端（例如玩家中途启动了 NVDA），所以玩家能自己切回来。
int Speech_CycleBackend(void);

// 当前后端中文名（静态字符串，永远非 NULL；没后端时返回 L"无"）。给日志/配置/提示用。
const wchar_t *Speech_BackendName(void);

// 关朗读、退 worker 线程、释放 COM/DLL。幂等；之后再 Init 可以重新开始。
void Speech_Shutdown(void);

// ---------------------------------------------------------------------------
//  辅助接口（补丁的配置界面/自检/问题排查用，可选）
// ---------------------------------------------------------------------------

int Speech_BackendId(void);              // 当前后端编号（SPEECH_BACKEND_*）
int Speech_IsReady(void);                // 1=worker 已就绪且有可用后端
const wchar_t *Speech_LastError(void);   // 最近一次失败原因（人话，永远非 NULL）
const wchar_t *Speech_LogPath(void);     // <游戏目录>\a11y\a11y.log 的绝对路径
const wchar_t *Speech_ConfigPath(void);  // <游戏目录>\a11y\config.ini 的绝对路径
const wchar_t *Speech_GameDir(void);     // 探测到的游戏目录（带结尾反斜杠）

// 读配置项（不区分大小写）。int 版容忍前后空格；str 版返回内部静态缓冲（别释放）。
int Speech_ConfigInt(const wchar_t *key, int def);
const wchar_t *Speech_ConfigStr(const wchar_t *key, const wchar_t *def);

// 按 key 名字切后端（"nvda"/"zdsr"/"sapi"/"tolk"）；1=成功。会念后端名。
int Speech_SelectBackend(const wchar_t *key);

// 运行统计（自测/问题排查用；多线程安全，随时可读）。which 用下面的 SPEECH_STAT_*。
#define SPEECH_STAT_STORY_ENQ        0   // 剧情入队次数（含失败后重排的）
#define SPEECH_STAT_STORY_SPOKEN     1   // 剧情真正念出去几次
#define SPEECH_STAT_STORY_DROPPED    2   // 剧情丢弃次数（队列满/后端失败/Stop 清空）
#define SPEECH_STAT_PENDING          3   // 当前队列里还等着念几句
#define SPEECH_STAT_ANNOUNCE_ENQ     4   // 短播报提交次数
#define SPEECH_STAT_ANNOUNCE_SPOKEN  5   // 短播报念出去次数（含 cycle 念后端名）
unsigned long Speech_Stat(int which);

#ifdef __cplusplus
}
#endif

#endif // GAL_A11Y_SPEECH_MODULE_H
