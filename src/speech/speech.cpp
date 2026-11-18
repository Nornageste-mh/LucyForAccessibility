// ============================================================================
//  speech.cpp —— GalA11y Forge 语音后端层（静态链接进补丁 DLL 的形态）
//
//  后端环（cycle/探测/自动选择同序）：NVDA → 争渡(ZDSR) → SAPI → Tolk
//  全部逻辑跑在一个自建 worker 线程里：
//      * 该线程是 STA（CoInitializeEx(COINIT_APARTMENTTHREADED)）—— SAPI 的 ISpVoice
//        只在这条线程上创建和使用，因此不需要跨套间封送；
//      * 线程里带消息泵（MsgWaitForMultipleObjects + PeekMessage），满足 STA 要求；
//      * LoadLibrary / CoCreateInstance / 第三方朗读 API 全在线程里跑，游戏线程只投递
//        请求（Speech_Speak/Stop 立即返回），因此不会卡帧。
//
//  本文件不导出任何符号（补丁把 speech.cpp 直接编进去即可），/MT，不用 STL 跨边界，
//  所有公开函数都不抛异常，内部只有一把 CRITICAL_SECTION + 几个事件。
//
//  朗读节奏（2026-11-18 第四轮拍板，Lead m00684）：
//      * 剧情两句"不重叠"由后端能力实现：
//          - 直投后端 NVDA / Tolk：speakText / Tolk_Speak 是异步投递，读屏器自己排队；
//            投递成功即视为本句完成，worker 立刻取下一句（不再按 6-7 秒时长估算等待，
//            那是第三轮为了"不重叠"的保守兜底，代价是台词密集时丢句）。
//          - ZDSR / SAPI：保留状态查询等念完（GetSpeakState()∈{3,4} / ISpVoice::GetStatus），
//            因为它们的返回值能可靠区分"念了"和"没念成"，自愈计数依赖这个。
//      * 短播报（Speech_SpeakImmediate：菜单/热键/切换提示）永远可以打断剧情：
//        直投后端先 cancelSpeech / Tolk_Silence 再投，ZDSR/SAPI 用各自的"打断"参数。
//      * Speech_Stop 全局生效（清队列 + 打断当前句）。
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <objbase.h>
#include <sapi.h>
#include <wchar.h>
#include <wctype.h>
#include <stdarg.h>
#include <stdio.h>
#include "speech.h"

// 只用这 4 个系统 DLL（uuid.lib 只是 GUID 数据，不产生 DLL 依赖）
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uuid.lib")

#define A11Y_MAX_TEXT      4096                 // 单条文本上限（含结尾 0）
#define A11Y_SLOTS         4
#define A11Y_LOG_MAX       2048
#define CFG_MAX_KEYS       32
#define CFG_KEY_CCH        48
#define CFG_VAL_CCH        160

// ---------------------------------------------------------------------------
//  路径：游戏目录 = 宿主 EXE 所在目录（补丁是 d3dx9_40.dll 代理，宿主就是游戏 exe）。
//  另外支持环境变量 A11Y_GAME_DIR 覆盖（自检/排查用）。
// ---------------------------------------------------------------------------
static wchar_t g_gameDir[MAX_PATH];     // 带结尾反斜杠
static wchar_t g_a11yDir[MAX_PATH];     // <game>\a11y\ 带结尾反斜杠
static wchar_t g_cfgPath[MAX_PATH];
static wchar_t g_logPath[MAX_PATH];

static void WCopy(wchar_t *dst, size_t cch, const wchar_t *src)
{
	if(!dst || cch == 0) return;
	if(!src) { dst[0] = 0; return; }
	size_t i = 0;
	for(; i + 1 < cch && src[i]; i++) dst[i] = src[i];
	dst[i] = 0;
}

static void WCat(wchar_t *dst, size_t cch, const wchar_t *src)
{
	if(!dst || cch == 0 || !src) return;
	size_t n = wcslen(dst);
	if(n >= cch - 1) return;
	WCopy(dst + n, cch - n, src);
}

// 注意：绝不要只用 _snwprintf_s(buf, cch, _TRUNCATE, fmt, ...) 的"数组版模板重载"，
// 它会静默匹配 CRT 模板重载并输出空串（SenrenBanka 项目踩过，见其 zdsr_api.h:124-132）。
// 这里统一先把 buf 转成指针再调。
static void Fmt(wchar_t *buf, size_t cch, const wchar_t *fmt, ...)
{
	if(!buf || cch == 0) return;
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf_s((wchar_t *)buf, cch, _TRUNCATE, fmt, ap);
	va_end(ap);
	buf[cch - 1] = 0;
}

static bool StrContainsNoCase(const wchar_t *hay, const wchar_t *needle)
{
	if(!hay || !needle || !needle[0]) return false;
	for(; *hay; hay++)
	{
		const wchar_t *h = hay, *n = needle;
		while(*h && *n && towlower(*h) == towlower(*n)) { h++; n++; }
		if(!*n) return true;
	}
	return false;
}

// 取 dirname（带结尾反斜杠）
static bool DirOf(const wchar_t *filePath, wchar_t *out, size_t cch)
{
	if(!filePath || !out || cch < 4) return false;
	WCopy(out, cch, filePath);
	for(size_t i = wcslen(out); i > 0; i--)
	{
		if(out[i - 1] == L'\\' || out[i - 1] == L'/') { out[i] = 0; return true; }
	}
	out[0] = 0;
	return false;
}

static void BuildPaths(void)
{
	g_gameDir[0] = 0;
	wchar_t env[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"A11Y_GAME_DIR", env, MAX_PATH);
	if(n > 0 && n < MAX_PATH)
	{
		WCopy(g_gameDir, MAX_PATH, env);
		size_t len = wcslen(g_gameDir);
		if(len > 0 && g_gameDir[len - 1] != L'\\') WCat(g_gameDir, MAX_PATH, L"\\");
	}
	else
	{
		wchar_t exe[MAX_PATH];
		DWORD m = GetModuleFileNameW(NULL, exe, MAX_PATH);   // 宿主 EXE = 游戏 exe
		if(m > 0 && m < MAX_PATH) DirOf(exe, g_gameDir, MAX_PATH);
	}
	if(!g_gameDir[0]) WCopy(g_gameDir, MAX_PATH, L".\\");

	Fmt(g_a11yDir, MAX_PATH, L"%sa11y\\", g_gameDir);
	Fmt(g_cfgPath, MAX_PATH, L"%sconfig.ini", g_a11yDir);
	Fmt(g_logPath, MAX_PATH, L"%sa11y.log", g_a11yDir);
}

// 建 <game>\a11y\（逐级建，不用 shell32）
static void EnsureA11yDir(void)
{
	if(!g_gameDir[0]) return;
	if(g_gameDir[0] != L'.')
	{
		wchar_t tmp[MAX_PATH * 2];
		WCopy(tmp, MAX_PATH * 2, g_gameDir);
		CreateDirectoryW(tmp, NULL);
	}
	CreateDirectoryW(g_a11yDir, NULL);
}

// ---------------------------------------------------------------------------
//  UTF-8 / UTF-16 转换（日志和 ini 都用 UTF-8）
// ---------------------------------------------------------------------------
static int WideToUtf8(const wchar_t *w, char *out, int outBytes)
{
	if(outBytes < 4) return 0;
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outBytes, NULL, NULL);
	if(n <= 0) { out[0] = 0; return 0; }
	return n - 1;
}

static void BytesToWide(const void *data, DWORD bytes, wchar_t *out, size_t cch)
{
	if(!out || cch == 0) { if(out) out[0] = 0; return; }
	out[0] = 0;
	if(!data || bytes < 2) return;

	const unsigned char *p = (const unsigned char *)data;
	if(bytes >= 2 && p[0] == 0xFF && p[1] == 0xFE)          // UTF-16LE BOM
	{
		const wchar_t *w = (const wchar_t *)(p + 2);
		size_t c = (bytes - 2) / sizeof(wchar_t);
		size_t i = 0;
		for(; i + 1 < cch && i < c; i++) out[i] = w[i];
		out[i] = 0;
		return;
	}
	const char *bytes8 = (const char *)p;
	int skip = 0;
	if(bytes >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) skip = 3;
	int n = MultiByteToWideChar(CP_UTF8, 0, bytes8 + skip, (int)(bytes - skip), out, (int)cch - 1);
	if(n <= 0)
	{
		n = MultiByteToWideChar(CP_ACP, 0, bytes8 + skip, (int)(bytes - skip), out, (int)cch - 1);
		if(n <= 0) { out[0] = 0; return; }
	}
	out[n] = 0;
}

// ---------------------------------------------------------------------------
//  日志：<game>\a11y\a11y.log，UTF-8 带 BOM，追加，毫秒时间戳。
//  每行独立 open/append/close —— 崩溃/被杀时最后几行也不会丢。
// ---------------------------------------------------------------------------
static volatile LONG g_logReady = 0;

static void LogLine(const wchar_t *fmt, ...)
{
	wchar_t msg[A11Y_LOG_MAX];
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf_s((wchar_t *)msg, (size_t)A11Y_LOG_MAX, _TRUNCATE, fmt, ap);
	va_end(ap);
	msg[A11Y_LOG_MAX - 1] = 0;

	SYSTEMTIME st;
	GetLocalTime(&st);
	wchar_t line[2200];
	Fmt(line, 2200, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u][%5lu] %s",
	    (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
	    (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
	    (unsigned)st.wMilliseconds, (unsigned long)GetCurrentThreadId(), msg);

	OutputDebugStringW(line);
	OutputDebugStringW(L"\r\n");

	if(InterlockedCompareExchange(&g_logReady, 0, 0) == 0 || !g_logPath[0]) return;

	wchar_t both[2300];
	Fmt(both, 2300, L"%s\r\n", line);
	char utf8[6000];
	int nb = WideToUtf8(both, utf8, 6000);

	HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if(h == INVALID_HANDLE_VALUE) return;
	LARGE_INTEGER sz;
	sz.QuadPart = 0;
	if(GetFileSizeEx(h, &sz) && sz.QuadPart == 0)
	{
		static const unsigned char kBom[3] = { 0xEF, 0xBB, 0xBF };
		DWORD w = 0;
		WriteFile(h, kBom, 3, &w, NULL);
	}
	DWORD wr = 0;
	if(nb > 0) WriteFile(h, utf8, (DWORD)nb, &wr, NULL);
	CloseHandle(h);
}

// ---------------------------------------------------------------------------
//  全局锁（InitOnce，任何线程安全；回调里不做可能失败的事）
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_cs;
static INIT_ONCE g_csOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK CsInit(PINIT_ONCE, PVOID, PVOID *)
{
	InitializeCriticalSection(&g_cs);
	return TRUE;
}
static void EnterCs(void)
{
	InitOnceExecuteOnce(&g_csOnce, CsInit, NULL, NULL);
	EnterCriticalSection(&g_cs);
}
static void LeaveCs(void)
{
	LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------------------
//  config.ini 读写（极简；不引第三方）
// ---------------------------------------------------------------------------
struct CfgItem
{
	wchar_t key[CFG_KEY_CCH];
	wchar_t val[CFG_VAL_CCH];
};
static CfgItem g_cfg[CFG_MAX_KEYS];
static int     g_cfgCount = 0;

static void CfgStore(const wchar_t *key, const wchar_t *val)
{
	if(!key || !key[0] || g_cfgCount >= CFG_MAX_KEYS) return;
	WCopy(g_cfg[g_cfgCount].key, CFG_KEY_CCH, key);
	WCopy(g_cfg[g_cfgCount].val, CFG_VAL_CCH, val);
	g_cfgCount++;
}

static void CfgParse(const wchar_t *text)
{
	g_cfgCount = 0;
	const wchar_t *p = text;
	while(p && *p)
	{
		const wchar_t *eol = p;
		while(*eol && *eol != L'\n' && *eol != L'\r') eol++;

		const wchar_t *s = p;
		while(s < eol && (*s == L' ' || *s == L'\t')) s++;
		if(s < eol && *s != L';' && *s != L'#')
		{
			const wchar_t *eq = s;
			while(eq < eol && *eq != L'=') eq++;
			if(eq < eol)
			{
				const wchar_t *ke = eq;
				while(ke > s && (ke[-1] == L' ' || ke[-1] == L'\t')) ke--;
				const wchar_t *vs = eq + 1;
				while(vs < eol && (*vs == L' ' || *vs == L'\t')) vs++;
				const wchar_t *ve = eol;
				while(ve > vs && (ve[-1] == L' ' || ve[-1] == L'\t')) ve--;

				wchar_t k[CFG_KEY_CCH], v[CFG_VAL_CCH];
				size_t kn = (size_t)(ke - s);
				if(kn > 0 && kn < CFG_KEY_CCH - 1)
				{
					for(size_t i = 0; i < kn; i++) k[i] = s[i];
					k[kn] = 0;
					size_t vn = (size_t)(ve - vs);
					if(vn >= CFG_VAL_CCH) vn = CFG_VAL_CCH - 1;
					for(size_t i = 0; i < vn; i++) v[i] = vs[i];
					v[vn] = 0;
					CfgStore(k, v);
				}
			}
		}
		p = (*eol) ? eol + 1 : eol;
	}
}

static bool FileReadAllW(const wchar_t *path, wchar_t *out, size_t cchOut)
{
	out[0] = 0;
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if(h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER sz;
	sz.QuadPart = 0;
	if(!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 64 * 1024)
	{
		CloseHandle(h);
		return false;
	}
	DWORD bytes = (DWORD)sz.QuadPart;
	void *buf = HeapAlloc(GetProcessHeap(), 0, bytes);
	if(!buf) { CloseHandle(h); return false; }
	DWORD rd = 0;
	bool ok = ReadFile(h, buf, bytes, &rd, NULL) && rd > 0;
	CloseHandle(h);
	if(ok) BytesToWide(buf, rd, out, cchOut);
	HeapFree(GetProcessHeap(), 0, buf);
	return ok && out[0] != 0;
}

static void FileWriteAllW(const wchar_t *path, const wchar_t *text, bool withBom)
{
	char *utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, 32 * 1024);
	if(!utf8) return;
	int n = WideToUtf8(text, utf8, 32 * 1024);
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if(h == INVALID_HANDLE_VALUE) { HeapFree(GetProcessHeap(), 0, utf8); return; }
	DWORD wr = 0;
	if(withBom)
	{
		static const unsigned char kBom[3] = { 0xEF, 0xBB, 0xBF };
		WriteFile(h, kBom, 3, &wr, NULL);
	}
	if(n > 0) WriteFile(h, utf8, (DWORD)n, &wr, NULL);
	CloseHandle(h);
	HeapFree(GetProcessHeap(), 0, utf8);
}

static const wchar_t *kDefaultIni =
	L"; =====================================================================\r\n"
	L";  GalA11y Forge —— 无障碍语音配置（改完重开游戏生效）\r\n"
	L"; =====================================================================\r\n"
	L";\r\n"
	L";  backend  语音后端：auto / nvda / zdsr / sapi / tolk\r\n"
	L";            auto = 自动按 NVDA → 争渡读屏 → SAPI → Tolk 的顺序挑第一个可用的\r\n"
	L";  rate     语速 0..100（50=系统默认）。只有 SAPI 后端真正生效；\r\n"
	L";            NVDA / 争渡 / Tolk 的客户端 dll 没有速率 API，请在读屏器自己的设置里调\r\n"
	L";  volume   音量 0..100。同上，只有 SAPI 后端真正生效\r\n"
	L";  re_read_key   重读当前台词键的虚拟键码（8 = Backspace）\r\n"
	L";  backend_key   切换语音后端的组合键（默认 Alt+Shift+F9）\r\n"
	L";  fail_threshold 连续朗读失败几次就判定该后端失效并自动降级（1..10，默认 3）\r\n"
	L";\r\n"
	L"backend=auto\r\n"
	L"rate=50\r\n"
	L"volume=100\r\n"
	L"re_read_key=8\r\n"
	L"backend_key=Alt+Shift+F9\r\n"
	L"fail_threshold=3\r\n";

static void CfgLoad(void)
{
	g_cfgCount = 0;
	if(!g_cfgPath[0]) return;
	EnsureA11yDir();

	wchar_t text[16 * 1024];
	if(FileReadAllW(g_cfgPath, text, 16 * 1024))
	{
		CfgParse(text);
	}
	if(g_cfgCount == 0)
	{
		// 不存在 / 空文件 / 读不了：写出默认配置（并直接采用默认值）
		FileWriteAllW(g_cfgPath, kDefaultIni, true);
		CfgParse(kDefaultIni);
		LogLine(L"配置: 已写出默认 config.ini -> %s", g_cfgPath);
	}
}

static const wchar_t *CfgGet(const wchar_t *key)
{
	if(!key) return NULL;
	for(int i = 0; i < g_cfgCount; i++)
		if(_wcsicmp(g_cfg[i].key, key) == 0) return g_cfg[i].val;
	return NULL;
}

static int CfgGetInt(const wchar_t *key, int def)
{
	const wchar_t *v = CfgGet(key);
	if(!v || !v[0]) return def;
	int n = 0, sign = 1;
	const wchar_t *p = v;
	while(*p == L' ' || *p == L'\t') p++;
	if(*p == L'-') { sign = -1; p++; }
	else if(*p == L'+') p++;
	bool any = false;
	for(; *p >= L'0' && *p <= L'9'; p++)
	{
		n = n * 10 + (int)(*p - L'0');
		any = true;
		if(n > 1000000) break;
	}
	if(!any) return def;
	return sign * n;
}

static const wchar_t *CfgGetStr(const wchar_t *key, const wchar_t *def)
{
	const wchar_t *v = CfgGet(key);
	return (v && v[0]) ? v : def;
}

// ---------------------------------------------------------------------------
//  后端基础
// ---------------------------------------------------------------------------
struct Slot
{
	int  id;
	bool available;
	wchar_t detail[192];   // 可用：加载到的 dll/语音；不可用：原因摘要
};
static Slot g_slot[A11Y_SLOTS];

// 环：0=NVDA 1=ZDSR 2=SAPI 3=TOLK
static const int kRingId[A11Y_SLOTS] = {
	SPEECH_BACKEND_NVDA, SPEECH_BACKEND_ZDSR, SPEECH_BACKEND_SAPI, SPEECH_BACKEND_TOLK
};
static const wchar_t *kRingName[A11Y_SLOTS] = {
	L"NVDA", L"争渡读屏", L"SAPI 语音", L"Tolk"
};

static int RingIndexOfId(int id)
{
	for(int i = 0; i < A11Y_SLOTS; i++) if(kRingId[i] == id) return i;
	return -1;
}

static int ParseBackendKey(const wchar_t *s)
{
	if(!s || !s[0]) return SPEECH_BACKEND_NONE;
	if(StrContainsNoCase(s, L"nvda")) return SPEECH_BACKEND_NVDA;
	if(StrContainsNoCase(s, L"zdsr") || wcsstr(s, L"争渡")) return SPEECH_BACKEND_ZDSR;
	if(StrContainsNoCase(s, L"sapi")) return SPEECH_BACKEND_SAPI;
	if(StrContainsNoCase(s, L"tolk")) return SPEECH_BACKEND_TOLK;
	return SPEECH_BACKEND_NONE;
}

// ===========================================================================
//  NVDA 后端：nvdaControllerClient.dll（x86，__stdcall，干净导出名）
//      testIfRunning() -> 0 表示 NVDA 在跑
//      speakText(const wchar_t*) -> 0 成功；自动打断当前句
//      cancelSpeech()
//      getProcessId(unsigned long*)   [可选]
//      isSpeaking(int*)               [可选；新客户端 3.0 才有]
// ===========================================================================
typedef int (__stdcall *NvdaTestFn)(void);
typedef int (__stdcall *NvdaSpeakFn)(const wchar_t *);
typedef int (__stdcall *NvdaCancelFn)(void);
typedef int (__stdcall *NvdaGetPidFn)(unsigned long *);
typedef int (__stdcall *NvdaIsSpeakingFn)(int *);

struct NvdaApi
{
	HMODULE dll;
	NvdaTestFn test;
	NvdaSpeakFn speak;
	NvdaCancelFn cancel;
	NvdaGetPidFn getPid;
	NvdaIsSpeakingFn isSpeaking;
	wchar_t path[MAX_PATH];
	int pid;
};
static NvdaApi g_nvda;

static void AppendPath(wchar_t paths[][MAX_PATH], int *n, int maxN, const wchar_t *p)
{
	if(!p || !p[0] || !n || *n >= maxN) return;
	if(wcslen(p) >= MAX_PATH) return;
	for(int i = 0; i < *n; i++) if(_wcsicmp(paths[i], p) == 0) return;
	WCopy(paths[*n], MAX_PATH, p);
	(*n)++;
}

// 统一候选目录：<game>\a11y\ 优先（补丁自己的文件），其次 <game>\plugin\，
// 最后 <game>\ 根目录 —— 全部绝对路径，从不依赖 PATH / 当前工作目录。
static void AppendGameDirs(wchar_t paths[][MAX_PATH], int *n, int maxN, const wchar_t *fileName)
{
	wchar_t p[MAX_PATH];
	Fmt(p, MAX_PATH, L"%s%s", g_a11yDir, fileName);          AppendPath(paths, n, maxN, p);
	Fmt(p, MAX_PATH, L"%splugin\\%s", g_gameDir, fileName);  AppendPath(paths, n, maxN, p);
	Fmt(p, MAX_PATH, L"%s%s", g_gameDir, fileName);          AppendPath(paths, n, maxN, p);
}

static bool NvdaOpen(void)
{
	wchar_t paths[12][MAX_PATH];
	int n = 0;
	wchar_t env[MAX_PATH];
	DWORD en = GetEnvironmentVariableW(L"A11Y_NVDA_CLIENT", env, MAX_PATH);
	if(en > 0 && en < MAX_PATH) AppendPath(paths, &n, 12, env);
	AppendGameDirs(paths, &n, 12, L"nvdaControllerClient.dll");
	AppendGameDirs(paths, &n, 12, L"nvdaControllerClient32.dll");

	if(n == 0) { LogLine(L"NVDA: 没有候选路径"); return false; }
	for(int i = 0; i < n; i++)
	{
		HMODULE dll = LoadLibraryExW(paths[i], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
		if(!dll) continue;
		NvdaTestFn test = (NvdaTestFn)GetProcAddress(dll, "nvdaController_testIfRunning");
		NvdaSpeakFn speak = (NvdaSpeakFn)GetProcAddress(dll, "nvdaController_speakText");
		NvdaCancelFn cancel = (NvdaCancelFn)GetProcAddress(dll, "nvdaController_cancelSpeech");
		if(!test || !speak || !cancel)
		{
			LogLine(L"NVDA: %s 导出不全，跳过", paths[i]);
			FreeLibrary(dll);
			continue;
		}
		if(test() != 0)
		{
			LogLine(L"NVDA: %s 已加载，但 testIfRunning 非 0（NVDA 未运行），跳过", paths[i]);
			FreeLibrary(dll);
			continue;
		}
		ZeroMemory(&g_nvda, sizeof(g_nvda));
		g_nvda.dll = dll;
		g_nvda.test = test;
		g_nvda.speak = speak;
		g_nvda.cancel = cancel;
		g_nvda.getPid = (NvdaGetPidFn)GetProcAddress(dll, "nvdaController_getProcessId");
		g_nvda.isSpeaking = (NvdaIsSpeakingFn)GetProcAddress(dll, "nvdaController_isSpeaking");
		WCopy(g_nvda.path, MAX_PATH, paths[i]);
		g_nvda.pid = -1;
		if(g_nvda.getPid)
		{
			unsigned long pid = 0;
			if(g_nvda.getPid(&pid) == 0) g_nvda.pid = (int)pid;
		}
		LogLine(L"NVDA: 可用 dll=%s nvdaPid=%d isSpeaking=%s",
		        paths[i], g_nvda.pid, g_nvda.isSpeaking ? L"有" : L"无");
		return true;
	}
	LogLine(L"NVDA: 试过 %d 个路径都不行（缺 x86 nvdaControllerClient.dll 或 NVDA 未运行）", n);
	return false;
}

static bool NvdaSpeak(const wchar_t *text)
{
	if(!g_nvda.speak) return false;
	int rc = g_nvda.speak(text);
	if(rc != 0)
	{
		LogLine(L"NVDA: speakText rc=%d（非 0=RPC 失败，NVDA 可能已退出）", rc);
		return false;
	}
	return true;
}

static void NvdaStop(void)
{
	if(g_nvda.cancel) g_nvda.cancel();
}

static void NvdaClose(void)
{
	NvdaStop();
	// 故意不 FreeLibrary：RPC 客户端可能还有在途调用；进程退出时系统统一收。
	ZeroMemory(&g_nvda, sizeof(g_nvda));
}

// ===========================================================================
//  争渡(ZDSR) 后端：ZDSRAPI.dll（官方 ZDSRAPI.txt，全部 __stdcall）
//      int  InitTTS(int type, const WCHAR* channelName, BOOL bKeyDownInterrupt)
//      int  Speak(const WCHAR* text, BOOL bInterrupt)
//      int  GetSpeakState()   3=正在朗读 4=空闲（只有 3/4 才说明真能用）
//      void StopSpeak()
//  坑：未授权/没运行时 Speak 照样返回 0 —— 必须看 GetSpeakState。
// ===========================================================================
typedef int  (__stdcall *ZdsrInitFn)(int, const wchar_t *, BOOL);
typedef int  (__stdcall *ZdsrSpeakFn)(const wchar_t *, BOOL);
typedef int  (__stdcall *ZdsrStateFn)(void);
typedef void (__stdcall *ZdsrStopFn)(void);

struct ZdsrApi
{
	HMODULE dll;
	ZdsrInitFn init;
	ZdsrSpeakFn speak;
	ZdsrStateFn state;
	ZdsrStopFn stop;
	wchar_t path[MAX_PATH];
};
static ZdsrApi g_zdsr;

static void ZdsrScanUninstall(HKEY hive, REGSAM view, wchar_t paths[][MAX_PATH], int *n, int maxN)
{
	HKEY root = NULL;
	if(RegOpenKeyExW(hive, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
	                 0, KEY_READ | view, &root) != ERROR_SUCCESS || !root) return;
	wchar_t name[512];
	for(DWORD i = 0; i < 4096; i++)
	{
		DWORD cch = 512;
		if(RegEnumKeyExW(root, i, name, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
		name[511] = 0;
		if(!StrContainsNoCase(name, L"zdsr") && !wcsstr(name, L"争渡") &&
		   !StrContainsNoCase(name, L"zhengdu")) continue;   // 快速过滤，少开几个键

		wchar_t sub[1100];
		Fmt(sub, 1100, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%s", name);
		HKEY item = NULL;
		if(RegOpenKeyExW(hive, sub, 0, KEY_READ | view, &item) != ERROR_SUCCESS || !item) continue;
		wchar_t disp[512];
		DWORD type = 0, cb = sizeof(disp);
		disp[0] = 0;
		if(RegQueryValueExW(item, L"DisplayName", NULL, &type, (LPBYTE)disp, &cb) == ERROR_SUCCESS &&
		   (type == REG_SZ || type == REG_EXPAND_SZ))
		{
			disp[511] = 0;
			if(wcsstr(disp, L"争渡") || StrContainsNoCase(disp, L"zdsr"))
			{
				wchar_t loc[MAX_PATH];
				DWORD cb2 = sizeof(loc);
				loc[0] = 0;
				if(RegQueryValueExW(item, L"InstallLocation", NULL, &type, (LPBYTE)loc, &cb2) == ERROR_SUCCESS &&
				   (type == REG_SZ || type == REG_EXPAND_SZ))
				{
					loc[MAX_PATH - 1] = 0;
					size_t len = wcslen(loc);
					while(len > 0 && (loc[len - 1] == L'\\' || loc[len - 1] == L' ')) loc[--len] = 0;
					if(len > 0)
					{
						wchar_t dll[MAX_PATH];
						Fmt(dll, MAX_PATH, L"%s\\ZDSRAPI.dll", loc);
						AppendPath(paths, n, maxN, dll);
					}
				}
			}
		}
		RegCloseKey(item);
	}
	RegCloseKey(root);
}

static bool ZdsrOpen(void)
{
	wchar_t paths[24][MAX_PATH];
	int n = 0;

	wchar_t pin[MAX_PATH * 2];
	DWORD pn = GetEnvironmentVariableW(L"A11Y_ZDSR_DIR", pin, MAX_PATH * 2);
	if(pn > 0 && pn < MAX_PATH * 2)
	{
		pin[MAX_PATH * 2 - 1] = 0;
		size_t len = wcslen(pin);
		if(len > 4 && _wcsicmp(pin + len - 4, L".dll") == 0) AppendPath(paths, &n, 24, pin);
		else
		{
			wchar_t p[MAX_PATH];
			Fmt(p, MAX_PATH, L"%s\\ZDSRAPI.dll", pin);
			AppendPath(paths, &n, 24, p);
		}
	}
	ZdsrScanUninstall(HKEY_LOCAL_MACHINE, 0, paths, &n, 24);
	ZdsrScanUninstall(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, paths, &n, 24);
	ZdsrScanUninstall(HKEY_CURRENT_USER, 0, paths, &n, 24);
	ZdsrScanUninstall(HKEY_CURRENT_USER, KEY_WOW64_64KEY, paths, &n, 24);
	{
		const wchar_t *envs[2] = { L"ProgramFiles(x86)", L"ProgramFiles" };
		for(int e = 0; e < 2; e++)
		{
			wchar_t pf[MAX_PATH], p[MAX_PATH];
			DWORD len = GetEnvironmentVariableW(envs[e], pf, MAX_PATH);
			if(len == 0 || len >= MAX_PATH) continue;
			Fmt(p, MAX_PATH, L"%s\\zdsr\\zdsr\\ZDSRAPI.dll", pf);
			AppendPath(paths, &n, 24, p);
		}
	}
	AppendGameDirs(paths, &n, 24, L"ZDSRAPI.dll");

	for(int i = 0; i < n; i++)
	{
		HMODULE dll = LoadLibraryExW(paths[i], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
		if(!dll)
		{
			LogLine(L"ZDSR: LoadLibrary 失败 err=%lu path=%s",
			        (unsigned long)GetLastError(), paths[i]);
			continue;
		}
		ZdsrInitFn init = (ZdsrInitFn)GetProcAddress(dll, "InitTTS");
		ZdsrSpeakFn speak = (ZdsrSpeakFn)GetProcAddress(dll, "Speak");
		ZdsrStateFn state = (ZdsrStateFn)GetProcAddress(dll, "GetSpeakState");
		ZdsrStopFn stop = (ZdsrStopFn)GetProcAddress(dll, "StopSpeak");
		if(!init || !speak || !state)
		{
			LogLine(L"ZDSR: %s 缺导出 InitTTS/Speak/GetSpeakState", paths[i]);
			FreeLibrary(dll);
			continue;
		}
		int rc = init(0, NULL, FALSE);      // type=0 读屏通道；channelName 忽略
		if(rc == 1)
		{
			LogLine(L"ZDSR: %s InitTTS=1 版本不匹配，试下一份", paths[i]);
			FreeLibrary(dll);
			continue;
		}
		if(rc != 0)
		{
			LogLine(L"ZDSR: %s InitTTS=%d（2=没运行/没授权），不可用", paths[i], rc);
			FreeLibrary(dll);
			return false;
		}
		int st = state();
		LogLine(L"ZDSR: 握手 dll=%s InitTTS=0 GetSpeakState=%d", paths[i], st);
		if(st != 3 && st != 4)
		{
			LogLine(L"ZDSR: GetSpeakState=%d 不在 {3,4}，判不可用（未授权/没连上）", st);
			FreeLibrary(dll);
			return false;
		}
		ZeroMemory(&g_zdsr, sizeof(g_zdsr));
		g_zdsr.dll = dll;
		g_zdsr.init = init;
		g_zdsr.speak = speak;
		g_zdsr.state = state;
		g_zdsr.stop = stop;
		WCopy(g_zdsr.path, MAX_PATH, paths[i]);
		return true;
	}
	LogLine(L"ZDSR: 所有候选都不可用（试了 %d 处）", n);
	return false;
}

static bool ZdsrSpeak(const wchar_t *text)
{
	if(!g_zdsr.speak || !g_zdsr.state) return false;
	int st = g_zdsr.state();
	if(st != 3 && st != 4)
	{
		LogLine(L"ZDSR: 朗读前 GetSpeakState=%d 不在 {3,4}，标记不可用", st);
		return false;
	}
	int rc = g_zdsr.speak(text, TRUE);   // TRUE = 打断当前句
	if(rc != 0)
	{
		LogLine(L"ZDSR: Speak rc=%d（0 才是成功）", rc);
		return false;
	}
	return true;
}

static void ZdsrStop(void)
{
	if(g_zdsr.stop) g_zdsr.stop();
}

static void ZdsrClose(void)
{
	ZdsrStop();
	ZeroMemory(&g_zdsr, sizeof(g_zdsr));
}

// ===========================================================================
//  SAPI 后端：worker 线程本身就是 STA，ISpVoice 只在该线程创建/使用
//  朗读用异步 + SPF_PURGEBEFORESPEAK（新句打断旧句），Stop 用 Speak(NULL, PURGE)。
//  语音必须挑中文（lang=804）—— 实测英文语音念中文会"返回 S_OK 但零音频"。
// ===========================================================================
static ISpVoice *g_spVoice = NULL;
static wchar_t   g_sapiVoiceName[256];

static void SapiPickVoice(ISpVoice *voice)
{
	ISpObjectTokenCategory *cat = NULL;
	IEnumSpObjectTokens *en = NULL;
	ISpObjectToken *pick = NULL;
	wchar_t why[256];
	why[0] = 0;

	HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory, NULL, CLSCTX_ALL,
	                              IID_ISpObjectTokenCategory, (void **)&cat);
	if(SUCCEEDED(hr) && cat) hr = cat->SetId(SPCAT_VOICES, FALSE);
	if(SUCCEEDED(hr) && cat) hr = cat->EnumTokens(NULL, NULL, &en);
	if(en)
	{
		ULONG count = 0;
		en->GetCount(&count);
		LogLine(L"SAPI: 32 位进程可见 %lu 个语音", (unsigned long)count);
		for(ULONG i = 0; i < count && !pick; i++)
		{
			ISpObjectToken *tok = NULL;
			if(en->Item(i, &tok) != S_OK || !tok) continue;
			wchar_t *name = NULL;
			tok->GetStringValue(NULL, &name);
			wchar_t *lang = NULL;
			ISpDataKey *attrs = NULL;
			if(SUCCEEDED(tok->OpenKey(L"Attributes", &attrs)) && attrs)
			{
				attrs->GetStringValue(L"Language", &lang);
				attrs->Release();
			}
			LogLine(L"SAPI: voice[%lu] name=%s lang=%s", (unsigned long)i,
			        name ? name : L"?", lang ? lang : L"?");
			bool zh = false;
			if(lang && wcsstr(lang, L"804")) zh = true;
			if(!zh && name && (wcsstr(name, L"Chinese") || wcsstr(name, L"Huihui") ||
			                   wcsstr(name, L"中文") || wcsstr(name, L"慧慧"))) zh = true;
			if(zh)
			{
				pick = tok;
				if(name) WCopy(g_sapiVoiceName, 256, name);
			}
			else tok->Release();
			if(name) CoTaskMemFree(name);
			if(lang) CoTaskMemFree(lang);
		}
		if(!pick && en->Item(0, &pick) == S_OK && pick)
		{
			wchar_t *name = NULL;
			pick->GetStringValue(NULL, &name);
			if(name) { WCopy(g_sapiVoiceName, 256, name); CoTaskMemFree(name); }
			WCopy(why, 256, L"（无中文语音，退回默认语音）");
			LogLine(L"SAPI: 警告——本机没有中文语音，中文朗读可能完全没声音");
		}
	}
	if(pick)
	{
		hr = voice->SetVoice(pick);
		LogLine(L"SAPI: SetVoice hr=0x%08lX voice=%s%s", (unsigned long)hr, g_sapiVoiceName, why);
		pick->Release();
	}
	else
	{
		WCopy(g_sapiVoiceName, 256, L"(系统默认)");
		LogLine(L"SAPI: 一个语音 token 都没枚举到，用系统默认语音");
	}
	if(en) en->Release();
	if(cat) cat->Release();
}

// rate 0..100 → SAPI -10..10（50 = 0）；volume 0..100 直接给 SAPI
static void SapiApplyRateVolume(ISpVoice *voice)
{
	int rate = CfgGetInt(L"rate", 50);
	if(rate < 0) rate = 0;
	if(rate > 100) rate = 100;
	long r = (long)((rate - 50) / 5);
	if(r < -10) r = -10;
	if(r > 10) r = 10;
	int vol = CfgGetInt(L"volume", 100);
	if(vol < 0) vol = 0;
	if(vol > 100) vol = 100;
	HRESULT h1 = voice->SetRate(r);
	HRESULT h2 = voice->SetVolume((USHORT)vol);
	LogLine(L"SAPI: rate=%d%%->%ld hr=0x%08lX volume=%d%% hr=0x%08lX（仅 SAPI 生效）",
	        rate, r, (unsigned long)h1, vol, (unsigned long)h2);
}

static bool SapiOpen(void)
{
	HRESULT hr = CoCreateInstance(CLSID_SpVoice, NULL, CLSCTX_ALL, IID_ISpVoice, (void **)&g_spVoice);
	if(FAILED(hr) || !g_spVoice)
	{
		LogLine(L"SAPI: CoCreateInstance(CLSID_SpVoice) hr=0x%08lX", (unsigned long)hr);
		g_spVoice = NULL;
		return false;
	}
	g_sapiVoiceName[0] = 0;
	SapiPickVoice(g_spVoice);
	SapiApplyRateVolume(g_spVoice);
	LogLine(L"SAPI: 就绪 voice=%s", g_sapiVoiceName);
	return true;
}

static bool SapiSpeak(const wchar_t *text)
{
	if(!g_spVoice) return false;
	HRESULT hr = g_spVoice->Speak(text, SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, NULL);
	if(FAILED(hr))
	{
		LogLine(L"SAPI: Speak hr=0x%08lX", (unsigned long)hr);
		return false;
	}
	return true;
}

static void SapiStop(void)
{
	if(g_spVoice) g_spVoice->Speak(NULL, SPF_PURGEBEFORESPEAK, NULL);
}

// STA 线程退出前必须把消息泵干净：SAPI 的异步朗读对象在 Release 前如果还有在途
// 音频回调，直接 Release 会 AV（实测 0xC0000005）。所以：先 Purge，再边泵消息边等
// dwRunningState==0，最后才 Release；Release 本身再用 SEH 兜一层，绝不让补丁把游戏带崩。
static void SapiPump(void)
{
	MSG msg;
	int guard = 0;
	while(guard++ < 200 && PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
}

static void SapiReleaseGuarded(ISpVoice *v)
{
	__try
	{
		v->Release();
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		LogLine(L"SAPI: Release 触发异常 0x%08lX，已忽略（进程退出前泄漏一个 COM 对象，无害）",
		        (unsigned long)GetExceptionCode());
	}
}

static void SapiClose(void)
{
	if(!g_spVoice) return;
	g_spVoice->Speak(NULL, SPF_PURGEBEFORESPEAK, NULL);
	for(int i = 0; i < 60; i++)     // 最多等 600ms，边等边泵消息
	{
		SPVOICESTATUS st;
		ZeroMemory(&st, sizeof(st));
		HRESULT hr = g_spVoice->GetStatus(&st, NULL);
		if(FAILED(hr) || st.dwRunningState == 0) break;
		SapiPump();
		Sleep(10);
	}
	SapiPump();
	ISpVoice *v = g_spVoice;
	g_spVoice = NULL;
	SapiReleaseGuarded(v);
	LogLine(L"SAPI: 已释放 voice");
}

// ===========================================================================
//  Tolk 后端：动态加载 Tolk.dll（__cdecl，导出名无修饰）
//  两个实测坑（见 evidence\tolk_order_*.log）：
//   1) 这版 Tolk-with-zdsr 按「宿主 EXE 目录/已加载同名模块」去找读屏客户端 dll，
//      不是按 Tolk.dll 自己所在目录。子目录部署时必须先用绝对路径预加载
//      nvdaControllerClient32.dll / nvdaControllerClient.dll / ZDSRAPI.dll，再 Tolk_Load。
//   2) Tolk_IsLoaded/HasSpeech/IsSpeaking 返回 C++ bool（只有 AL 有效），用 int 接会读到
//      EAX 高位垃圾 → 必须用 unsigned char。
//  Tolk 是 LGPL-3.0；本模块只运行时 LoadLibrary，不分发其二进制。
// ===========================================================================
typedef void           (__cdecl *TolkLoadFn)(void);
typedef unsigned char  (__cdecl *TolkIsLoadedFn)(void);
typedef unsigned char  (__cdecl *TolkHasSpeechFn)(void);
typedef int            (__cdecl *TolkSpeakFn)(const wchar_t *, int);
typedef int            (__cdecl *TolkSilenceFn)(void);
typedef unsigned char  (__cdecl *TolkIsSpeakingFn)(void);
typedef const wchar_t *(__cdecl *TolkDetectFn)(void);
typedef void           (__cdecl *TolkUnloadFn)(void);

struct TolkApi
{
	HMODULE dll;
	TolkLoadFn load;
	TolkIsLoadedFn isLoaded;
	TolkHasSpeechFn hasSpeech;
	TolkSpeakFn speak;
	TolkSilenceFn silence;
	TolkIsSpeakingFn isSpeaking;
	TolkDetectFn detect;
	TolkUnloadFn unload;
	wchar_t path[MAX_PATH];
	wchar_t detected[128];
};
static TolkApi g_tolk;

static void TolkPreloadProviders(const wchar_t *tolkPath)
{
	static const wchar_t *kNames[3] = {
		L"nvdaControllerClient32.dll", L"nvdaControllerClient.dll", L"ZDSRAPI.dll"
	};
	wchar_t dir[MAX_PATH];
	if(!DirOf(tolkPath, dir, MAX_PATH)) return;
	for(int i = 0; i < 3; i++)
	{
		wchar_t p[MAX_PATH];
		Fmt(p, MAX_PATH, L"%s%s", dir, kNames[i]);
		HMODULE h = LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
		LogLine(L"Tolk: 预加载 %s -> %s", p, h ? L"OK" : L"失败（没有这个读屏客户端）");
	}
}

static bool TolkOpen(void)
{
	wchar_t paths[12][MAX_PATH];
	int n = 0;
	wchar_t env[MAX_PATH];
	DWORD en = GetEnvironmentVariableW(L"A11Y_TOLK_DLL", env, MAX_PATH);
	if(en > 0 && en < MAX_PATH) AppendPath(paths, &n, 12, env);
	AppendGameDirs(paths, &n, 12, L"Tolk.dll");
	{
		wchar_t p[MAX_PATH];
		Fmt(p, MAX_PATH, L"%stolk\\Tolk.dll", g_a11yDir); AppendPath(paths, &n, 12, p);
		Fmt(p, MAX_PATH, L"%stolk\\Tolk.dll", g_gameDir); AppendPath(paths, &n, 12, p);
	}
	for(int i = 0; i < n; i++)
	{
		HMODULE dll = LoadLibraryExW(paths[i], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
		if(!dll) continue;
		TolkApi a;
		ZeroMemory(&a, sizeof(a));
		a.dll = dll;
		a.load = (TolkLoadFn)GetProcAddress(dll, "Tolk_Load");
		a.isLoaded = (TolkIsLoadedFn)GetProcAddress(dll, "Tolk_IsLoaded");
		a.hasSpeech = (TolkHasSpeechFn)GetProcAddress(dll, "Tolk_HasSpeech");
		a.speak = (TolkSpeakFn)GetProcAddress(dll, "Tolk_Speak");
		a.silence = (TolkSilenceFn)GetProcAddress(dll, "Tolk_Silence");
		a.isSpeaking = (TolkIsSpeakingFn)GetProcAddress(dll, "Tolk_IsSpeaking");
		a.detect = (TolkDetectFn)GetProcAddress(dll, "Tolk_DetectScreenReader");
		a.unload = (TolkUnloadFn)GetProcAddress(dll, "Tolk_Unload");
		if(!a.load || !a.isLoaded || !a.hasSpeech || !a.speak || !a.silence)
		{
			LogLine(L"Tolk: %s 导出不全，跳过", paths[i]);
			FreeLibrary(dll);
			continue;
		}
		TolkPreloadProviders(paths[i]);
		a.load();
		if(!a.isLoaded() || !a.hasSpeech())
		{
			LogLine(L"Tolk: %s 已加载但 IsLoaded=%d HasSpeech=%d（没有可用读屏）",
			        paths[i], (int)a.isLoaded(), (int)a.hasSpeech());
			FreeLibrary(dll);
			continue;
		}
		if(a.detect)
		{
			const wchar_t *d = a.detect();
			if(d) WCopy(a.detected, 128, d);
		}
		WCopy(a.path, MAX_PATH, paths[i]);
		g_tolk = a;
		LogLine(L"Tolk: 可用 dll=%s detected=%s", paths[i],
		        g_tolk.detected[0] ? g_tolk.detected : L"?");
		return true;
	}
	LogLine(L"Tolk: 没有可用 Tolk.dll（试过 %d 处；可选组件，未部署就跳过）", n);
	return false;
}

static bool TolkSpeak(const wchar_t *text)
{
	if(!g_tolk.speak) return false;
	if(g_tolk.silence) g_tolk.silence();          // 先掐断上一句（Tolk_Speak 不保证打断）
	int ok = g_tolk.speak(text, 1);
	if(!ok) LogLine(L"Tolk: Tolk_Speak 返回 false");
	return ok != 0;
}

static void TolkStop(void)
{
	if(g_tolk.silence) g_tolk.silence();
}

static void TolkClose(void)
{
	if(g_tolk.dll && g_tolk.unload) g_tolk.unload();
	ZeroMemory(&g_tolk, sizeof(g_tolk));
}

// ===========================================================================
//  worker 线程：探测 + 请求处理（唯一接触第三方 DLL / COM 的线程）
// ===========================================================================
static HANDLE  g_evtWake = NULL;
static HANDLE  g_evtReady = NULL;
static HANDLE  g_evtCycleDone = NULL;
static HANDLE  g_hWorker = NULL;
static DWORD   g_workerTid = 0;

static volatile LONG g_inited = 0;      // 1 = 已初始化（含 worker 在跑）
static volatile LONG g_ready = 0;       // 1 = worker 探测完成
static volatile LONG g_backend = SPEECH_BACKEND_NONE;
static volatile LONG g_quitReq = 0;
static volatile LONG g_cycleReq = 0;
static volatile LONG g_stopReq = 0;
static wchar_t g_lastError[512];

// --- 请求队列：三个线程并发调用 Speech_Speak 时的串行化点 --------------------
// 剧情朗读走 FIFO（一次只念一句、绝不重叠）；短播报（菜单/热键）只保留最新一条、
// 允许打断剧情。队列内容受 g_cs 保护；真正的后端朗读调用只发生在 worker 线程，
// 所以第三方 API 天然串行、不会两句话同时喂给同一个读屏器。
#define A11Y_QUEUE_MAX 32
static wchar_t g_qText[A11Y_QUEUE_MAX][A11Y_MAX_TEXT];
static volatile LONG g_qHead = 0;         // 只在 g_cs 内改
static volatile LONG g_qCount = 0;        // 只在 g_cs 内改
static wchar_t g_announce[A11Y_MAX_TEXT];
static volatile LONG g_announceSeq = 0;   // 生产者自增
static volatile LONG g_announcedSeq = 0;  // worker 已取走的序号
static volatile LONG g_statStoryEnq = 0;
static volatile LONG g_statStorySpoken = 0;
static volatile LONG g_statStoryDropped = 0;
static volatile LONG g_statAnnounceEnq = 0;
static volatile LONG g_statAnnounceSpoken = 0;
static volatile LONG g_failStreak = 0;    // 连续朗读失败次数（成功即清零）

// worker 私有状态：上一句是否还在念（后端不报告朗读状态时用"时长估算"兜底）
static bool  g_workerSpeaking = false;
static DWORD g_workerEndTick = 0;
static int   g_workerSpeakingId = SPEECH_BACKEND_NONE;

// COM 套间存活探针：CO_E_NOTINITIALIZED(0x800401F0) 说明本线程的 COM 已被别人卸载掉了。
static void LogComState(const wchar_t *tag)
{
	IMalloc *m = NULL;
	HRESULT hr = CoGetMalloc(1, &m);
	if(SUCCEEDED(hr) && m) m->Release();
	LogLine(L"COM 探针[%s]: CoGetMalloc hr=0x%08lX %s", tag, (unsigned long)hr,
	        hr == CO_E_NOTINITIALIZED ? L"(套间已死!)" : L"(套间正常)");
}

static void SetLastErrorText(const wchar_t *s)
{
	EnterCs();
	WCopy(g_lastError, 512, s ? s : L"");
	LeaveCs();
}

static bool ProbeSlot(int i)
{
	Slot *s = &g_slot[i];
	WCopy(s->detail, 192, L"");
	bool ok = false;
	switch(s->id)
	{
	case SPEECH_BACKEND_NVDA:
		ok = NvdaOpen();
		if(ok) Fmt(s->detail, 192, L"nvdaPid=%d %s", g_nvda.pid, g_nvda.path);
		else WCopy(s->detail, 192, L"缺 nvdaControllerClient.dll 或 NVDA 未运行");
		break;
	case SPEECH_BACKEND_ZDSR:
		ok = ZdsrOpen();
		if(ok) WCopy(s->detail, 192, g_zdsr.path);
		else WCopy(s->detail, 192, L"ZDSRAPI.dll 不可用（未安装/未运行/未授权）");
		break;
	case SPEECH_BACKEND_SAPI:
		ok = SapiOpen();
		if(ok) Fmt(s->detail, 192, L"voice=%s", g_sapiVoiceName);
		else WCopy(s->detail, 192, L"SAPI 初始化失败（COM/语音包）");
		break;
	case SPEECH_BACKEND_TOLK:
		ok = TolkOpen();
		if(ok) Fmt(s->detail, 192, L"Tolk(%s) %s", g_tolk.detected[0] ? g_tolk.detected : L"?", g_tolk.path);
		else WCopy(s->detail, 192, L"没部署 Tolk.dll（可选组件）");
		break;
	}
	s->available = ok;
	LogLine(L"后端[%d] %s: %s（%s）", i + 1, kRingName[i], ok ? L"可用" : L"不可用", s->detail);
	return ok;
}

static bool SpeakOnId(int id, const wchar_t *text)
{
	switch(id)
	{
	case SPEECH_BACKEND_NVDA: return NvdaSpeak(text);
	case SPEECH_BACKEND_ZDSR: return ZdsrSpeak(text);
	case SPEECH_BACKEND_SAPI: return SapiSpeak(text);
	case SPEECH_BACKEND_TOLK: return TolkSpeak(text);
	}
	return false;
}

static void StopOnId(int id)
{
	switch(id)
	{
	case SPEECH_BACKEND_NVDA: NvdaStop(); break;
	case SPEECH_BACKEND_ZDSR: ZdsrStop(); break;
	case SPEECH_BACKEND_SAPI: SapiStop(); break;
	case SPEECH_BACKEND_TOLK: TolkStop(); break;
	}
}

// 直投后端（NVDA / Tolk）：speakText / Tolk_Speak 本身就是"异步投递 + 读屏器自己排队"，
// 我们不需要、也不应该等它念完。投递返回成功即视为本句"已完成"，worker 立刻取下一句；
// 打断靠 cancelSpeech / Tolk_Silence（见 announce 分支）。ZDSR/SAPI 有状态查询，仍等念完，
// 因为它们返回 FALSE/错误码时才能可靠判定"没念成"（自愈依赖这个）。
// 取舍：直投在台词密集时会"追上"下图/下一句，靠玩家按键的天然节奏拉开；偶发打断由读屏器
// 自己的队列语义决定，比"按估算等 6-7 秒导致丢句"划算得多（Lead m00684 拍板）。
static bool IsDirectFeedId(int id)
{
	return id == SPEECH_BACKEND_NVDA || id == SPEECH_BACKEND_TOLK;
}

// 供复盘用的一行状态：speech: backend=<中文名> direct_feed=0/1
static void LogBackendMode(const wchar_t *where)
{
	int id = (int)InterlockedCompareExchange(&g_backend, 0, 0);
	int idx = RingIndexOfId(id);
	const wchar_t *name = (id == SPEECH_BACKEND_NONE || idx < 0) ? L"无" : kRingName[idx];
	LogLine(L"speech: backend=%s direct_feed=%d（%s）", name, IsDirectFeedId(id) ? 1 : 0, where);
}

// 后端"是否正在出声"：1=在念 0=空闲 -1=不知道（该后端没有可用的状态查询）
static int IsSpeakingOnId(int id)
{
	switch(id)
	{
	case SPEECH_BACKEND_NVDA:
		if(!g_nvda.isSpeaking) return -1;      // 老客户端没有 isSpeaking 导出（dumpbin 实测）
		{
			int v = 0;
			if(g_nvda.isSpeaking(&v) != 0) return -1;
			return v ? 1 : 0;
		}
	case SPEECH_BACKEND_ZDSR:
		if(!g_zdsr.state) return -1;
		{
			int st = g_zdsr.state();
			if(st == 3) return 1;              // 正在朗读
			if(st == 4) return 0;              // 空闲
			return -1;
		}
	case SPEECH_BACKEND_SAPI:
		if(!g_spVoice) return -1;
		{
			SPVOICESTATUS vs;
			ZeroMemory(&vs, sizeof(vs));
			if(FAILED(g_spVoice->GetStatus(&vs, NULL))) return -1;
			return vs.dwRunningState != 0 ? 1 : 0;
		}
	case SPEECH_BACKEND_TOLK:
		if(!g_tolk.isSpeaking) return -1;
		return g_tolk.isSpeaking() ? 1 : 0;
	}
	return -1;
}

// 估算一句话要念多久（毫秒）。中文/全角约 170ms/字、ASCII 约 85ms/字，再按 config rate
// 缩放（rate=50 为基准；实际只有 SAPI 会真的变速，其余后端用它做"排队不重叠"的兜底）。
static DWORD EstimateSpeechMs(const wchar_t *text)
{
	int zh = 0, ascii = 0;
	for(const wchar_t *p = text; p && *p; p++)
	{
		if(*p < 128) ascii++;
		else zh++;
	}
	int rate = CfgGetInt(L"rate", 50);
	if(rate < 0) rate = 0;
	if(rate > 100) rate = 100;
	int durationPct = 200 - rate * 3 / 2;   // rate=0 -> 200%，50 -> 125%，100 -> 50%
	if(durationPct < 50) durationPct = 50;
	DWORD ms = (DWORD)(((zh * 170) + (ascii * 85)) * durationPct / 100);
	if(ms < 300) ms = 300;
	if(ms > 30000) ms = 30000;
	return ms;
}

static void MarkSpeaking(int id, const wchar_t *text)
{
	g_workerSpeakingId = id;
	if(IsDirectFeedId(id))
	{
		// 直投：投出去就算完成，不等"念完"（否则就是旧的 6-7 秒估算等待）
		g_workerSpeaking = false;
		g_workerEndTick = 0;
		return;
	}
	g_workerSpeaking = true;
	g_workerEndTick = GetTickCount() + EstimateSpeechMs(text);
}

static bool StillSpeaking(void)
{
	if(!g_workerSpeaking) return false;
	if(IsDirectFeedId(g_workerSpeakingId)) return false;   // 直投后端不产生"等待"
	int known = IsSpeakingOnId(g_workerSpeakingId);
	if(known == 1) return true;
	if(known == 0) return false;
	return (LONG)(g_workerEndTick - GetTickCount()) > 0;   // 不知道就问时钟
}

static int FirstAvailableFrom(int afterId)
{
	int start = RingIndexOfId(afterId);
	for(int k = 1; k <= A11Y_SLOTS; k++)
	{
		int i = (start < 0) ? (k - 1) : ((start + k) % A11Y_SLOTS);
		if(g_slot[i].available) return g_slot[i].id;
	}
	return SPEECH_BACKEND_NONE;
}

// 句首插回队列（用于"这句没念成、切了后端再念"）；调用方不要持有 g_cs。
static bool RequeueFront(const wchar_t *text)
{
	bool ok = false;
	EnterCs();
	if(g_qCount < A11Y_QUEUE_MAX)
	{
		int nh = (int)((g_qHead - 1 + A11Y_QUEUE_MAX) % A11Y_QUEUE_MAX);
		WCopy(g_qText[nh], A11Y_MAX_TEXT, text);
		g_qHead = nh;
		g_qCount++;
		ok = true;
	}
	LeaveCs();
	return ok;
}

// 真正的朗读入口（只在 worker 线程调用）。失败计数到阈值 → 判失效 + 降级 + 念"已切换到 XX"
static bool BackendSpeak(const wchar_t *text, const wchar_t *kind)
{
	int id = (int)InterlockedCompareExchange(&g_backend, 0, 0);
	if(id == SPEECH_BACKEND_NONE)
	{
		LogLine(L"speak[%s]: 当前没有可用后端，丢弃一句", kind);
		InterlockedIncrement(&g_statStoryDropped);
		return false;
	}
	if(SpeakOnId(id, text))
	{
		InterlockedExchange(&g_failStreak, 0);
		return true;
	}

	int idx = RingIndexOfId(id);
	LONG fail = InterlockedIncrement(&g_failStreak);
	int threshold = CfgGetInt(L"fail_threshold", 3);
	if(threshold < 1) threshold = 1;
	if(threshold > 10) threshold = 10;
	LogLine(L"speak[%s]: 后端 %s 朗读失败（连续第 %ld 次，阈值 %d）",
	        kind, kRingName[idx], (long)fail, threshold);
	if(fail < threshold)
	{
		// 还没到阈值：先当作偶发失败（不切后端、不念错），这一句丢
		InterlockedIncrement(&g_statStoryDropped);
		return false;
	}

	// 到阈值 → 判定这个后端失效
	LogLine(L"self-heal: %s 连续失败 %ld 次，判定失效，开始降级", kRingName[idx], (long)fail);
	if(idx >= 0) g_slot[idx].available = false;
	wchar_t err[256];
	Fmt(err, 256, L"后端 %s 连续朗读失败，已自动降级", kRingName[idx]);
	SetLastErrorText(err);
	InterlockedExchange(&g_failStreak, 0);

	int next = FirstAvailableFrom(id);
	if(next == SPEECH_BACKEND_NONE || next == id)
	{
		InterlockedExchange(&g_backend, SPEECH_BACKEND_NONE);
		LogLine(L"self-heal: 没有别的可用后端了（玩家需要启动某个读屏器）");
		InterlockedIncrement(&g_statStoryDropped);
		return false;
	}

	InterlockedExchange(&g_backend, next);
	LogLine(L"self-heal: 已切换到 %s", kRingName[RingIndexOfId(next)]);
	LogBackendMode(L"self-heal");
	// 这一句没念成 → 塞回队首，等切换播报完再念
	if(RequeueFront(text)) InterlockedIncrement(&g_statStoryEnq);
	else InterlockedIncrement(&g_statStoryDropped);

	wchar_t msg[160];
	Fmt(msg, 160, L"已切换到%ls", kRingName[RingIndexOfId(next)]);
	if(SpeakOnId(next, msg))
	{
		InterlockedIncrement(&g_statAnnounceSpoken);
		MarkSpeaking(next, msg);
	}
	SetLastErrorText(L"");
	return true;
}

static void SpeakBackendName(int id)
{
	int idx = RingIndexOfId(id);
	if(idx < 0) return;
	wchar_t msg[160];
	Fmt(msg, 160, L"语音后端：%s", kRingName[idx]);
	LogLine(L"worker: 朗读后端名 -> %s", msg);
	if(SpeakOnId(id, msg))
	{
		InterlockedIncrement(&g_statAnnounceSpoken);
		MarkSpeaking(id, msg);
	}
}

static void DoCycle(void)
{
	int cur = (int)InterlockedCompareExchange(&g_backend, 0, 0);
	int chosen = SPEECH_BACKEND_NONE;
	int start = RingIndexOfId(cur);
	for(int k = 1; k <= A11Y_SLOTS; k++)
	{
		int i = (start < 0) ? (k - 1) : ((start + k) % A11Y_SLOTS);
		if(!g_slot[i].available)
		{
			// 玩家可能在游戏运行中途启动/关闭了读屏 → 重探一次（worker 线程里做，不卡游戏）
			LogLine(L"cycle: 重探 %s", kRingName[i]);
			ProbeSlot(i);
		}
		if(g_slot[i].available) { chosen = g_slot[i].id; break; }
	}
	if(chosen == SPEECH_BACKEND_NONE)
	{
		LogLine(L"cycle: 没有可用后端，保持当前不变");
		SetLastErrorText(L"切换语音后端失败：没有任何可用后端");
		return;
	}
	InterlockedExchange(&g_backend, chosen);
	LogLine(L"cycle: %s -> %s", (cur == 0 || start < 0) ? L"无" : kRingName[start],
	        kRingName[RingIndexOfId(chosen)]);
	LogBackendMode(L"cycle");
	SpeakBackendName(chosen);
}

static void SelectInitialBackend(void)
{
	// 1) 环境变量（自检/排查优先）  2) config.ini 的 backend=
	int want = SPEECH_BACKEND_NONE;
	wchar_t env[64];
	DWORD en = GetEnvironmentVariableW(L"A11Y_SPEECH_BACKEND", env, 64);
	if(en > 0 && en < 64) { env[63] = 0; want = ParseBackendKey(env); }
	if(want == SPEECH_BACKEND_NONE)
	{
		const wchar_t *cfg = CfgGetStr(L"backend", L"auto");
		if(_wcsicmp(cfg, L"auto") != 0) want = ParseBackendKey(cfg);
	}

	if(want != SPEECH_BACKEND_NONE)
	{
		int i = RingIndexOfId(want);
		if(i >= 0 && g_slot[i].available)
		{
			InterlockedExchange(&g_backend, want);
			LogLine(L"选中后端: %s（配置指定）", kRingName[i]);
			return;
		}
		LogLine(L"配置指定的后端 %d 不可用，回退到自动选择", want);
	}
	int id = FirstAvailableFrom(SPEECH_BACKEND_NONE);
	InterlockedExchange(&g_backend, id);
	if(id != SPEECH_BACKEND_NONE) LogLine(L"选中后端: %s（自动）", kRingName[RingIndexOfId(id)]);
	else
	{
		LogLine(L"选中后端: 无（四个后端都不可用）");
		SetLastErrorText(L"没有任何可用语音后端");
	}
}

// 速率/音量到底能不能落地（不许编 API：只写 dll 里真实存在的函数）
static void LogRateVolumeSupport(void)
{
	int rate = CfgGetInt(L"rate", 50);
	int vol = CfgGetInt(L"volume", 100);
	if(rate < 0) rate = 0;
	if(rate > 100) rate = 100;
	if(vol < 0) vol = 0;
	if(vol > 100) vol = 100;
	LogLine(L"rate/volume: 配置 rate=%d volume=%d", rate, vol);
	LogLine(L"rate/volume: SAPI = 已生效（ISpVoice::SetRate(-10..10) / SetVolume(0..100)）");
	LogLine(L"rate/volume: NVDA = 不生效（x86 客户端 7 个导出里没有任何速率/音量函数，"
	        L"只有 testIfRunning/speakText/speakSsml/cancelSpeech/brailleMessage/getProcessId/"
	        L"setOnSsmlMarkReachedCallback；请在 NVDA 语音设置里调）");
	LogLine(L"rate/volume: 争渡 = 不生效（ZDSRAPI.dll 只有 InitTTS/Speak/StopSpeak/GetSpeakState/Braille，"
	        L"无速率/音量 API；请在争渡设置里调）");
	LogLine(L"rate/volume: Tolk = 不生效（Tolk 无 rate/volume 导出）");
}

static DWORD WINAPI WorkerProc(LPVOID)
{
	MSG msg;
	PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);   // 先建消息队列

	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	bool comOk = SUCCEEDED(hr);
	LogLine(L"worker: 启动 tid=%lu CoInitializeEx(STA) hr=0x%08lX",
	        (unsigned long)GetCurrentThreadId(), (unsigned long)hr);
	if(!comOk && hr != RPC_E_CHANGED_MODE)
		LogLine(L"worker: 警告——COM 初始化失败，SAPI 后端会不可用");

	LogLine(L"==== Speech_Init: 开始探测后端（顺序 NVDA -> 争渡 -> SAPI -> Tolk）====");
	for(int i = 0; i < A11Y_SLOTS; i++)
	{
		g_slot[i].id = kRingId[i];
		g_slot[i].available = false;
	}
	for(int i = 0; i < A11Y_SLOTS; i++) ProbeSlot(i);
	SelectInitialBackend();
	LogRateVolumeSupport();
	LogBackendMode(L"init");

	InterlockedExchange(&g_ready, 1);
	if(g_evtReady) SetEvent(g_evtReady);

	int curId = (int)InterlockedCompareExchange(&g_backend, 0, 0);
	LogLine(L"==== Speech_Init: 完成，当前后端=%s ====",
	        curId == 0 ? L"无" : kRingName[RingIndexOfId(curId)]);

	for(;;)
	{
		if(InterlockedCompareExchange(&g_quitReq, 0, 0) != 0) break;

		DWORD r = MsgWaitForMultipleObjects(1, &g_evtWake, FALSE, 50, QS_ALLINPUT);
		if(r == WAIT_OBJECT_0) ResetEvent(g_evtWake);

		if(InterlockedCompareExchange(&g_cycleReq, 0, 0) != 0)
		{
			InterlockedExchange(&g_cycleReq, 0);
			DoCycle();
			if(g_evtCycleDone) SetEvent(g_evtCycleDone);
		}
		if(InterlockedCompareExchange(&g_stopReq, 0, 0) != 0)
		{
			InterlockedExchange(&g_stopReq, 0);
			int id = (int)InterlockedCompareExchange(&g_backend, 0, 0);
			if(id != SPEECH_BACKEND_NONE) StopOnId(id);
			g_workerSpeaking = false;
			InterlockedExchange(&g_announcedSeq, (LONG)InterlockedCompareExchange(&g_announceSeq, 0, 0));
			EnterCs();
			LONG cleared = g_qCount;
			g_qCount = 0;
			g_qHead = 0;
			LeaveCs();
			if(cleared > 0) InterlockedExchangeAdd(&g_statStoryDropped, cleared);
			LogLine(L"worker: stop 已执行（%s，清空未读 %ld 句）",
			        id == 0 ? L"无" : kRingName[RingIndexOfId(id)], (long)cleared);
		}

		// 1) 短播报优先：允许打断剧情朗读（只保留最新一条）
		LONG as = (LONG)InterlockedCompareExchange(&g_announceSeq, 0, 0);
		LONG aseen = (LONG)InterlockedCompareExchange(&g_announcedSeq, 0, 0);
		if(as != aseen)
		{
			wchar_t text[A11Y_MAX_TEXT];
			EnterCs();
			WCopy(text, A11Y_MAX_TEXT, g_announce);
			LeaveCs();
			InterlockedExchange(&g_announcedSeq, as);
			{
				int aid = (int)InterlockedCompareExchange(&g_backend, 0, 0);
				// 短播报必须"真打断"：直投后端先显式取消当前句（NVDA cancelSpeech / Tolk_Silence）。
				// 少了这一步，Speak 只是排到读屏器队尾，玩家要等旧剧情念完才听到菜单/热键反馈。
				if(IsDirectFeedId(aid) && aid != SPEECH_BACKEND_NONE) StopOnId(aid);
				if(BackendSpeak(text, L"announce")) InterlockedIncrement(&g_statAnnounceSpoken);
				MarkSpeaking((int)InterlockedCompareExchange(&g_backend, 0, 0), text);
			}
		}
		else if(!StillSpeaking())
		{
			// 2) 剧情：上一句念完才取下一句 → 两条剧情朗读不会互相打断
			g_workerSpeaking = false;
			wchar_t text[A11Y_MAX_TEXT];
			bool have = false;
			EnterCs();
			if(g_qCount > 0)
			{
				WCopy(text, A11Y_MAX_TEXT, g_qText[g_qHead]);
				g_qHead = (g_qHead + 1) % A11Y_QUEUE_MAX;
				g_qCount--;
				have = true;
			}
			LeaveCs();
			if(have)
			{
				LogLine(L"worker: 取剧情一句开始念（队列剩 %ld 句）",
				        (long)InterlockedCompareExchange(&g_qCount, 0, 0));
				if(BackendSpeak(text, L"story"))
				{
					InterlockedIncrement(&g_statStorySpoken);
					MarkSpeaking((int)InterlockedCompareExchange(&g_backend, 0, 0), text);
				}
			}
		}

		// 心跳：只在"有积压或正在念"时打，用来排查"游戏里不出声但日志没报错"这类现场
		{
			static DWORD hbTick = 0;
			DWORD nowTick = GetTickCount();
			LONG pend = (LONG)InterlockedCompareExchange(&g_qCount, 0, 0);
			if((pend > 0 || g_workerSpeaking) && (DWORD)(nowTick - hbTick) >= 3000)
			{
				hbTick = nowTick;
				LogLine(L"worker: 心跳 pending=%ld speaking=%d id=%d", (long)pend,
				        g_workerSpeaking ? 1 : 0, g_workerSpeakingId);
			}
		}

		while(PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
	}

	// 退出清理（仍在本线程，保证 COM 对象在本套间释放）
	// 顺序很重要：先放掉自己的 COM 对象(SAPI)，再收别人；Tolk_Unload 放最后 ——
	// 实测某些 Tolk 版本在 Tolk_Unload 里会 CoUninitialize 掉本线程的 COM 套间，
	// 之后再碰 ISpVoice 就是悬空指针 → 0xC0000005（见 evidence\a11y.log 的 COM 探针）。
	LogLine(L"cleanup: 开始");
	LogComState(L"cleanup 起点");
	LogLine(L"cleanup: SAPI release 前");
	SapiClose();
	LogComState(L"cleanup SAPI 之后");
	LogLine(L"cleanup: NVDA stop 前");
	NvdaClose();
	LogLine(L"cleanup: ZDSR stop 前");
	if(g_zdsr.dll) ZdsrClose();
	LogComState(L"cleanup NVDA/ZDSR 之后");
	if(g_tolk.dll)
	{
		LogLine(L"cleanup: Tolk_Unload 前");
		TolkClose();
		LogComState(L"cleanup Tolk 之后");
	}
	LogLine(L"cleanup: 结束");

	LogLine(L"worker: 退出 tid=%lu", (unsigned long)GetCurrentThreadId());
	if(comOk) CoUninitialize();
	return 0;
}

// ===========================================================================
//  公开 API
// ===========================================================================
int Speech_Init(void)
{
	if(InterlockedCompareExchange(&g_inited, 0, 0) != 0)
		return (InterlockedCompareExchange(&g_ready, 0, 0) == 1 &&
		        InterlockedCompareExchange(&g_backend, 0, 0) != SPEECH_BACKEND_NONE) ? 1 : 0;

	EnterCs();
	if(g_inited)
	{
		int r = (g_ready == 1 && g_backend != SPEECH_BACKEND_NONE) ? 1 : 0;
		LeaveCs();
		return r;
	}
	InterlockedExchange(&g_inited, 1);
	InterlockedExchange(&g_ready, 0);
	InterlockedExchange(&g_backend, SPEECH_BACKEND_NONE);
	InterlockedExchange(&g_quitReq, 0);
	InterlockedExchange(&g_cycleReq, 0);
	InterlockedExchange(&g_stopReq, 0);
	InterlockedExchange(&g_failStreak, 0);
	g_qHead = 0;
	g_qCount = 0;
	g_qText[0][0] = 0;
	g_announce[0] = 0;
	InterlockedExchange(&g_announceSeq, 0);
	InterlockedExchange(&g_announcedSeq, 0);
	InterlockedExchange(&g_statStoryEnq, 0);
	InterlockedExchange(&g_statStorySpoken, 0);
	InterlockedExchange(&g_statStoryDropped, 0);
	InterlockedExchange(&g_statAnnounceEnq, 0);
	InterlockedExchange(&g_statAnnounceSpoken, 0);
	g_workerSpeaking = false;
	g_workerSpeakingId = SPEECH_BACKEND_NONE;
	g_workerEndTick = 0;
	WCopy(g_lastError, 512, L"");

	BuildPaths();
	InterlockedExchange(&g_logReady, 1);
	EnsureA11yDir();
	CfgLoad();
	LogLine(L"==== Speech_Init ==== 游戏目录=%s", g_gameDir);
	LogLine(L"配置: %s（backend=%s rate=%d volume=%d re_read_key=%d backend_key=%s fail_threshold=%d）",
	        g_cfgPath, CfgGetStr(L"backend", L"auto"),
	        CfgGetInt(L"rate", 50), CfgGetInt(L"volume", 100),
	        CfgGetInt(L"re_read_key", 8), CfgGetStr(L"backend_key", L"Alt+Shift+F9"),
	        CfgGetInt(L"fail_threshold", 3));

	if(!g_evtWake) g_evtWake = CreateEventW(NULL, TRUE, FALSE, NULL);
	if(!g_evtReady) g_evtReady = CreateEventW(NULL, FALSE, FALSE, NULL);
	if(!g_evtCycleDone) g_evtCycleDone = CreateEventW(NULL, FALSE, FALSE, NULL);
	if(!g_evtWake || !g_evtReady || !g_evtCycleDone)
	{
		LogLine(L"致命: 事件对象创建失败");
		InterlockedExchange(&g_inited, 0);
		SetLastErrorText(L"事件对象创建失败");
		LeaveCs();
		return 0;
	}
	ResetEvent(g_evtReady);
	g_hWorker = CreateThread(NULL, 0, WorkerProc, NULL, 0, &g_workerTid);
	if(!g_hWorker)
	{
		LogLine(L"致命: CreateThread 失败 err=%lu", (unsigned long)GetLastError());
		InterlockedExchange(&g_inited, 0);
		SetLastErrorText(L"worker 线程创建失败");
		LeaveCs();
		return 0;
	}
	SetLastErrorText(L"");
	LeaveCs();

	DWORD w = WaitForSingleObject(g_evtReady, 5000);
	if(w != WAIT_OBJECT_0)
	{
		LogLine(L"Speech_Init: 等 worker 就绪超时（5s）");
		SetLastErrorText(L"语音后端探测超时");
		return 0;
	}
	return (InterlockedCompareExchange(&g_ready, 0, 0) == 1 &&
	        InterlockedCompareExchange(&g_backend, 0, 0) != SPEECH_BACKEND_NONE) ? 1 : 0;
}

// 拷贝文本到调用者缓冲，超长截断并记一次日志
static void CopyTextArg(const wchar_t *utf16, wchar_t *buf, const wchar_t *what)
{
	if(wcslen(utf16) >= A11Y_MAX_TEXT)
	{
		WCopy(buf, A11Y_MAX_TEXT, utf16);
		LogLine(L"speak[%s]: 文本超过 %d 字符，已截断", what, (int)(A11Y_MAX_TEXT - 1));
	}
	else WCopy(buf, A11Y_MAX_TEXT, utf16);
}

void Speech_Speak(const wchar_t *utf16)
{
	if(!utf16 || !utf16[0]) return;
	if(InterlockedCompareExchange(&g_inited, 0, 0) == 0)
	{
		Speech_Init();
		if(InterlockedCompareExchange(&g_inited, 0, 0) == 0) return;
	}
	if(!g_evtWake) return;

	wchar_t buf[A11Y_MAX_TEXT];
	CopyTextArg(utf16, buf, L"story");

	bool stored = false;
	EnterCs();
	if(g_qCount < A11Y_QUEUE_MAX)
	{
		int tail = (int)((g_qHead + g_qCount) % A11Y_QUEUE_MAX);
		WCopy(g_qText[tail], A11Y_MAX_TEXT, buf);
		g_qCount++;
		stored = true;
	}
	else
	{
		// 队列满：丢最旧的一条（最新的剧情更贴近玩家当前位置）
		g_qHead = (g_qHead + 1) % A11Y_QUEUE_MAX;
		int tail = (int)((g_qHead + g_qCount - 1) % A11Y_QUEUE_MAX);
		WCopy(g_qText[tail], A11Y_MAX_TEXT, buf);
	}
	LeaveCs();

	if(stored) InterlockedIncrement(&g_statStoryEnq);
	else
	{
		InterlockedIncrement(&g_statStoryDropped);   // 被顶掉的那条算丢弃
		InterlockedIncrement(&g_statStoryEnq);
		// 队列满说明"念得比出字慢"，游戏里会持续发生 → 节流，最多 2 秒记一条
		static DWORD lastFullLog = 0;
		DWORD nowTick = GetTickCount();
		if(lastFullLog == 0 || (DWORD)(nowTick - lastFullLog) >= 2000)
		{
			lastFullLog = nowTick;
			LogLine(L"speak[story]: 队列已满（%d 句），丢弃最旧一句（日志已节流）", (int)A11Y_QUEUE_MAX);
		}
	}
	SetEvent(g_evtWake);
}

void Speech_SpeakImmediate(const wchar_t *utf16)
{
	if(!utf16 || !utf16[0]) return;
	if(InterlockedCompareExchange(&g_inited, 0, 0) == 0)
	{
		Speech_Init();
		if(InterlockedCompareExchange(&g_inited, 0, 0) == 0) return;
	}
	if(!g_evtWake) return;

	wchar_t buf[A11Y_MAX_TEXT];
	CopyTextArg(utf16, buf, L"announce");
	EnterCs();
	WCopy(g_announce, A11Y_MAX_TEXT, buf);
	LeaveCs();
	InterlockedIncrement(&g_announceSeq);
	InterlockedIncrement(&g_statAnnounceEnq);
	SetEvent(g_evtWake);
}

void Speech_Stop(void)
{
	if(InterlockedCompareExchange(&g_inited, 0, 0) == 0) return;
	// 全局停止：清空待读队列 + 丢掉待播短播报 + 让 worker 打断当前这句
	EnterCs();
	LONG cleared = g_qCount;
	g_qCount = 0;
	g_qHead = 0;
	LeaveCs();
	if(cleared > 0) InterlockedExchangeAdd(&g_statStoryDropped, cleared);
	InterlockedExchange(&g_announcedSeq, (LONG)InterlockedCompareExchange(&g_announceSeq, 0, 0));
	InterlockedExchange(&g_stopReq, 1);
	if(g_evtWake) SetEvent(g_evtWake);
}

int Speech_CycleBackend(void)
{
	if(InterlockedCompareExchange(&g_inited, 0, 0) == 0)
	{
		if(Speech_Init() == 0) return SPEECH_BACKEND_NONE;
	}
	if(!g_evtCycleDone) return (int)InterlockedCompareExchange(&g_backend, 0, 0);
	ResetEvent(g_evtCycleDone);
	InterlockedExchange(&g_cycleReq, 1);
	SetEvent(g_evtWake);
	WaitForSingleObject(g_evtCycleDone, 3000);
	return (int)InterlockedCompareExchange(&g_backend, 0, 0);
}

int Speech_SelectBackend(const wchar_t *key)
{
	int want = ParseBackendKey(key);
	if(want == SPEECH_BACKEND_NONE) return 0;
	int steps = 0;
	for(;;)
	{
		int now = Speech_BackendId();
		if(now == want) return 1;
		if(++steps > A11Y_SLOTS) return 0;
		int next = Speech_CycleBackend();
		if(next == SPEECH_BACKEND_NONE || next == now) return (next == want) ? 1 : 0;
	}
}

const wchar_t *Speech_BackendName(void)
{
	int i = RingIndexOfId((int)InterlockedCompareExchange(&g_backend, 0, 0));
	return (i >= 0) ? kRingName[i] : L"无";
}

int Speech_BackendId(void)
{
	return (int)InterlockedCompareExchange(&g_backend, 0, 0);
}

int Speech_IsReady(void)
{
	return (InterlockedCompareExchange(&g_ready, 0, 0) == 1 &&
	        InterlockedCompareExchange(&g_backend, 0, 0) != SPEECH_BACKEND_NONE) ? 1 : 0;
}

const wchar_t *Speech_LastError(void)
{
	static wchar_t copy[512];
	EnterCs();
	WCopy(copy, 512, g_lastError);
	LeaveCs();
	return copy;
}

const wchar_t *Speech_LogPath(void)    { return g_logPath[0] ? g_logPath : L""; }
const wchar_t *Speech_ConfigPath(void) { return g_cfgPath[0] ? g_cfgPath : L""; }
const wchar_t *Speech_GameDir(void)    { return g_gameDir[0] ? g_gameDir : L""; }

int Speech_ConfigInt(const wchar_t *key, int def)
{
	if(!key) return def;
	EnterCs();
	int v = CfgGetInt(key, def);
	LeaveCs();
	return v;
}

// 注意：返回的是内部缓冲（4 个轮转），同一次函数调用参数表里连续调两次要留神：
// 超过 4 次调用后指针会被复用。需要长期保存请自行拷贝。
const wchar_t *Speech_ConfigStr(const wchar_t *key, const wchar_t *def)
{
	static wchar_t bufs[4][CFG_VAL_CCH];
	static volatile LONG idx = 0;
	if(!key) return def;
	LONG n = InterlockedIncrement(&idx);
	wchar_t *buf = bufs[(ULONG)(n - 1) % 4];
	EnterCs();
	WCopy(buf, CFG_VAL_CCH, CfgGetStr(key, def));
	LeaveCs();
	return buf;
}

unsigned long Speech_Stat(int which)
{
	switch(which)
	{
	case SPEECH_STAT_STORY_ENQ:      return (unsigned long)InterlockedCompareExchange(&g_statStoryEnq, 0, 0);
	case SPEECH_STAT_STORY_SPOKEN:   return (unsigned long)InterlockedCompareExchange(&g_statStorySpoken, 0, 0);
	case SPEECH_STAT_STORY_DROPPED:  return (unsigned long)InterlockedCompareExchange(&g_statStoryDropped, 0, 0);
	case SPEECH_STAT_PENDING:        return (unsigned long)InterlockedCompareExchange(&g_qCount, 0, 0);
	case SPEECH_STAT_ANNOUNCE_ENQ:   return (unsigned long)InterlockedCompareExchange(&g_statAnnounceEnq, 0, 0);
	case SPEECH_STAT_ANNOUNCE_SPOKEN:return (unsigned long)InterlockedCompareExchange(&g_statAnnounceSpoken, 0, 0);
	}
	return 0;
}

void Speech_Shutdown(void)
{
	EnterCs();
	if(!g_inited)
	{
		LeaveCs();
		return;
	}
	InterlockedExchange(&g_inited, 0);
	InterlockedExchange(&g_quitReq, 1);
	if(g_evtWake) SetEvent(g_evtWake);
	HANDLE th = g_hWorker;
	g_hWorker = NULL;
	g_qCount = 0;
	g_qHead = 0;
	LeaveCs();

	if(th)
	{
		DWORD w = WaitForSingleObject(th, 5000);
		if(w != WAIT_OBJECT_0) LogLine(L"Speech_Shutdown: 等 worker 退出超时（5s）");
		CloseHandle(th);
	}
	InterlockedExchange(&g_backend, SPEECH_BACKEND_NONE);
	InterlockedExchange(&g_ready, 0);
	g_workerSpeaking = false;
	LogLine(L"==== Speech_Shutdown 完成 ====");
}
