#Requires -Version 5.1
<#
  安装器：把补丁的代理 DLL 装进游戏目录（可干跑、可回滚）。

  做法（幂等）：
    1. 游戏目录里必须有 d3dx9_40.dll；
    2. 把原文件改名为 d3dx9_40.orig.dll 备份（已有备份 = 已经装过，直接拒绝）；
    3. 把 dist\d3dx9_40.dll 复制进去；
    4. 校验复制后的哈希，写安装记录（游戏路径 / 版本 / 两个文件的 SHA256）。

  用法：
    pwsh -File tools\install.ps1 -GameDir "D:\...\Lucy" -DryRun   # 只看不动
    pwsh -File tools\install.ps1 -GameDir "D:\...\Lucy"

  注意：本脚本不会、也不应该由仓库维护者对本机游戏目录直接执行；
        游戏目录由玩家自己的机器指定，仓库测试一律只跑 -DryRun。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [string]$SourceDll = "",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BackupName = "d3dx9_40.orig.dll"
$TargetName = "d3dx9_40.dll"

function Get-RecordDir {
    $base = $env:LOCALAPPDATA
    if ([string]::IsNullOrEmpty($base)) { $base = Join-Path $env:USERPROFILE "AppData\Local" }
    $dir = Join-Path $base "LucyForAccessibility"
    return $dir
}
$RecordDir = Get-RecordDir
$RecordPath = Join-Path $RecordDir "install.json"
$LogPath = Join-Path $RecordDir "install.log"

$script:logLines = New-Object System.Collections.Generic.List[string]
function Say([string]$m) {
    $prefix = ""
    if ($DryRun) { $prefix = "[dry] " }
    $line = "$prefix$m"
    Write-Host $line
    $script:logLines.Add($line)
}
function Fail([string]$m) {
    $line = "[x] $m"
    Write-Host $line -ForegroundColor Red
    $script:logLines.Add($line)
    # 失败时也把日志落盘，方便排查（干跑除外）。
    if (-not $DryRun) {
        try {
            New-Item -ItemType Directory -Force -Path $RecordDir | Out-Null
            Add-Content -LiteralPath $LogPath -Value $script:logLines -Encoding UTF8
        } catch { }
    }
    exit 1
}
function Get-Sha256([string]$path) {
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}
function Get-PatchVersion {
    $vh = Join-Path $RepoRoot "src\version.h"
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
# 1. 检查游戏目录与源文件
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) {
    Fail "游戏目录不存在：$GameDir"
}
$GameDirFull = (Resolve-Path -LiteralPath $GameDir).Path.TrimEnd('\')
$target = Join-Path $GameDirFull $TargetName
$backup = Join-Path $GameDirFull $BackupName

if ([string]::IsNullOrEmpty($SourceDll)) {
    $SourceDll = Join-Path $RepoRoot "dist\$TargetName"
}
if (-not (Test-Path -LiteralPath $SourceDll -PathType Leaf)) {
    Fail "找不到补丁文件：$SourceDll`n      请先构建（build\build.cmd）或解压发布包，把 d3dx9_40.dll 放到 dist\ 下。"
}
$SourceDllFull = (Resolve-Path -LiteralPath $SourceDll).Path
if ((Get-Item -LiteralPath $SourceDllFull).Length -le 0) {
    Fail "补丁文件是空的：$SourceDllFull"
}

if (Test-Path -LiteralPath $backup) {
    Fail "这个游戏目录已经装过补丁（存在 $BackupName）。`n      请先运行 tools\uninstall.ps1 还原，再安装。"
}
if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
    Fail "游戏目录里没有 $TargetName：$GameDirFull`n      可能选错了目录，或游戏版本与本补丁不匹配。"
}

$version = Get-PatchVersion
$originalHash = Get-Sha256 $target
$patchHash = Get-Sha256 $SourceDllFull

Say "游戏目录：$GameDirFull"
Say "补丁版本：$version"
Say "原文件  ：$TargetName  SHA256=$originalHash"
Say "补丁文件：$SourceDllFull  SHA256=$patchHash"

# ---------------------------------------------------------------------------
# 2. 干跑：只打印计划
# ---------------------------------------------------------------------------
if ($DryRun) {
    Say "将执行："
    Say "  1) 改名 $TargetName -> $BackupName"
    Say "  2) 复制 $SourceDllFull -> $GameDirFull\$TargetName"
    Say "  3) 校验复制后的 SHA256"
    Say "  4) 写安装记录 $RecordPath"
    Write-Host ""
    Write-Host "干跑结束，未改动任何文件。" -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------------------
# 3. 实际安装（失败自动回滚）
# ---------------------------------------------------------------------------
$backupDone = $false
$copyDone = $false
try {
    New-Item -ItemType Directory -Force -Path $RecordDir | Out-Null
    Say "改名备份：$TargetName -> $BackupName"
    Rename-Item -LiteralPath $target -NewName $BackupName -ErrorAction Stop
    $backupDone = $true

    Say "复制补丁：$SourceDllFull -> $target"
    Copy-Item -LiteralPath $SourceDllFull -Destination $target -Force -ErrorAction Stop
    $copyDone = $true

    $newHash = Get-Sha256 $target
    if ($newHash -ne $patchHash) {
        throw "复制后哈希不符：期望 $patchHash，实际 $newHash"
    }
    Say "哈希校验通过：$newHash"

    $record = [ordered]@{
        tool           = "LucyForAccessibility"
        schema         = 1
        gameDir        = $GameDirFull
        patchVersion   = $version
        installedAt    = (Get-Date).ToString("yyyy-MM-ddTHH:mm:sszzz")
        targetName     = $TargetName
        backupName     = $BackupName
        originalSha256 = $originalHash
        patchSha256    = $patchHash
        sourceDll      = $SourceDllFull
    }
    $tmp = "$RecordPath.tmp"
    $record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $tmp -Encoding UTF8
    Move-Item -LiteralPath $tmp -Destination $RecordPath -Force
    Say "安装记录已写：$RecordPath"

    Add-Content -LiteralPath $LogPath -Value $script:logLines -Encoding UTF8
    Write-Host ""
    Write-Host "安装完成。启动游戏即可；没声音请查看 docs\常见问题.md。" -ForegroundColor Green
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
    Fail "安装失败并已回滚：$($_.Exception.Message)"
}
