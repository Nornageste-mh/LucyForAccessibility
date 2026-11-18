@echo off
rem ===========================================================================
rem  Build the Lucy a11y proxy DLL (32-bit).
rem
rem  Requires:
rem    VCVARS   full path to vcvars32.bat (Visual Studio 32-bit build env)
rem
rem  Usage:  set VCVARS=...   &   build\build.cmd          release build (default)
rem          set VCVARS=...   &   build\build.cmd dev      dev build (/DA11Y_DEV)
rem
rem  Output: build\out\d3dx9_40.dll
rem
rem  Release (default): A11Y_DEV is NOT defined, so the dev diagnostic channel
rem  is compiled out and the shipped DLL contains no dev-only path (Framework G40).
rem  Dev: defines A11Y_DEV, keeps debug info (/Od /Z7), does not define /DNDEBUG.
rem
rem  NOTE: keep this file ASCII-only and CRLF-terminated - cmd.exe parses it in
rem  the OEM codepage and mis-handles UTF-8 comments and lone LF line endings.
rem ===========================================================================
setlocal
set ROOT=%~dp0..
set OUTDIR=%ROOT%\build\out
if "%OUTDIR%"=="" set OUTDIR=%ROOT%build\out

if "%VCVARS%"=="" (
  echo [x] VCVARS is not set - point it at vcvars32.bat
  exit /b 1
)
if not exist "%VCVARS%" (
  echo [x] VCVARS does not exist: %VCVARS%
  exit /b 1
)
if not exist "%ROOT%\src\a11y.cpp" (
  echo [x] src\a11y.cpp not found - the source is maintained in src\
  exit /b 1
)
if not exist "%ROOT%\src\forward.cpp" (
  echo [x] src\forward.cpp missing - run: python tools\gen_forward.py --dll "^
       "<game>\d3dx9_40_orig.dll" --out src
  exit /b 1
)

call "%VCVARS%" >nul || exit /b 1

rem vcvars32.bat sets VSCMD_ARG_TGT_ARCH=x86. This guard matters: with a 64-bit
rem vcvars the build used to succeed and silently emit an x64 DLL that the 32-bit
rem game cannot load (the only symptom would be "the patch does nothing").
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" (
  echo [x] wrong target arch: VSCMD_ARG_TGT_ARCH="%VSCMD_ARG_TGT_ARCH%" - use vcvars32.bat
  exit /b 1
)

rem Build flavour: release is the default; "build\build.cmd dev" enables A11Y_DEV.
set DEV_BUILD=
if /i "%~1"=="dev" set DEV_BUILD=1

if "%DEV_BUILD%"=="1" (
  set MODE_OPT=/Od /Z7
  set MODE_DEFS=/DA11Y_DEV
) else (
  set MODE_OPT=/O1
  set MODE_DEFS=/DNDEBUG
)

if "%DEV_BUILD%"=="1" echo [i] dev build: A11Y_DEV defined - diagnostic channel enabled

rem /MT links the CRT statically so the game process gets no extra runtime DLL.
set CL=/nologo /EHsc /MT %MODE_OPT% /D_CRT_SECURE_NO_WARNINGS %MODE_DEFS% /DUNICODE /D_UNICODE /D__WIN32__ /utf-8
set INC=/I"%ROOT%\src"
set LOG=%ROOT%\build_log.txt

if not exist "%OUTDIR%" mkdir "%OUTDIR%"
del "%LOG%" 2>nul
del "%OUTDIR%\d3dx9_40.dll" 2>nul
del "%OUTDIR%\*.obj" 2>nul

set SRC=%ROOT%\src

rem Note: no "for" loop here. Inside a parenthesised block cmd.exe expands
rem %OBJS% when the block is parsed, so an appended object list silently loses
rem every entry but the last one and the link fails with missing symbols.
echo [i] compiling a11y.cpp
cl /c %CL% %INC% /Fo"%OUTDIR%\a11y.obj" "%SRC%\a11y.cpp" >>"%LOG%" 2>&1
if errorlevel 1 (
  echo [x] compile failed: a11y.cpp - see %LOG%
  exit /b 1
)

echo [i] compiling fonthook.cpp
cl /c %CL% %INC% /Fo"%OUTDIR%\fonthook.obj" "%SRC%\fonthook.cpp" >>"%LOG%" 2>&1
if errorlevel 1 (
  echo [x] compile failed: fonthook.cpp - see %LOG%
  exit /b 1
)

echo [i] compiling menus.cpp
cl /c %CL% %INC% /Fo"%OUTDIR%\menus.obj" "%SRC%\menus.cpp" >>"%LOG%" 2>&1
if errorlevel 1 (
  echo [x] compile failed: menus.cpp - see %LOG%
  exit /b 1
)

echo [i] compiling proxy.cpp
cl /c %CL% %INC% /Fo"%OUTDIR%\proxy.obj" "%SRC%\proxy.cpp" >>"%LOG%" 2>&1
if errorlevel 1 (
  echo [x] compile failed: proxy.cpp - see %LOG%
  exit /b 1
)

echo [i] compiling forward.cpp
cl /c %CL% %INC% /Fo"%OUTDIR%\forward.obj" "%SRC%\forward.cpp" >>"%LOG%" 2>&1
if errorlevel 1 (
  echo [x] compile failed: forward.cpp - see %LOG%
  exit /b 1
)

echo [i] compiling speech\speech.cpp
cl /c %CL% %INC% /Fo"%OUTDIR%\speech.obj" "%SRC%\speech\speech.cpp" >>"%LOG%" 2>&1
if errorlevel 1 (
  echo [x] compile failed: speech\speech.cpp - see %LOG%
  exit /b 1
)

echo [i] compiling audio\audio_hook.cpp
cl /c %CL% %INC% /Fo"%OUTDIR%\audio_hook.obj" "%SRC%\audio\audio_hook.cpp" >>"%LOG%" 2>&1
if errorlevel 1 (
  echo [x] compile failed: audio\audio_hook.cpp - see %LOG%
  exit /b 1
)

set LINK_DEBUG=
if "%DEV_BUILD%"=="1" set LINK_DEBUG=/DEBUG

link /nologo /DLL /MACHINE:X86 %LINK_DEBUG% /OUT:"%OUTDIR%\d3dx9_40.dll" ^
  "%OUTDIR%\a11y.obj" "%OUTDIR%\fonthook.obj" "%OUTDIR%\menus.obj" "%OUTDIR%\proxy.obj" ^
  "%OUTDIR%\forward.obj" "%OUTDIR%\speech.obj" "%OUTDIR%\audio_hook.obj" ^
  user32.lib gdi32.lib ole32.lib oleaut32.lib winmm.lib >>"%LOG%" 2>&1
if errorlevel 1 (
  echo [x] link failed - see %LOG%
  exit /b 1
)

if not exist "%OUTDIR%\d3dx9_40.dll" (
  echo [x] build reported success but the DLL is missing
  exit /b 1
)

echo BUILD_OK  -^>  %OUTDIR%\d3dx9_40.dll
