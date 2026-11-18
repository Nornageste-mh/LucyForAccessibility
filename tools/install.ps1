#Requires -Version 5.1
<#
  install.ps1 -- 露西 无障碍补丁 安装器（玩家入口，幂等、可干跑、可回滚）

  做什么：
    1) 检查：游戏目录里要有 Lucy.exe + Engine.dll；游戏没从该目录运行；
       d3dx9_40.dll 必须是游戏原版（SHA256 = 16FD909A...E216B）。
    2) 安装：原文件改名为 d3dx9_40_orig.dll 保留；复制补丁 d3dx9_40.dll；
       建 a11y\ 目录；包内若有 nvdaControllerClient.dll 一并复制；
       写 a11y\install.json（版本 / 时间 / 两个 SHA256 / 备份名）。
    3) 幂等：已装过（install.json 或备份存在）只提示，不覆盖备份。

  用法：
    pwsh -File tools\install.ps1 -GameDir "D:\...\Lucy" -DryRun   # 只看不动
    pwsh -File tools\install.ps1 -GameDir "D:\...\Lucy"
    pwsh -File tools\install.ps1 -GameDir "D:\...\Lucy" -SourceDir "D:\...\发布包"

  退出码：0 = 成功或无需动作；1 = 检查失败或出错（会写清原因）。
  不装服务、不写注册表、不需要管理员权限；只动指定游戏目录。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)][string]$GameDir = "",
    [Parameter(Mandatory = $false)][string]$SourceDir = "",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$TargetName = "d3dx9_40.dll"
$BackupName = "d3dx9_40_orig.dll"
$EngineExe = "Lucy.exe"
$EngineDll = "Engine.dll"
# 中文版 Ver 1.14 的 d3dx9_40.dll 原版哈希（实测）。
$OriginalSha256 = "16FD909AEB68D0D1ACA8529DC7F78880B97D6649D70CE8D03A2C858BC28E216B"

if ([string]::IsNullOrEmpty($SourceDir)) {
    # 默认：脚本所在包的上一级（发布包根目录 / 仓库根目录）。
    $SourceDir = Split-Path -Parent $PSScriptRoot
}

$log = New-Object System.Collections.Generic.List[string]
function Say([string]$m) {
    if ($DryRun) { $m = "[dry] $m" }
    Write-Host $m
    $script:log.Add($m)
}
function Warn([string]$m) {
    Write-Host "[warn] $m" -ForegroundColor Yellow
    $script:log.Add("[warn] $m")
}
function Fail([string]$m) {
    Write-Host "[x] $m" -ForegroundColor Red
    $script:log.Add("[x] $m")
    exit 1
}
function Get-Sha256([string]$path) {
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToUpperInvariant()
}
function Get-PatchVersion {
    # 优先读发布包里的 CHANGELOG.md 第一个版本标题；仓库里也认 src\version.h。
    $cl = Join-Path $SourceDir "CHANGELOG.md"
    if (Test-Path -LiteralPath $cl) {
        $t = Get-Content -LiteralPath $cl -Raw -Encoding UTF8
        $m = [regex]::Match($t, '(?m)^##\s+([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)')
        if ($m.Success) { return $m.Groups[1].Value }
    }
    $vh = Join-Path (Split-Path -Parent $PSScriptRoot) "src\version.h"
    if (Test-Path -LiteralPath $vh) {
        $t = Get-Content -LiteralPath $vh -Raw -Encoding UTF8
        $m = [regex]::Match($t, '(?m)^\s*#\s*define\s+A11Y_VERSION\s+"?([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)"?')
        if ($m.Success) { return $m.Groups[1].Value }
    }
    return "unknown"
}

Write-Host ""
Write-Host "== 露西 无障碍补丁 安装器" -ForegroundColor Cyan
if ($DryRun) { Write-Host "   干跑模式：只打印步骤，不改任何文件。" -ForegroundColor Yellow }
Write-Host ""

# ---------------------------------------------------------------------------
# 1. 游戏目录检查
# ---------------------------------------------------------------------------
if ([string]::IsNullOrEmpty($GameDir)) { Fail "请用 -GameDir 指定游戏安装目录。" }
if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) { Fail "游戏目录不存在：$GameDir" }
$GameDirFull = (Resolve-Path -LiteralPath $GameDir).Path.TrimEnd('\')
if (-not (Test-Path -LiteralPath (Join-Path $GameDirFull $EngineExe) -PathType Leaf)) {
    Fail "这个目录里没有 $EngineExe，看起来不是《露西 -她所期望的一切-》的游戏目录：$GameDirFull"
}
if (-not (Test-Path -LiteralPath (Join-Path $GameDirFull $EngineDll) -PathType Leaf)) {
    Fail "这个目录里没有 $EngineDll，游戏文件不完整或目录选错了：$GameDirFull"
}

# ---------------------------------------------------------------------------
# 2. 游戏是否正从该目录运行（只拒绝该目录的进程，别的副本在跑不影响）
# ---------------------------------------------------------------------------
$ourExe = (Join-Path $GameDirFull $EngineExe)
foreach ($p in @(Get-Process -Name ([System.IO.Path]::GetFileNameWithoutExtension($EngineExe)) -ErrorAction SilentlyContinue)) {
    $ppath = $null
    try { $ppath = $p.Path } catch { $ppath = $null }
    if ($ppath -and ($ppath.TrimEnd('\') -ieq $ourExe.TrimEnd('\'))) {
        Fail "游戏正在运行（进程 $($p.Id)）。请先完全退出游戏再安装。"
    }
}

$target = Join-Path $GameDirFull $TargetName
$backup = Join-Path $GameDirFull $BackupName
$a11yDir = Join-Path $GameDirFull "a11y"
$recordPath = Join-Path $a11yDir "install.json"
$installLog = Join-Path $a11yDir "install.log"

# ---------------------------------------------------------------------------
# 3. 补丁源文件检查
# ---------------------------------------------------------------------------
$sourceDll = Join-Path $SourceDir $TargetName
if (-not (Test-Path -LiteralPath $sourceDll -PathType Leaf)) {
    Fail "找不到补丁文件：$sourceDll`n      请先解压发布包，或用 -SourceDir 指向补丁所在目录（里面有 $TargetName）。"
}
$sourceDll = (Resolve-Path -LiteralPath $sourceDll).Path
if ((Get-Item -LiteralPath $sourceDll).Length -le 0) { Fail "补丁文件是空的：$sourceDll" }
$patchHash = Get-Sha256 $sourceDll
if ($patchHash -eq $OriginalSha256) {
    Fail "补丁文件与游戏原版一模一样（$sourceDll），这不是补丁，拒绝安装。"
}

# ---------------------------------------------------------------------------
# 4. 当前安装状态
# ---------------------------------------------------------------------------
$version = Get-PatchVersion
$alreadyInstalled = (Test-Path -LiteralPath $recordPath) -or (Test-Path -LiteralPath $backup)
if ($alreadyInstalled) {
    Say "这个游戏目录已经装过补丁：$GameDirFull"
    Say "  备份文件：$backup"
    Say "  安装记录：$recordPath"
    Say "无需重复安装；要换版本请先运行 tools\uninstall.ps1 还原，再重新安装。"
    exit 0
}
if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
    Fail "游戏目录里没有 $TargetName。`n      游戏文件不完整；请先用 Steam「验证游戏文件完整性」修复。"
}

$currentHash = Get-Sha256 $target
if ($currentHash -ne $OriginalSha256) {
    if ($currentHash -eq $patchHash) {
        Say "这个目录里已经是本补丁的 $TargetName（没有安装记录，可能是手动装过）。"
        Say "无需重复安装；要还原请运行 tools\uninstall.ps1 -GameDir `"$GameDirFull`"。"
        exit 0
    }
    Fail ("游戏目录里的 $TargetName 不是原版文件（SHA256=$currentHash）。`n" +
          "      可能装了别的补丁、或文件已损坏；为避免覆盖，拒绝安装。`n" +
          "      请先用 Steam「验证游戏文件完整性」恢复原文件后再试。")
}

Say "游戏目录：$GameDirFull"
Say "补丁版本：$version"
Say "原文件  ：$TargetName  SHA256=$OriginalSha256（原版已确认）"
Say "补丁文件：$sourceDll  SHA256=$patchHash"

# ---------------------------------------------------------------------------
# 5. 干跑
# ---------------------------------------------------------------------------
$nvdaCandidates = @(
    (Join-Path $SourceDir "nvdaControllerClient.dll"),
    (Join-Path $SourceDir "a11y\nvdaControllerClient.dll"),
    (Join-Path $SourceDir "dll\nvdaControllerClient.dll")
)
$nvdaSource = $null
foreach ($c in $nvdaCandidates) { if (Test-Path -LiteralPath $c -PathType Leaf) { $nvdaSource = $c; break } }

if ($DryRun) {
    Say "将执行："
    Say "  1) 改名 $TargetName -> $BackupName（保留原文件）"
    Say "  2) 复制 $sourceDll -> $GameDirFull\$TargetName"
    Say "  3) 校验复制后的 SHA256"
    Say "  4) 建目录 $a11yDir"
    if ($nvdaSource) { Say "  5) 复制 $nvdaSource -> $a11yDir\nvdaControllerClient.dll" }
    else { Say "  5) 包内没有 nvdaControllerClient.dll，跳过（运行时从本机 NVDA 目录加载）" }
    Say "  6) 写安装记录 $recordPath"
    Write-Host ""
    Write-Host "干跑结束，未改动任何文件。" -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------------------
# 6. 实际安装（失败自动回滚）
# ---------------------------------------------------------------------------
$backupDone = $false
$copyDone = $false
try {
    Say "改名备份：$TargetName -> $BackupName"
    Rename-Item -LiteralPath $target -NewName $BackupName -ErrorAction Stop
    $backupDone = $true

    Say "复制补丁：$sourceDll -> $target"
    Copy-Item -LiteralPath $sourceDll -Destination $target -Force -ErrorAction Stop
    $copyDone = $true

    $newHash = Get-Sha256 $target
    if ($newHash -ne $patchHash) { throw "复制后哈希不符：期望 $patchHash，实际 $newHash" }
    Say "哈希校验通过：$newHash"

    New-Item -ItemType Directory -Force -Path $a11yDir | Out-Null
    Say "已建目录：$a11yDir"

    if ($nvdaSource) {
        Copy-Item -LiteralPath $nvdaSource -Destination (Join-Path $a11yDir "nvdaControllerClient.dll") -Force
        Say "已复制 nvdaControllerClient.dll 到 a11y\"
    } else {
        Say "包内没有 nvdaControllerClient.dll，跳过（运行时从本机 NVDA 目录加载）"
    }

    $record = [ordered]@{
        tool           = "LucyForAccessibility"
        schema         = 1
        version        = $version
        installedAt    = (Get-Date).ToString("yyyy-MM-ddTHH:mm:sszzz")
        gameDir        = $GameDirFull
        originalSha256 = $OriginalSha256
        patchSha256    = $patchHash
        backupName     = $BackupName
        sourceDll      = $sourceDll
    }
    $tmp = "$recordPath.tmp"
    $record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $tmp -Encoding UTF8
    Move-Item -LiteralPath $tmp -Destination $recordPath -Force
    Say "安装记录已写：$recordPath"

    Add-Content -LiteralPath $installLog -Value $script:log -Encoding UTF8
    Write-Host ""
    Write-Host "安装完成。启动游戏即可；听不到声音请看 docs\常见问题.md。" -ForegroundColor Green
    exit 0
} catch {
    Write-Host ""
    Write-Host "[!] 安装失败，正在回滚……" -ForegroundColor Yellow
    if ($copyDone -and (Test-Path -LiteralPath $target)) {
        Remove-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue
    }
    if ($backupDone -and (Test-Path -LiteralPath $backup)) {
        try { Rename-Item -LiteralPath $backup -NewName $TargetName -ErrorAction Stop }
        catch { Write-Host "[!] 回滚改名失败，请手动把 $BackupName 改回 $TargetName" -ForegroundColor Red }
    }
    if (Test-Path -LiteralPath $recordPath) { Remove-Item -LiteralPath $recordPath -Force -ErrorAction SilentlyContinue }
    Fail "安装失败并已回滚：$($_.Exception.Message)"
}
