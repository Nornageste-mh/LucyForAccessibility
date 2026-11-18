#Requires -Version 5.1
<#
  发布脚本：构建 32 位代理 DLL，过三道发布前守卫，组装发布目录并打包。

  用法（在仓库根目录）：
    $env:VCVARS = "C:\...\vcvars32.bat"
    pwsh -File build\release.ps1

  可选：
    -VersionFile <path>   默认 src\a11y.h
    -SkipBuild            复用 build\out\d3dx9_40.dll（仍会跑守卫）
    -SkipPackage          只构建与守卫，不组装 dist 与 zip

  三道守卫（Framework G7 / G40 / G44）：
    1. 版本号必须是四段纯数字，且与 README / CHANGELOG 一致；
    2. 产物不得含调试通道（A11Y_DEV / D:\a11y 等标记）；
    3. 产物必须是 PE32 / machine=0x014C（x86）。

  退出码：0 成功；1 失败（任何守卫命中）。
#>
[CmdletBinding()]
param(
    [string]$VersionFile = "",
    [switch]$SkipBuild,
    [switch]$SkipPackage
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $Root

function Write-Step([string]$m) { Write-Host ""; Write-Host "== $m" -ForegroundColor Cyan }
function Write-Ok([string]$m)   { Write-Host "  [ok] $m" -ForegroundColor Green }
function Write-Warn([string]$m) { Write-Host "  [warn] $m" -ForegroundColor Yellow }
function Write-Err([string]$m)  { Write-Host "  [x] $m" -ForegroundColor Red }
function Die([string]$m) { Write-Err $m; Write-Host ""; Write-Host "发布中止（未生成发布包）。" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------------------
# 步骤 1：从 src\a11y.h 读取 A11Y_VERSION（四段纯数字）
# ---------------------------------------------------------------------------
Write-Step "读取版本号"
if ([string]::IsNullOrEmpty($VersionFile)) { $VersionFile = Join-Path $Root "src\a11y.h" }
if (-not (Test-Path -LiteralPath $VersionFile)) {
    Die "找不到版本号约定文件：$VersionFile（应由 src/a11y.h 提供 A11Y_VERSION，例如 #define A11Y_VERSION L`"0.2.0.0`"）"
}
$vhText = Get-Content -LiteralPath $VersionFile -Raw -Encoding UTF8
$lineMatch = [regex]::Match($vhText, '(?m)^\s*#\s*define\s+A11Y_VERSION\s+L?"?([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)"?')
if (-not $lineMatch.Success) {
    Die "版本号文件里没有 #define A11Y_VERSION：$VersionFile"
}
$rawVersion = $lineMatch.Groups[1].Value.Trim()
if ($rawVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$') {
    Die "版本号形状不合法：'$rawVersion'。必须是四段纯数字（如 0.2.0.0），不能有字母或 v 前缀（Framework G7）。"
}
$parts = $rawVersion.Split('.')
foreach ($p in $parts) {
    if ([int]$p -gt 65535) { Die "版本号某一段超过 65535：'$rawVersion'（Windows 版本资源上限）。" }
}
$Version = $rawVersion
Write-Ok "版本号 $Version（来源 $VersionFile）"

# 与 README / CHANGELOG 交叉核对：三处不一致就不许发布。
$readmePath = Join-Path $Root "README.md"
if (Test-Path -LiteralPath $readmePath) {
    $readme = Get-Content -LiteralPath $readmePath -Raw -Encoding UTF8
    $mReadme = [regex]::Match($readme, '当前版本[：:]\s*\*{0,2}([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)')
    if ($mReadme.Success -and $mReadme.Groups[1].Value -ne $Version) {
        Die "版本号不一致：a11y.h=$Version，README=$($mReadme.Groups[1].Value)。先同步再发布。"
    }
}
$changelogPath = Join-Path $Root "CHANGELOG.md"
if (Test-Path -LiteralPath $changelogPath) {
    $changelog = Get-Content -LiteralPath $changelogPath -Raw -Encoding UTF8
    $mChange = [regex]::Match($changelog, '(?m)^##\s+([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)\b')
    if ($mChange.Success -and $mChange.Groups[1].Value -ne $Version) {
        Die "版本号不一致：a11y.h=$Version，CHANGELOG=$($mChange.Groups[1].Value)。先同步再发布。"
    }
}

# ---------------------------------------------------------------------------
# 步骤 2：构建
# ---------------------------------------------------------------------------
Write-Step "构建 32 位代理 DLL"
$dll = Join-Path $Root "build\out\d3dx9_40.dll"
if ($SkipBuild) {
    Write-Warn "已跳过构建（-SkipBuild），复用现有产物"
} else {
    if ([string]::IsNullOrEmpty($env:VCVARS)) {
        Die "环境变量 VCVARS 未设置。请先设为 vcvars32.bat 的完整路径，例如：`n  `$env:VCVARS = 'C:\Program Files\Microsoft Visual Studio\...\vcvars32.bat'"
    }
    $buildCmd = Join-Path $Root "build\build.cmd"
    & cmd.exe /c "`"$buildCmd`""
    if ($LASTEXITCODE -ne 0) { Die "构建失败（build.cmd 退出码 $LASTEXITCODE），见 build_log.txt。" }
}
if (-not (Test-Path -LiteralPath $dll)) { Die "构建产物不存在：$dll" }
$dllInfo = Get-Item -LiteralPath $dll
if ($dllInfo.Length -le 0) { Die "构建产物是空文件：$dll" }
Write-Ok "产物 $dll（$($dllInfo.Length) 字节）"

$bytes = [System.IO.File]::ReadAllBytes($dll)
$ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
$wide = [System.Text.Encoding]::Unicode.GetString($bytes, 0, $bytes.Length - ($bytes.Length % 2))

# ---------------------------------------------------------------------------
# 守卫 1：版本号形状（已在步骤 1 校验，这里只做最终确认）
# ---------------------------------------------------------------------------
Write-Step "守卫 1/3：版本号形状"
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$') {
    Die "版本号形状不合法：$Version"
}
Write-Ok "四段纯数字：$Version"

# ---------------------------------------------------------------------------
# 守卫 2：调试通道不得进入发布产物（Framework G40）
# ---------------------------------------------------------------------------
Write-Step "守卫 2/3：检查调试通道"
# 这些字符串只在 A11Y_DEV 构建里出现：诊断日志路径、REPL 命令文件、开发标记。
$devMarkers = @(
    'D:\a11y',
    '\\.\pipe\a11y',
    'A11Y_DEV',
    'a11y_dev',
    'a11y_cmd',
    'a11y_load',
    'a11y_diag'
)
$hit = @()
foreach ($mk in $devMarkers) {
    if ($ascii.Contains($mk) -or $wide.Contains($mk)) { $hit += $mk }
}
if ($hit.Count -gt 0) {
    Die "发布守卫失败：产物含调试通道标记（$($hit -join ', ')）。请用 release 模式重新构建（不要带 dev 参数）。"
}
Write-Ok "未发现调试通道标记"

# ---------------------------------------------------------------------------
# 守卫 3：必须是 32 位 PE（Framework G44）
# ---------------------------------------------------------------------------
Write-Step "守卫 3/3：PE 架构"
if ($bytes.Length -lt 0x40) { Die "产物太小，不是有效 PE：$($bytes.Length) 字节" }
$eLfanew = [System.BitConverter]::ToInt32($bytes, 0x3C)
if ($eLfanew -le 0 -or ($eLfanew + 24) -ge $bytes.Length) { Die "PE 头偏移非法：0x$($eLfanew.ToString('X'))" }
$peSig = [System.Text.Encoding]::ASCII.GetString($bytes, $eLfanew, 4)
if ($peSig -ne "PE`0`0") { Die "PE 签名不对（读到 '$peSig'），产物不是有效 DLL。" }
$machine = [System.BitConverter]::ToUInt16($bytes, $eLfanew + 4)
$optMagic = [System.BitConverter]::ToUInt16($bytes, $eLfanew + 24)
if ($machine -ne 0x014C) {
    Die ("发布守卫失败：PE machine=0x{0:X4}，不是 x86（0x014C）。32 位游戏会静默加载失败。" -f $machine)
}
if ($optMagic -ne 0x010B) {
    Die ("发布守卫失败：OptionalHeader magic=0x{0:X4}，不是 PE32（0x010B）。" -f $optMagic)
}
Write-Ok ("PE32 / machine=0x014C（x86）")

if ($SkipPackage) {
    Write-Host ""
    Write-Host "构建与三道守卫通过（-SkipPackage，未打包）。" -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------------------
# 组装发布目录
# ---------------------------------------------------------------------------
Write-Step "组装发布目录"
$stage = Join-Path $Root "build\release_stage\$Version"
$dist = Join-Path $Root "dist"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage "docs") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage "tools") | Out-Null
New-Item -ItemType Directory -Force -Path $dist | Out-Null

Copy-Item -LiteralPath $dll -Destination (Join-Path $stage "d3dx9_40.dll") -Force
foreach ($f in @("README.md", "CHANGELOG.md", "LICENSE", "NOTICE.md")) {
    $src = Join-Path $Root $f
    if (-not (Test-Path -LiteralPath $src)) { Die "发布必需文件缺失：$f" }
    Copy-Item -LiteralPath $src -Destination (Join-Path $stage $f) -Force
}
$docDir = Join-Path $Root "docs"
if (Test-Path -LiteralPath $docDir) {
    Copy-Item -Path (Join-Path $docDir "*.md") -Destination (Join-Path $stage "docs") -Force
}
foreach ($t in @("install.ps1", "uninstall.ps1")) {
    $src = Join-Path $Root "tools\$t"
    if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination (Join-Path $stage "tools\$t") -Force }
}

# 发布目录里绝不允许出现游戏文件 / 构建中间产物 / 其他 DLL。
$forbiddenExt = @('.exe', '.pdb', '.obj', '.lib', '.exp', '.ilk', '.res', '.log',
                  '.zip', '.7z', '.xp3', '.sig', '.wav', '.ogg', '.mp3', '.png', '.jpg', '.jpeg')
$forbidden = @()
Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
    $ext = $_.Extension.ToLowerInvariant()
    if ($forbiddenExt -contains $ext) { $forbidden += $_.FullName }
    if ($ext -eq '.dll' -and $_.Name -ne 'd3dx9_40.dll') { $forbidden += $_.FullName }
}
if ($forbidden.Count -gt 0) {
    Die "发布目录含禁止文件：`n  $($forbidden -join "`n  ")"
}
Write-Ok "发布目录就绪：$stage"

# ---------------------------------------------------------------------------
# 打包（必须走 makezip.ps1，禁止 Compress-Archive）
# ---------------------------------------------------------------------------
Write-Step "打包 zip"
$zip = Join-Path $dist "LucyForAccessibility-$Version.zip"
$makezip = Join-Path $PSScriptRoot "makezip.ps1"
if (-not (Test-Path -LiteralPath $makezip)) { Die "缺少打包脚本：$makezip" }
& $makezip -Source $stage -Zip $zip
if ($LASTEXITCODE -ne 0) { Die "打包失败（makezip.ps1 退出码 $LASTEXITCODE）。" }
if (-not (Test-Path -LiteralPath $zip)) { Die "打包脚本没有生成 zip：$zip" }

# 复制一份到 dist，方便直接分发（zip 内也有一份）。
Copy-Item -LiteralPath $dll -Destination (Join-Path $dist "d3dx9_40.dll") -Force

# 校验 zip：条目名必须是正斜杠，且必需文件齐全。
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipFull = [System.IO.Path]::GetFullPath($zip)
$zipStream = [System.IO.File]::OpenRead($zipFull)
try {
    $archive = New-Object System.IO.Compression.ZipArchive($zipStream, [System.IO.Compression.ZipArchiveMode]::Read)
    try {
        $names = @($archive.Entries | ForEach-Object { $_.FullName })
        $bad = @($names | Where-Object { $_.Contains('\') })
        if ($bad.Count -gt 0) { Die "zip 条目名含反斜杠（Linux/macOS 解压会坏）：$($bad -join ', ')" }
        foreach ($need in @('d3dx9_40.dll', 'README.md', 'LICENSE', 'NOTICE.md', 'CHANGELOG.md',
                            'docs/操作约定.md', 'docs/安装说明.md', 'docs/常见问题.md')) {
            if (-not ($names -contains $need)) { Die "zip 缺少必需条目：$need" }
        }
        Write-Ok "zip 条目 $($names.Count) 个，全部为正斜杠，交叉核对通过"
    } finally { $archive.Dispose() }
} finally { $zipStream.Dispose() }

Write-Ok "发布包 $zip（$((Get-Item -LiteralPath $zip).Length) 字节）"

Write-Host ""
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host " 发布前人工清单" -ForegroundColor Cyan
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host "  [ ] 上传 zip 到 Release，附注本版本变更要点"
Write-Host "  [ ] 干净游戏目录安装测试（原版 dll 改名 → 复制 → 启动 → 卸载还原）"
Write-Host "  [ ] 按测试问卷做 P4 实机验收，结果回填 docs\project_status.md"
Write-Host "  [ ] 核对 README / NOTICE / 安装说明里的键位与覆盖范围说法一致"
Write-Host "  [ ] 确认 NOTICE 中 nvdaControllerClient 的再分发条款仍标注「待确认」"
Write-Host ""
Write-Host "发布包已生成：$zip" -ForegroundColor Green
exit 0
