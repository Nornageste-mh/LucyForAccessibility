# 发布前审计报告 — 0.2.4.0

- 审计对象：`D:\DSHWorkSpace\LucyForAccessibility`（32 位 Windows 原生 C++ 代理 DLL，产物 `build\out\d3dx9_40.dll`）
- 版本：`src\a11y.h:17` `#define A11Y_VERSION L"0.2.4.0"`
- 审计方式：只读源码 + 只读产物/日志 + 只读构建脚本；**未修改任何源码、未改动游戏目录、未运行游戏**
- 证据标注：`[实测]` = 本机命令输出；`[文件]` = `路径:行号`；`[推断]` = 无直接证据的合理推测（写明依据与验证方法）
- 审计时间：2026-11-19
- **审计基线**：提交 `b90d9c5`（`0.2.4.0: …`），审计期间**工作区出现他人未提交改动**（`README.md` 05:07、`docs\常见问题.md` 05:07、`tools\install.ps1` 05:03、`src\menus.cpp` 05:10，均为修复类小改）。本报告的行号取自 04:55–05:15 读取时的工作区状态；被这些改动顺手修掉的条目已在正文标注"审计期间已修"。

## 结论摘要

1. **不建议按当前状态直接发布**：代码结构与发布脚本成熟（代理层、三道守卫、zip 校验齐备），但默认配置与文档存在会直接误导玩家的缺陷。
2. 阻断项 6 条，最严重两条：① 默认安装下**配音判据完全不生效**（`voice_l1` 默认 0），与 0.2.4.0 主打功能相反；② `Alt+Shift+F9` 在低层键盘钩子内阻塞最长 **3 秒**，可能被系统摘钩导致键盘整体失灵。
3. `DllMain` 内做文件 IO/写配置/起线程，违反 `speech.h:11-14` 自述契约，有加载器锁死锁风险。
4. 音频判据状态表 `g_rec[256]` / `g_file[64]` **只增不减**，长局后记录耗尽或命中陈旧对象，判据在中后段失效/误判。
5. 发布物本身干净：仓库 32 个文件无游戏资源；`dist` zip 内无 NVDA DLL、无中间产物；DLL 哈希与构建产物、游戏目录部署件三者一致（`926C48C1…E475`，257536 字节）。
6. 文档与实现：键位、日志路径、`config.ini` 名称一致；但 README 顶部与"常见问题"仍写"标题画面未打通"（与 0.2.4.0 实现及另外 6 处文档矛盾），"5 条滑条"实际只有 4 条。
7. 可复现构建：只依赖 `vcvars32.bat` + 系统库；`tools\gen_forward.py` 输出确定（`sorted(set(names))`）；但产物**无版本资源**、发布脚本不校验导出表、zip 会把内部文档一起打进去。

## 阻断项（必须改才能发布）

### B1 `DllMain` 内做文件 IO / 写配置 / 起线程

- 证据：`[文件] src\proxy.cpp:104-133`——`DllMain` 里依次调 `LogInit(dir)`（`:115`，内部 `CreateDirectoryW` + `_wfopen`，见 `src\a11y.cpp:34-50`）、`Logf(...)`（`:116`，每行 `fwrite`+`fflush`，见 `a11y.cpp:54-76`）、`ConfigLoad()`（`:118`，配置文件不存在时 `ConfigSave()` 直接 `_wfopen`+`fprintf`，见 `a11y.cpp:107-111`、`:140-160`）、`StartA11y(self)`（`:131`，`CreateThread`，见 `a11y.cpp:372-377`）。
- 契约冲突：`[文件] src\speech\speech.h:11-14` 明写"**绝对不要在 DllMain 里调 Speech_Init**……正确做法：DllMain 只保存 hModule 直接 return TRUE"；框架要求"`Logf` 不进 `DllMain`"。当前实现比注释更激进（文件 IO + 建线程）。
- 后果：加载器锁内做文件系统调用，磁盘/杀软/网络盘挂起时会拖住全进程加载；`CreateThread` 在 `DllMain` 内启动的线程若先跑起来调 `LoadLibraryW`（`proxy_resolve`，`proxy.cpp:41-64`）会等加载器锁，行为不可预期。
- 建议改法：`DllMain` 只保存 `g_self` 并 `return TRUE`；把 `LogInit`/`ConfigLoad`/`StartA11y` 移到 `TickThread` 首段执行（`a11y.cpp:337-340` 已在注释里承认"Everything heavy is initialised here, never in DllMain"，只是没做到）；初始化失败只降级，不阻塞游戏。

### B2 默认配置下配音判据完全不生效（与 0.2.4.0 主打功能相反）

- 证据：`[文件] src\audio\audio_hook.cpp:182` `g_enableL1 = 0`、`:237` `IniInt("voice_l1", 0)`；`:1324-1339` L1 关闭时**不打** audiere IAT（并打印"L1 被配置关闭"）；`g_rec` 只由 L1 钩子写入（`:938`、`:971`）；`OnMusicPlay` 无记录时 `:1216-1218` 直接 `return`（打印 `MUSIC_PLAY ... (no record)`）。
- 实测佐证：`[实测]` 游戏目录 `a11y\audio_hook.log`（67 行，两次运行）第二次运行标 `l0/l1/l3=101`，随后 `MUSIC_PLAY t=… obj=… (no record)`，全文件 `HEUR_VOICE`=0、`VOICE_PLAY`=0、`JUDGE gen=… voiced=1`=0；`a11y.log` 只有 4 条 `line ready: voiced=0`，没有一条 `voiced=1`。
- 结论：全新安装（`config.ini` 未写 `voice_l1`）时，**所有台词都会被当旁白自动朗读**，恰好是 `[文件] README.md:152-155` 宣称的反面；0.2.4.0 的"48 句中 3 句静默"来自测试机 ini 里手工打开的 L1，发布默认值没有跟上。
- 建议改法：`audio_hook.cpp:237` 改为 `IniInt("voice_l1", 1)`（同一日志显示 L1 IAT 5/5 安装成功、未崩；崩的是 `voice_l1_vtable`，默认仍为 0），同步改 `audio_hook.h:39` 注释；若出于稳定性要保留默认关，则必须在 `Audio_Init` 末尾语音播报一次"配音判据未启用"，并在 README 说明。

### B3 判据状态表只增不减：`g_rec` / `g_file` 永不回收

- 证据：`[文件] src\audio\audio_hook.cpp:800-801` `FileRec g_file[64]`、`:824` `Rec g_rec[AUDIO_MAX_REC=256]`；`:844-852` `RecAlloc` 只置 `inUse=1`；`:865-878` `Hook_CreateMemoryFile` 只置 `inUse=1`；全文件 `g_rec[i].inUse` / `g_file[i].inUse` **无任何置 0 处**（`grep inUse` 13 处匹配，置 0 的只有 `g_src`，见 `:949`、`:981`）。
- 后果：第 257 次 `OpenSound` 后 `RecAlloc` 返回 NULL（`:938-939`、`:971-972` 直接 return）→ 之后所有播放都不再记录，`OnMusicPlay` 恢复成"(no record)"；第 65 个 `AdrCreateMemoryFile` 之后新音源 size/magic 全 0（`:891-895` 匹配不到）→ size/dur 阈值判据失效。更隐蔽的是 `RecFindObj`（`:854-863`）按裸指针匹配，引擎释放后复用同一地址的**新对象会命中旧记录**，读出错误的 `cat/conf/size`。
- 建议改法：在 `Hook_OpenSound`/`Hook_OpenSoundEffect` 对应 stop/unref 路径回收；短期至少改成"满时 LRU 覆盖"（`RecAlloc` 找不到空槽就覆盖 `obj`/`origVt` 最旧的一条并清零），`g_file` 改成 64 槽 FIFO 环；再加一条自检日志"rec full"。

### B4 低层键盘钩子内做阻塞操作（磁盘日志 / 3 秒等待 / 数百毫秒 Sleep）

- 证据：`[文件] src\a11y.cpp:297-318` `LowLevelKb` 内直接调 `Logf`（`:303-308`，`Logf` 每行 `fflush`，`a11y.cpp:74`）与 `HandleHotkey`；`HandleHotkey` 的 F9 分支 `:284-287` → `Speech_CycleBackend()`（`src\speech\speech.cpp:1818-1829`）**等待 `g_evtCycleDone` 最多 3000 ms**；若 speech 未初始化还会在钩子里调 `Speech_Init`（`:1822`，起线程/COM/LoadLibrary）。降级点击路径 `menus.cpp:270-292`：`EnsureForeground`（`:240-263`，`Sleep(120)`×2 + `AttachThreadInput`）+ `Sleep(120)` + `Sleep(60)`，全部在钩子线程同步执行。
- 后果：`WH_KEYBOARD_LL` 回调超过 `LowLevelHooksTimeout`（默认约 300 ms，Win10 可调）会被系统跳过甚至摘除钩子——表现是"按一次 F9 后键盘全部失灵"或明显卡顿；同一问题在 Speech 初始化慢的机器上更严重。
- 建议改法：钩子回调只做"记 vk → 投递到队列"并立刻 `CallNextHookEx`；`CycleBackend`/重读/点击由 `TickThread` 取出执行；`Cycle` 改成非阻塞语义（置 `g_cycleReq` 后立即返回，后端名由 worker 播报）；`Logf` 在钩子路径改写入内存环，由 tick 线程落盘。

### B5 README 顶部与"常见问题"仍写"标题画面未打通"，与 0.2.4.0 实现及另外 6 处文档直接矛盾

- 矛盾证据：`[文件] README.md:13`（"标题画面开始游戏这一环还是未打通（须先点一次鼠标）"）、`README.md:194-195`（"为什么开始游戏还要先点一下鼠标：标题画面这一环补丁还没打通"）× 对照 `README.md:75`、`CHANGELOG.md:18/32/61/71`、`docs\安装说明.md:53/165`、`docs\常见问题.md:54-56/83`、`docs\交互形态判定.md:115/146`、`docs\project_status.md:101` 全部写"0.2.4.0 已打通，实机 3 次"。
- 后果：README 是用户第一入口，第 13 行会让玩家直接以为核心功能没做，是发布前最显眼的对外错误（截至审计结束时 `README.md:13`、`:194-195` 仍在，未被 05:07 的修改覆盖）。
- 建议改法：删除 `README.md:13` 的告示块与 `:194-195` 的旧问答，改写为"标题画面 0.2.4.0 起可用键盘启动；4 项朗读顺序/跳过细节未逐项核对"。

### B6 键位配置项承诺未实现（`re_read_key` / `backend_key` / `fail_threshold`）

- 证据：`[文件] src\speech\speech.h:27` 的契约注释、`src\speech\speech.cpp:364-383` 的 `kDefaultIni` 模板都会写出这三个键、`:1683` 日志还会回显 `re_read_key=8 backend_key=Alt+Shift+F9`；但 `[文件] src\a11y.cpp:100-103` 的热键是编译期常量（`VK_BACK`/`VK_MENU`/`VK_F9`），`HandleHotkey`（`:272-295`）只用 `g_cfg.keyRepeat`/`g_cfg.keyBackend`，`ConfigLoad`（`:122-128`）**不解析**这三个键。
- 后果：玩家/文档作者以为改 `a11y\config.ini` 能改键，实际无效；键位与游戏原生键冲突时无法自救（README:175 自己也写"若与游戏原生键冲突，未测"）。
- 建议改法：二选一——(a) 在 `ConfigLoad` 里解析 `re_read_key`/`backend_key`（推荐，约 20 行，`backend_key` 按 "Alt+Shift+F9" 解析成 vk+修饰位）；(b) 从 `kDefaultIni` 与 `speech.h:27` 注释里删掉这三键并改成注释说明"键位固定"。发布前必须与文档二选一对齐。

## 建议项（不阻断，但发布前值得处理）

- **S1 日志降噪/隐私**：`voice_log` 默认 1（`audio_hook.cpp:52`、`:235`），且每条事件 `OutputDebugStringA` + `CreateFileA`/`WriteFile`/`CloseHandle`（`:153-173`，运行在游戏音频线程），文件无上限；`a11y.log` 每行 `fflush`（`a11y.cpp:74`）且写入完整台词与资源路径（`[实测]` 游戏目录 `a11y.log` 271 行含"第1条：机器人不得伤害人类…"）。建议 `voice_log` 默认 0、只记录失败与判定结果；日志加 1MB 上限/轮转；`config.ini` 的 `log_draws` 保持 0（`[实测]` 部署机 `a11y\config.ini` 23 字节，内容为 `log_draws=1`，属测试遗留）。
- **S2 a11y.log 中间重复写 BOM**：`a11y.cpp:40` 以 `"ab"` 打开、`:44` 用 `ftell(g_log)==0` 判空后写 BOM；`[实测]` `a11y.log` 第 5 行行首残留 BOM，且文件里有 2 个进程的 `loaded` 记录 → `[推断]` append 流的初始 `ftell` 在 MSVC 下可能为 0，导致每次进程加载都追加一个 BOM。验证：单独用 `_wfopen(...,L"ab")` 打开非空文件后打印 `ftell`。改法：改用 `GetFileSizeEx`（speech 层就是这么做的，`speech.cpp:224-237`）或 `fseek(END)` 后判断。
- **S3 路径拼接无边界检查（内存安全）**：`a11y.cpp:37/39/106/143`、`proxy.cpp:36` 用 `wsprintfW` 往 `wchar_t[MAX_PATH]` 里拼 `baseDir + "\a11y"`、`+ "\config.ini"`、`+ "\d3dx9_40_orig.dll"`，`baseDir` 本身来自 `GetModuleFileNameW(..., MAX_PATH)`，接近 260 字符的安装路径会越界写全局。改法：全部换 `_snwprintf_s(..., _TRUNCATE)`，并在 `LogInit` 里若路径被截断则退到 `%LOCALAPPDATA%\LucyA11y`。
- **S4 退出与卸载**：`Speech_Shutdown`（`speech.h:100` / `speech.cpp:1915`）与 `Audio_Shutdown`（`audio_hook.h:86` / `audio_hook.cpp:1374`）**全工程无调用点**；`DllMain` 非 ATTACH 直接 `return TRUE`（`proxy.cpp:106`）；`TickThread`（`a11y.cpp:363-368`）、`KeyThread`（`:330-333`）、audio `InitThread`（`audio_hook.cpp:1357-1365`）均无退出路径；`Audio_Shutdown` 本身也只恢复 L0/L3，不恢复 L0b vtable 与 IAT。结论：**进程正常退出不会卡死**（ExitProcess 直接终止线程），但若游戏/覆盖层调用 `FreeLibrary` 释放本 DLL，残留钩子指向已卸载代码会崩。建议至少在 `DLL_PROCESS_DETACH` 里做"只置退出标志、不 LoadLibrary/不 COM"的最小清理，并在 README 写清"仅支持随进程退出"。
- **S5 注释过期**：`audio_hook.h:39` 写"默认 `voice_min_conf=2`、conf=1 不采信、l0/l1/l3 默认全开、0~2500ms 窗口"，代码是 `min_conf=1`（`:51/234`）、`l1=0`（`:182/237`）、`lag<=900`（`:1238`）。`CHANGELOG.md:43` 已记录把默认改成 1，请一并改头文件注释。
- **S6 写死 1280×720 客户区**：菜单矩形全是 1280×720 客户区坐标（`menus.cpp:11`、`:43`、`:94-95`、`:147`），代码里**没有 `GetClientRect` 校验**（`grep GetClientRect` 0 命中）；`ToScreen`/`ClientToScreen`（`:229-234`）只在客户区恰好 1280×720 时成立。若窗口被缩放/全屏方式不同，合成点击会落到别的控件（含 risky 的"标题 / 退出游戏"，`menus.cpp:50/61`）。建议 `MenuInit` 里校验客户区，不符时禁用菜单拟态并语音提示，避免盲点乱按。
- **S7 产物无版本信息**：全仓库无 `.rc`/`.manifest`（`[实测]` glob 0 命中），所以 `[实测]` `d3dx9_40.dll` 的 `FileVersion/ProductVersion/FileDescription/CompanyName` 全空；`a11y\install.json` 的 `version` 落成 `"unknown"`（`[实测]`）。建议加最小 `VERSIONINFO`（与 `a11y.h` 同源同步），发布包加 `VERSION.txt`。注：审计期间 `tools\install.ps1` 的 `Get-PatchVersion`（05:03 未提交改动）已补上"仓库 `src\a11y.h` → 包内 README → CHANGELOG"的兜底，按新脚本重装应能取到 `0.2.4.0`；但历史 `install.json` 仍是 `unknown`，且该兜底依赖包内 README 的"当前版本："字样（本次实测该正则可命中）。
- **S8 发布校验可加强**：`build\release.ps1:140-153` 只查 PE 签名/machine/magic，不比对产物导出表与 `src\exports_expected.txt`（336 条）；守卫 2（`:118-133`）搜索的 `A11Y_DEV`/`a11y_diag`/`\\\\.\\pipe\\a11y` 等标记在源码里**根本不存在**（`grep A11Y_DEV` 在 `src\` 下 0 命中；`grep a11y_diag|a11y_load|a11y_cmd` 同样 0 命中），等于空转；`-SkipBuild` 可复用旧产物。建议：`dumpbin /exports` 与 `exports_expected.txt` 逐行比对；`release.ps1` 默认强制重建（`-SkipBuild` 时打印产物 mtime 与源码最新 mtime 对比）；把空转的守卫 2 换成真正有效的"调试通道"检查（例如断言默认配置里 `voice_log=0`）。
- **S9 发布包会把内部文档打进去**：`release.ps1:179-182` 用 `docs\*.md` 全量拷贝，`[实测]` 当前 `dist\LucyForAccessibility-0.2.4.0.zip` 已含 `docs\project_status.md`、`docs\交互形态判定.md`；本次审计报告 `docs\AUDIT-0.2.4.0.md` 也会被下一版打进包。建议改成白名单（只放 `安装说明.md`/`操作约定.md`/`常见问题.md`），内部文档单独留给仓库。
- **S10 dist 卫生**：`[实测]` `dist\` 同时有 `LucyForAccessibility-0.2.2.0.zip`（179843 字节，旧）与 `LucyForAccessibility-0.2.4.0.zip`（190100 字节，当前）；`build\release_stage\0.2.2.0\` 也残留。二者都已被 `.gitignore` 排除（`build/out:49`、`build/release_stage:50`、`dist/*:74`），发布前删旧包避免误传。
- **S11 许可证表述**：`NOTICE.md:31-34` 只说"再分发条款尚未确认"，未点明 NVDA 本体是 GPL 系。建议补一句："NVDA 本体为 GPLv2；其控制器客户端本包**不随包分发、不修改**，运行时从用户本机 NVDA 安装目录加载"。另 `[文件] docs\常见问题.md:14-17` 让用户"从争渡读屏的游戏补丁包 `nvda-bridge\x86\nvdaControllerClient32.dll` 改名使用"，有许可与来源风险，建议改成"从本机 NVDA 安装目录复制"。核对结果：`[实测]` 当前 zip 内**没有**任何 `nvdaControllerClient*.dll`，`tools\install.ps1:180-182` 的"包内若有就复制"分支在本版不会触发，`release.ps1:195` 也禁止额外 DLL —— 与 NOTICE 一致。游戏目录 `a11y\` 下现存的 `nvdaControllerClient.dll`/`nvdaControllerClient32.dll` 是测试遗留，不在包内。
- **S12 小竞态/死代码**：`RepeatLast()`（`a11y.cpp:174-179`）在锁外读 `g_repCount/g_repPos`，与 `RepeatPush` 的 `wcsncpy_s` 并发时可能念到半句；`g_repCs`/`g_cs`/`g_logCs` 均惰性初始化（`a11y.cpp:187`、`fonthook.cpp:49-55`、`audio_hook.cpp:110-120`）且非原子，多线程首次调用会重复 `InitializeCriticalSection`；`HandleHotkey` 用按键事件跟踪 Alt/Shift（`a11y.cpp:275-276`）而 Ctrl 用 `GetAsyncKeyState`（`:279`），窗口切换时可能残留修饰键状态；`menus.cpp:249-250` 连续两次 `GetForegroundWindow()`（第二条是死代码）。

## 逐项清单结论

### 1. 调试/诊断脚手架

- **窗口过程子类化：无**（`grep SetWindowLongPtr|CallWindowProc|Subclass` 0 命中）——这一项干净。
- **条件编译调试通道：无实际内容**。`build.cmd:57-63` 的 `dev` 只切 `/Od /Z7` 与 `A11Y_DEV`，但源码里没有任何 `#ifdef A11Y_DEV`（grep 0 命中），`AUDIO_HOOK_SELFTEST` 也未定义（`audio_hook.h:128`）→ `release.ps1` 守卫 2 搜索的标记字符串在两种构建里都不会出现（见 S8）。
- **默认打开的诊断项**：`voice_log=1`（`audio_hook.cpp:52/235`，每事件写文件+`OutputDebugStringA`）；`log_draws` 代码默认 0（`a11y.cpp:99`）但**部署机 ini 为 1**（`[实测]`）；`voice_vtspy` 默认 0（`:241`，诊断代码 `:1164-1187` 留存在产物里但关闭）；`log_draws` 打开时 DRAW 日志有 `s_seen<20000` 上限、按键日志有 `s_keySeen<300` 上限（`a11y.cpp:305`、`fonthook.cpp:349-372`）→ 属"临时限制"，不影响关闭状态。
- **写死的一次性测试坐标**：菜单/选项矩形是产品坐标而非一次性脚本坐标，但写死 1280×720 客户区（见 S6）；`[实测]` 未在其它分辨率验证。
- **建议清单**：① `voice_log` 默认改 0（或仅失败时写）；② 部署机 `config.ini` 的 `log_draws=1` 恢复 0，并在 `release.ps1` 人工清单里加一条"确认发布机 ini 是默认值"；③ 保留 `voice_vtspy`/`AUDIO_HOOK_SELFTEST` 可以，但建议在发布构建里用宏整体剔除（现在它们只是默认关，代码仍在包里）。

### 2. 健壮性

- **空指针/未初始化**：`g_wnd`/`g_self`/`g_log`/`Rec*` 返回值均有判空（`menus.cpp:243`、`inputhook.cpp:113/147`、`audio_hook.cpp:938/971`）；`Speech_*` 入口都有 `g_inited` 检查。**未见未检查的解引用**；薄弱点是 `RepeatLast` 无锁读与 `RecFindObj` 可能命中陈旧记录（S12/B3）。
- **临界区配对**：全部 `EnterCriticalSection` 都能追到 `LeaveCriticalSection`（逐文件核对 `a11y.cpp:72/75、188/191/199`；`fonthook.cpp` 8 对；`audio_hook.cpp` 20+ 对，其中 `:410-414` 是显式先 `Leave` 再 `return`，正确）。问题在惰性初始化非原子（S12）。
- **`__try/__except`**：全工程仅 1 处（`speech.cpp:928-936`），用于 SAPI `ISpVoice::Release` 的 AV 兜底，注释 `:916-918` 记录了实测 `0xC0000005` 的成因 → **用在必要处，可接受**。
- **`VirtualProtect` 恢复**：9 处全部恢复原属性（`inputhook.cpp:78-81`；`audio_hook.cpp:774-779、1063-1066、1110-1112、1178-1183、1295-1300、1383-1386、1396-1399`），且均为"改→写→恢复→`FlushInstructionCache`"，写法一致。`PatchIat`（`inputhook.cpp:52-86`）会遍历所有匹配槽位而非只改第一个，`original` 只记首处，无副作用。
- **退出/线程卡死**：见 S4。结论：进程退出不卡死，但无清理、无卸载路径；`Speech_Shutdown`/`Audio_Shutdown` 是死代码。
- **全局缓冲越界**：`g_rep[24][2048]` 用 `wcsncpy_s(...,_TRUNCATE)`（`a11y.cpp:195`）、`g_evt[64]` 取模轮转（`audio_hook.cpp:283-284`）、`DrawRec` 64/128 环有界、`VoiceEvt.name[160]` 用 `_snprintf_s(_TRUNCATE)`（`:289`）→ **环本身没有越界**；`g_rec`/`g_file` 耗尽的表现是"停止记录/脏记录"，不是数组越界。真正的越界风险只在 `wsprintfW` 路径拼接（S3）。另外 `Logf` 的 `msg[2048]`/`line[4300]` 与 `_vsnwprintf_s(_TRUNCATE)` 配合是安全的。

### 3. 发布物洁净度

- **仓库**：`[实测]` `git ls-files` 32 个文件（审计开始时 `git status --porcelain` 为空，审计期间出现的 4 个未提交改动见"审计基线"）；全是源码/脚本/文档/构建发布脚本，**无游戏资源、无台词文本、无图片音频、无存档、无 `.dll`/`.zip`/`.obj` 入库**。
- **忽略规则**：`[实测]` `git check-ignore -v` 命中 `build/out`（`.gitignore:49`）、`build/release_stage`（`:50`）、`dist/*`（`:74`，只放行 `dist/README.md`）→ `dist\`、`build\out\` 均不会入库，符合要求。
- **发布包内容**：`[实测]` `dist\LucyForAccessibility-0.2.4.0.zip`（190100 字节）条目只有 `d3dx9_40.dll` + README/CHANGELOG/LICENSE/NOTICE + `docs\*.md` × 5 + `tools\install.ps1`/`uninstall.ps1`；包内 DLL 的 SHA256 与 `dist\d3dx9_40.dll`、`build\out\d3dx9_40.dll`、游戏目录部署件完全一致（`926C48C15456B46E13AA63DA8518B35F7D4E21EB50F6411EC2F3E00BC5FFE475`，257536 字节）；**包内无 `nvdaControllerClient*.dll`**（与 NOTICE、"不随包分发"一致）。
- **许可证一致性**：`LICENSE` 为 MIT（Copyright (c) 2026 Nornageste-mh）；`NOTICE.md:17-18` 声明不含游戏本体/文本/图片/音频/存档（与包内容一致）；`:31-33` 声明 nvdaControllerClient 不随包分发；`:39-41` 声明 audiere/d3dx9 原版不复制（实际包内只有本工程构建的代理 `d3dx9_40.dll`，`:55` 行表格已把它列为 MIT → 与实际一致）。需要补 GPL/LGPL 明示（S11）。
- **遗留**：`dist\` 有旧 0.2.2.0 zip、`build\release_stage\0.2.2.0\` 残留（S10）；游戏目录里有测试遗留的 NVDA 客户端 DLL 与 `lucy_probe_*.log`（不在包内，但建议在干净目录做最终验收）。

### 4. 文档与代码一致性

- **键位：一致**——`README.md:32-33/112-114`、`docs\操作约定.md:38-40`、`docs\安装说明.md:148-150` 的 Backspace / Ctrl+Backspace / Alt+Shift+F9 ↔ `a11y.cpp:100-103`（`keyRepeat=VK_BACK`、Ctrl 分支 `:279/290`、`keyBackend=VK_F9` + Alt/Shift `:284`）。
- **日志路径：一致**——文档 `游戏目录\a11y\a11y.log` ↔ `a11y.cpp:37-40`（`<baseDir>\a11y\a11y.log`，`baseDir` 为 DLL 所在目录即游戏目录）。
- **配置文件名：一致**——`a11y\config.ini` ↔ `a11y.cpp:106`、`audio_hook.cpp:221/227`（后者先找 exe 目录再找 DLL 目录，实际都指向游戏目录）。
- **不一致项**：① README 顶部/尾部仍写"标题画面未打通"（B5）；② 文档统一写"**5 条滑条**"（`README.md:26/128`、`操作约定.md:52/84`、`交互形态判定.md:66`、`常见问题.md:86/118`、`project_status.md:53/93`、`安装说明.md:162`），实现只有 **4 条 `K_SLIDER`**（`menus.cpp:66-70`：文字速度/背景音乐/效果音/人物声音）+ 1 个 `K_TOGGLE`（`:71` 文字窗口）——建议文档改"4 条滑条 + 1 个开关"，或补第 5 条滑条；③ `re_read_key`/`backend_key`/`fail_threshold`（B6）；④ `audio_hook.h:39` 注释过期（S5）；⑤ `README.md:140` 称"点击在后台线程排队执行，不阻塞键盘钩子"——仅当 `InputHookReady()` 为真时成立，降级路径会阻塞（`menus.cpp:282-291`，S4/B4）；⑥ `安装说明.md:29` 仍把 `nvdaControllerClient.dll` 无条件列进"安装后游戏目录里会多出的文件"，但安装器不会复制它（`install.ps1:223-226` 只在包内有该 DLL 时才复制，实测包内没有）→ 应加"可选，需自行放置"的注脚（`README.md:92` 审计期间已被改成"可选：自己放的 32 位 NVDA 客户端（不随包分发）"，安装说明需同步）。

### 5. 可复现构建

- **依赖**：`[文件] build\build.cmd:25-51` 只要求环境变量 `VCVARS` 指向 `vcvars32.bat` 并校验 `VSCMD_ARG_TGT_ARCH==x86`；编译选项 `/nologo /EHsc /MT /O1 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /D__WIN32__ /utf-8`（`:68`），链接只用 `user32.lib gdi32.lib ole32.lib oleaut32.lib winmm.lib`（`:141-144`）→ **只依赖 VS2022 vcvars32 + 系统库，无第三方库**（链接行里没有任何第三方 `.lib`，`/MT` 静态 CRT 也意味着游戏进程不新增 CRT 运行库依赖）。
- **生成器确定性**：`[文件] tools\gen_forward.py:41` `names = sorted(set(names))`，`:98-101` 以 `encoding="utf-8", newline="\n"` 写出，模板无时间戳/随机数 → **同一原始 DLL 输入产出同一 `forward.cpp` / `exports_expected.txt`**。它依赖第三方 `pefile` 与"真 DLL 输入"，但 `forward.cpp` 已入库，**构建本身不需要 Python**（`build.cmd:37-41` 只在缺失时报错）。注意：没有"重新生成并与入库版比对"的步骤，若换成另一个版本的 `d3dx9_40.dll` 重新生成，导出层会变而无人察觉（建议 CI/发布脚本加 `gen_forward.py` 重算 + `git diff --exit-code`）。
- **版本号贯穿**：`release.ps1:47-78` 从 `a11y.h` 读四段版本并校验形状、每段 ≤65535，并与 `README.md` 的"当前版本："、`CHANGELOG.md` 首个 `## x.y.z.w` 交叉核对；zip 名 `LucyForAccessibility-<version>.zip` 含版本号（`:206`）；`makezip.ps1` 强制正斜杠条目名并回读校验（`:78-97`）。`[实测]` 当前 zip 名与包内 README 版本均为 0.2.4.0。
- **PE x86 校验**：`release.ps1:140-153` 手工解析 `e_lfanew`、PE 签名、`machine==0x014C`、`OptionalHeader.Magic==0x010B` → 满足清单要求。**不足**：不校验导出表（336 条）、无版本资源、无镜像校验和/时间戳策略（PE `TimeDateStamp` 与 PDB 路径使产物**非字节级可复现**，与 `CHANGELOG.md` 的 G45 说法有差距 → 建议在文档里改成"源码级可重建、字节级不保证"）。
- **发布包组装**：`release.ps1:165-199` 组装 `build\release_stage\<ver>`（dll + README/CHANGELOG/LICENSE/NOTICE + `docs\*.md` + `tools\*.ps1`），禁止 `.exe/.pdb/.obj/.lib/.exp/.ilk/.res/.log/.zip/.7z/.xp3/.sig/.wav/.ogg/.mp3/.png/.jpg/.jpeg` 及除 `d3dx9_40.dll` 外的任何 `.dll`，最后强制走 `makezip.ps1` 并核对必需条目（`:227-230`）→ 结构与内容过滤是合格的，唯一问题是把内部文档一起打包（S9）。

## 未核查项

1. **未做游戏内实测**（按要求不运行游戏）：所有"运行时行为"结论来自源码、既有日志（`a11y\a11y.log` 271 行、`audio_hook.log` 67 行）与安装记录，不是新一轮实机验证。
2. **判据命中率**：`IsStoryDraw`（`fonthook.cpp:93-107`：`fmt&0x410==0x410`、非 `0x100`、`count!=1`、正宽 `<600` 排除）与音频启发式在其它字体/语言/分辨率下的准确率未测。
3. **导出表逐名比对**：只核对了数量（`exports_expected.txt` 336 行、`forward.cpp` 336 个 `__declspec(dllexport)`、日志 `RESOLVE ok=336 missing=0`），未拿 `dumpbin /exports` 与真 DLL 逐名 diff。
4. **非 1280×720 客户区 / 全屏切换 / 多显示器 / DPI 缩放**：未测，`S6` 的风险未量化。
5. **NVDA/争渡/SAPI 三后端**在 0.2.4.0 构建下的实际朗读未复核：既有 `a11y.log` 只有少量后端探测/失败痕迹，没有"三个后端各跑一遍"的记录。
6. **存档落盘/读回**：文档自述"未逐项验证"，本次同样未验证。
7. **`ftell` 在 `"ab"` 流上的行为**（S2）是 `[推断]`，未写最小 CRT 实验确认；`grep inUse` 等静态结论已核对，但"引擎多久会复用对象地址"未验证。
8. **`pefile` 版本未固定**：生成器逻辑确定，但不同 `pefile` 版本对畸形/加壳导出表的解析是否一致未验证（本次输入是本地原版 DLL，`[实测]` 输出与入库 `forward.cpp` 的导出数一致）。
9. **杀软误报**：未对产物做多引擎扫描（README 承认 DLL 注入类补丁常被误报）。
