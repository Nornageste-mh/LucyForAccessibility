/* =============================================================================
 * audio_hook.h -- Lucy (nkNovel 1.50) 配音判据模块 · GalA11y Forge
 *
 * 用途：编进 d3dx9_40 代理 DLL（/MT，x86），给文本层回答一个问题：
 *       “刚刚显示的这一句台词，有没有配音在播？”
 *
 * ---------------------------------------------------------------------------
 * 判据语义（定死，实现见 audio_hook.cpp 顶部注释）
 * ---------------------------------------------------------------------------
 *  VOICE_PLAY  = 一次“语音开始播放”事件。来源按可信度分级：
 *                conf=3  L0：nekoVoiceInstance::play() 被调用（Engine+0xCA710）——最强
 *                conf=2  L1：audiere OutputStream::play() 且资源名/类名证明是 Voices\*
 *                conf=1  L1：只有体积/时长启发式（默认**不采信**，只为日志）
 *                BGM / 音效（SoundEffect 路径）永远不产生 VOICE_PLAY
 *
 *  一行台词的生命周期：
 *      Audio_OnLineStarted()   -> 文本层检测到“新行开始”（打起点）
 *      Audio_OnLineLaunched()  -> 一行已完整、即将朗读/判断（打终点）
 *      Audio_WasLineVoiced()   -> 取判定
 *
 *  判定规则：
 *      有配音 := 存在一个 conf>=minConf 的 VOICE_PLAY，其 tPlay 落在
 *                [ 行起点 - preroll_ms , 行终点 + window_ms ] 内，
 *                且该事件没有被“过早 stop”作废（played < min_played_ms），
 *                且该事件没有被更早的一行消费掉（一条语音只归属一行）。
 *      否则 := 无配音。
 *
 *  返回值（Audio_WasLineVoiced）：
 *       1  = 有配音     -> 文本层：不自动朗读（角色名也不念），但可重读
 *       0  = 无配音     -> 文本层：自动朗读
 *      -1  = 还没定论   -> 文本层：等 Audio_RecommendDelayMs() 后再问一次
 *                          （绝不提前返回 1，宁可漏判成旁白，不可把该念的吞掉）
 *
 *  时间窗口默认值（可被 a11y\config.ini 覆盖，见下）：
 *      voice_window_ms    = 1200   行结束后继续观察多久
 *      voice_preroll_ms   = 150    行开始前多久内的语音也算本行
 *      voice_min_played_ms= 100    短于此就被 stop 的语音视为“假启动”作废
 *      voice_min_dur_ms   = 250    已知总时长短于此的，不当台词
 *      voice_min_conf     = 2      只采信 conf>=2 的事件
 *      voice_log          = 1      写日志
 *      voice_require_name = 0      1=要求资源名必须以 Voices\ 开头才算语音（更严；
 *                                  默认 0，因为 L0 按实例类判定已足够，且个别语音
 *                                  资源名可能不在 Voices\ 下）。资源名解出 BGMs\/FXs\
 *                                  时无论该键如何都直接丢弃（误判≈0 优先）。
 *      voice_l0/voice_l1/voice_l3 = 1  三层钩子开关（默认全开）
 *
 *  资源名解码：nekoHWMusic 实例 +0x2C 是 std::wstring（32 位：16B 内联缓冲 + size + cap），
 *  解码器两种布局都试、只接受全 ASCII；解不出就记 <raw:...> 且不参与判定。
 *
 *  config.ini 位置：<游戏exe目录>\a11y\config.ini（不存在则试 <代理DLL目录>\a11y\config.ini）
 *      格式（键可写在任意 section 下，按行扫描，`#`/`;` 开头为注释）：
 *          [a11y]
 *          voice_window_ms=1200
 *          voice_preroll_ms=150
 *          voice_min_conf=2
 *
 * ---------------------------------------------------------------------------
 * 线程/调用约定
 * ---------------------------------------------------------------------------
 *  - 所有函数线程安全（内部 CRITICAL_SECTION）。
 *  - Audio_Init() 不要在 DllMain 里直接调用：它会 GetModuleHandle("Engine.dll")，
 *    代理 DLL 被加载时 Engine.dll 可能还没进内存。请在 DllMain 里起一个线程，
 *    或调用 Audio_InitAsync()；Audio_Init() 返回 0 表示“还没就绪，稍后重试”。
 *  - Audio_OnLineStarted/Launched/WasLineVoiced 从 D3D 渲染线程调用即可（无阻塞）。
 *  - Backspace 重读：文本层自己缓存每行结果即可；Audio_GetLastFinalVerdict()
 *    额外提供“最近一次已定论的行”的结果，便于重读时无需重算。
 * ========================================================================== */
#ifndef LUCY_AUDIO_HOOK_H
#define LUCY_AUDIO_HOOK_H

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_JUDGE_VOICED      1
#define AUDIO_JUDGE_UNVOICED    0
#define AUDIO_JUDGE_UNDECIDED  (-1)

/* 安装钩子。成功返回 1；0 = Engine.dll/audiere.dll 还没加载或版本不符（可稍后重试）。 */
int  Audio_Init(void);

/* 起一个后台线程重试 Audio_Init（最多 ~60s）。d3dx9 代理的 DllMain 里调这个即可。 */
void Audio_InitAsync(void);

/* 卸载：恢复被改的 vtable / IAT / 内联 patch，并停用判据。可重复调用。 */
void Audio_Shutdown(void);

/* 关键 API：返回 AUDIO_JUDGE_VOICED / AUDIO_JUDGE_UNVOICED / AUDIO_JUDGE_UNDECIDED */
int  Audio_WasLineVoiced(void);

/* 文本层：检测到“新的一行开始显示”时调用（打起点，清空上一行状态）。 */
void Audio_OnLineStarted(void);

/* 文本层：一行已完整、即将朗读/判断时调用（打终点）。 */
void Audio_OnLineLaunched(void);

/* 判据窗口（行结束后观察多久）。clamp 到 [100, 5000]，返回实际生效值。 */
int  Audio_SetWindow(int ms);
int  Audio_GetWindow(void);

/* 行开始前置量（行开始前多久内的语音也算本行）。clamp 到 [0, 1000]。 */
int  Audio_SetPreRoll(int ms);

/* 最近一次 VOICE_PLAY 的 GetTickCount 时刻；从未有过返回 0。 */
int  Audio_GetLastTick(void);

/* 最近一次已定论行的结果（1/0；还没定论过返回 -1）。Backspace 重读用。 */
int  Audio_GetLastFinalVerdict(void);

/* 当前是否有语音在播（1/0）。可用于“语音播放期间不要抢读”。 */
int  Audio_IsVoicePlaying(void);

/* 现在调用 Audio_WasLineVoiced() 还差多少毫秒才有定论；已可定论返回 0。 */
int  Audio_RecommendDelayMs(void);

/* 最近一次 VOICE_PLAY 的资源名（形如 "Voices\lucy0001_jp.mp3"），无则空串。 */
int  Audio_GetLastVoiceName(char* out, int cap);

/* 诊断计数：*voiced/*unvoiced/*plays 可为 NULL。 */
void Audio_GetStats(int* voiced, int* unvoiced, int* plays);

/* 日志文件全路径（<DLL目录>\audio_hook.log），返回长度；cap 不足返回 -1。 */
int  Audio_GetLogPath(char* out, int cap);

/* ---------------------------------------------------------------------------
 * 自检专用（仅 -DAUDIO_HOOK_SELFTEST 时编译进来；正式补丁 DLL 不要定义它）
 * ------------------------------------------------------------------------- */
#ifdef AUDIO_HOOK_SELFTEST
void Audio__TestReset(void);                                  /* 清状态/事件，保持配置 */
void Audio__TestSetClock(unsigned t);                         /* 接管 GetTickCount */
unsigned Audio__TestNow(void);
void Audio__TestVoicePlay(int conf, int durMs, const char* name); /* 模拟 VOICE_PLAY */
void Audio__TestVoiceStop(void);                              /* 模拟 VOICE_STOP */
int  Audio__TestEventCount(void);                             /* 事件环形缓冲里的条数 */

/* 资源名解码：p 指向一个"nekoHWMusic 布局"的内存（+0x2C 放 std::wstring）。
 * 成功返回名字长度，失败 0 且 out 为空串。用于验证 wstring 内联/堆两种布局。 */
int  Audio__TestDecodeName(const void* p, char* out, int cap);

/* 资源名前缀分类：返回 0=unknown 1=voice 2=bgm 3=sfx（与日志 cat= 一致）。 */
int  Audio__TestCatFromName(const char* src);

/* 直接喂一条 L0 事件（跳过内联汇编 thunk）：用于验证"名字非语音则忽略"的分支。
 * 返回 1=已记录 0=被忽略。self 必须是 nekoHWMusic 布局的对象（+0x74 放裸指针）。 */
int  Audio__TestL0Play(const void* self);
#endif

#ifdef __cplusplus
}
#endif

#endif /* LUCY_AUDIO_HOOK_H */
