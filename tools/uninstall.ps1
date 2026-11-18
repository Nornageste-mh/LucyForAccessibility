#Requires -Version 5.1
<#
  卸载器：把游戏目录还原成安装前的状态（可干跑、带哈希校验）。

  做法：
    1. 从安装记录（%LOCALAPPDATA%\LucyForAccessibility\install.json）找到游戏目录；
       记录丢了就用 -GameDir 指定；
    2. 校验游戏目录里的 d3dx9_40.dll 确实是本补丁装的（哈希一致）才删；
    3. 把 d3dx9_40.orig.dll 改回 d3dx9_40.dll；
    4. 校验还原后的哈希，删除安装记录。

  用法：
    pwsh -File tools\uninstall.ps1 -DryRun          # 只看不动
    pwsh -File tools\uninstall.ps1                  # 按安装记录还原
    pwsh -File tools\uninstall.ps1 -GameDir "D:\...\Lucy" -Force

  幂等：已经还原过再跑一次，会提示「未安装」，退出码 0。
#>
[CmdletBinding()]
param(
    [string]$GameDir = "",
    [switch]$DryRun,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$BackupName = "d3dx9_40.orig.dll"
$TargetName = "d3dx9_40.dll"

function Get-RecordDir {
    $base = $env:LOCALAPPDATA
    if ([string]::IsNullOrEmpty($base)) { $base = Join-Path $env:USERPROFILE "AppData\Local" }
    return (Join-Path $base "LucyForAccessibility")
}
$RecordDir = Get-RecordDir
$RecordPath = Join-Path $RecordDir "install.json"
$LogPath = Join-Path $RecordDir "install.log"

$script:logLines = New-Object System.Collections.Generic.List[string]
function Say([string]$m) {
    $prefix = ""
    if ($DryRun) { $prefix = "[dry] " }
    Write-Host "$prefix$m"
    $script:logLines.Add("$prefix$m")
}
function Warn([string]$m) { Write-Host "[warn] $m" -ForegroundColor Yellow; $script:logLines.Add("[warn] $m") }
function Fail([string]$m) {
    Write-Host "[x] $m" -ForegroundColor Red
    $script:logLines.Add("[x] $m")
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

Write-Host ""
Write-Host "== 露西 无障碍补丁 卸载器" -ForegroundColor Cyan
if ($DryRun) { Write-Host "   干跑模式：只打印步骤，不改任何文件。" -ForegroundColor Yellow }
Write-Host ""

# ---------------------------------------------------------------------------
# 1. 读安装记录
# ---------------------------------------------------------------------------
$record = $null
if (Test-Path -LiteralPath $RecordPath) {
    try {
        $record = Get-Content -LiteralPath $RecordPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        Warn "安装记录解析失败，将按 -GameDir / 手动模式处理：$RecordPath"
        $record = $null
    }
}

if ([string]::IsNullOrEmpty($GameDir)) {
    if ($record -and $record.gameDir) {
        $GameDir = [string]$record.gameDir
    } else {
        # 幂等：没有记录 = 没装过 / 已经还原过，重复运行不算错误。
        Say "没有安装记录，也没有指定 -GameDir：本补丁当前未安装（或已经卸载过），无需还原。"
        Say "如果游戏目录里还留着补丁的 $TargetName，请用 -GameDir 指定游戏目录再运行。"
        exit 0
    }
}
if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) {
    Fail "游戏目录不存在：$GameDir"
}
$GameDirFull = (Resolve-Path -LiteralPath $GameDir).Path.TrimEnd('\')
$target = Join-Path $GameDirFull $TargetName
$backup = Join-Path $GameDirFull $BackupName

$recordPatchHash = $null
$recordOriginalHash = $null
$recordVersion = "unknown"
if ($record) {
    if ($record.patchSha256) { $recordPatchHash = [string]$record.patchSha256 }
    if ($record.originalSha256) { $recordOriginalHash = [string]$record.originalSha256 }
    if ($record.patchVersion) { $recordVersion = [string]$record.patchVersion }
    if ($record.gameDir -and ([string]$record.gameDir).TrimEnd('\') -ne $GameDirFull) {
        Warn "安装记录里的游戏目录与 -GameDir 不一致：$($record.gameDir) / $GameDirFull；按 -GameDir 处理。"
    }
}

Say "游戏目录：$GameDirFull"
Say "补丁版本：$recordVersion"

# ---------------------------------------------------------------------------
# 2. 判断当前状态
# ---------------------------------------------------------------------------
$targetExists = Test-Path -LiteralPath $target -PathType Leaf
$backupExists = Test-Path -LiteralPath $backup -PathType Leaf

if (-not $backupExists) {
    if (-not $targetExists) {
        Say "游戏目录里既没有 $TargetName 也没有 $BackupName：未安装，无需卸载。"
        if ($record) {
            if ($DryRun) { Say "将删除残留的安装记录：$RecordPath" }
            else { Remove-Item -LiteralPath $RecordPath -Force; Say "已删除残留安装记录。" }
        }
        exit 0
    }
    # 没有备份，但目录里有 d3dx9_40.dll：可能是已还原过的原文件，也可能是备份被人删了。
    $targetHash = Get-Sha256 $target
    if ($recordOriginalHash -and $targetHash -eq $recordOriginalHash) {
        Say "游戏目录里的 $TargetName 已经是原文件（与安装记录哈希一致）：已经还原过，无需再卸载。"
        if ($record) {
            if ($DryRun) { Say "将删除残留的安装记录：$RecordPath" }
            else { Remove-Item -LiteralPath $RecordPath -Force; Say "已删除残留安装记录。" }
        }
        exit 0
    }
    if ($recordPatchHash -and $targetHash -eq $recordPatchHash) {
        Fail "补丁还装着，但备份文件 $BackupName 不见了，无法自动还原。`n      请用 Steam「验证游戏文件完整性」找回原文件（会覆盖 $TargetName）。"
    }
    Warn "没有安装记录或备份可以核对，无法判断 $TargetName 是补丁还是原文件；未改动任何文件。"
    Warn "如果游戏打不开，请用 Steam「验证游戏文件完整性」重下原文件。"
    exit 0
}

# 有备份：说明处于「已安装」或「还原到一半」。
$targetHash = $null
if ($targetExists) { $targetHash = Get-Sha256 $target }

if ($targetExists) {
    if ([string]::IsNullOrEmpty($recordPatchHash)) {
        if (-not $Force) {
            Fail "安装记录里没有补丁哈希，无法确认 $TargetName 是本补丁装的。`n      确认要还原请加 -Force（会删除当前 $TargetName）。"
        }
        Warn "没有安装记录哈希，按 -Force 强制还原。"
    } elseif ($targetHash -ne $recordPatchHash) {
        if (-not $Force) {
            Fail "游戏目录里的 $TargetName 不是本补丁安装的（哈希不符），拒绝删除。`n      期望 $recordPatchHash，实际 $targetHash。`n      确认要强制还原请加 -Force。"
        }
        Warn "哈希不符（实际 $targetHash），按 -Force 强制还原。"
    } else {
        Say "哈希校验通过：$targetHash 是本补丁装的。"
    }
}

# ---------------------------------------------------------------------------
# 3. 干跑
# ---------------------------------------------------------------------------
if ($DryRun) {
    Say "将执行："
    if ($targetExists) { Say "  1) 删除补丁文件 $TargetName" }
    else { Say "  1) 补丁文件 $TargetName 已不存在，跳过删除" }
    Say "  2) 改名还原 $BackupName -> $TargetName"
    if ($recordOriginalHash) { Say "  3) 校验还原后的 SHA256（期望 $recordOriginalHash）" }
    Say "  4) 删除安装记录 $RecordPath"
    Write-Host ""
    Write-Host "干跑结束，未改动任何文件。" -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------------------
# 4. 实际还原
# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $RecordDir | Out-Null
try {
    if ($targetExists) {
        Say "删除补丁文件：$target"
        Remove-Item -LiteralPath $target -Force -ErrorAction Stop
    }
    Say "改名还原：$BackupName -> $TargetName"
    Rename-Item -LiteralPath $backup -NewName $TargetName -ErrorAction Stop

    if ($recordOriginalHash) {
        $restoredHash = Get-Sha256 $target
        if ($restoredHash -ne $recordOriginalHash) {
            Warn "还原后的哈希与安装记录不一致：期望 $recordOriginalHash，实际 $restoredHash。"
            Warn "游戏可能已更新过原文件；如游戏打不开，请用 Steam 校验文件完整性。"
        } else {
            Say "哈希校验通过：$restoredHash"
        }
    }

    if (Test-Path -LiteralPath $RecordPath) {
        Remove-Item -LiteralPath $RecordPath -Force
        Say "安装记录已删除：$RecordPath"
    }
    Add-Content -LiteralPath $LogPath -Value $script:logLines -Encoding UTF8
    Write-Host ""
    Write-Host "卸载完成，游戏目录已还原。" -ForegroundColor Green
    exit 0
} catch {
    # 还原失败时尽量保证游戏能启动：备份还在就复制回去。
    if (-not (Test-Path -LiteralPath $target) -and (Test-Path -LiteralPath $backup)) {
        try {
            Copy-Item -LiteralPath $backup -Destination $target -Force -ErrorAction Stop
            Write-Host "[!] 改名失败，已复制备份为 $TargetName；请手动检查游戏目录。" -ForegroundColor Yellow
        } catch { }
    }
    Fail "卸载失败：$($_.Exception.Message)"
}
