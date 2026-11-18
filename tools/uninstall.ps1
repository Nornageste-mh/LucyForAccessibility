#Requires -Version 5.1
<#
  uninstall.ps1 -- 露西 无障碍补丁 卸载器（可干跑、带哈希校验）

  做什么：
    1) 检查游戏目录、游戏没在运行、备份 d3dx9_40_orig.dll 还在；
    2) 用 a11y\install.json 里的 patchSha256 确认当前 d3dx9_40.dll 是本补丁
       （不一致要 -Force 才继续）；
    3) 删除补丁 DLL，把 d3dx9_40_orig.dll 改名回 d3dx9_40.dll；
    4) 校验还原后的 SHA256 = 原版哈希；
    5) 删除 a11y\install.json，日志默认保留（-PurgeLogs 才删掉 a11y\）。

  用法：
    pwsh -File tools\uninstall.ps1 -GameDir "D:\...\Lucy" -DryRun
    pwsh -File tools\uninstall.ps1 -GameDir "D:\...\Lucy"
    pwsh -File tools\uninstall.ps1 -GameDir "D:\...\Lucy" -Force
    pwsh -File tools\uninstall.ps1 -GameDir "D:\...\Lucy" -PurgeLogs

  退出码：0 = 已还原或本来就无需还原；1 = 无法安全还原（会写清原因与手动办法）。
  不装服务、不写注册表、不需要管理员权限；只动指定游戏目录。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)][string]$GameDir = "",
    [switch]$DryRun,
    [switch]$Force,
    [switch]$PurgeLogs
)

$ErrorActionPreference = "Stop"

$TargetName = "d3dx9_40.dll"
$BackupName = "d3dx9_40_orig.dll"
$EngineExe = "Lucy.exe"
$EngineDll = "Engine.dll"
$OriginalSha256 = "16FD909AEB68D0D1ACA8529DC7F78880B97D6649D70CE8D03A2C858BC28E216B"

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

Write-Host ""
Write-Host "== 露西 无障碍补丁 卸载器" -ForegroundColor Cyan
if ($DryRun) { Write-Host "   干跑模式：只打印步骤，不改任何文件。" -ForegroundColor Yellow }
Write-Host ""

# ---------------------------------------------------------------------------
# 1. 游戏目录检查
# ---------------------------------------------------------------------------
if ([string]::IsNullOrEmpty($GameDir)) {
    Fail ("请用 -GameDir 指定游戏安装目录（安装记录就在游戏目录的 a11y\install.json 里，`n" +
          "      没有全局记录可查）。例如：`n" +
          "      pwsh -File tools\uninstall.ps1 -GameDir `"D:\...\Lucy`"")
}
if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) { Fail "游戏目录不存在：$GameDir" }
$GameDirFull = (Resolve-Path -LiteralPath $GameDir).Path.TrimEnd('\')
if (-not (Test-Path -LiteralPath (Join-Path $GameDirFull $EngineExe) -PathType Leaf)) {
    Fail "这个目录里没有 $EngineExe，看起来不是《露西 -她所期望的一切-》的游戏目录：$GameDirFull"
}

$target = Join-Path $GameDirFull $TargetName
$backup = Join-Path $GameDirFull $BackupName
$a11yDir = Join-Path $GameDirFull "a11y"
$recordPath = Join-Path $a11yDir "install.json"
$installLog = Join-Path $a11yDir "install.log"

# ---------------------------------------------------------------------------
# 2. 游戏是否正从该目录运行
# ---------------------------------------------------------------------------
$ourExe = (Join-Path $GameDirFull $EngineExe)
foreach ($p in @(Get-Process -Name ([System.IO.Path]::GetFileNameWithoutExtension($EngineExe)) -ErrorAction SilentlyContinue)) {
    $ppath = $null
    try { $ppath = $p.Path } catch { $ppath = $null }
    if ($ppath -and ($ppath.TrimEnd('\') -ieq $ourExe.TrimEnd('\'))) {
        Fail "游戏正在运行（进程 $($p.Id)）。请先完全退出游戏再卸载。"
    }
}

# ---------------------------------------------------------------------------
# 3. 读安装记录
# ---------------------------------------------------------------------------
$record = $null
if (Test-Path -LiteralPath $recordPath) {
    try { $record = Get-Content -LiteralPath $recordPath -Raw -Encoding UTF8 | ConvertFrom-Json }
    catch { Warn "安装记录解析失败，将按备份文件还原（需要 -Force 确认）：$recordPath" }
}
$recordPatchHash = $null
$recordOriginalHash = $null
$recordVersion = "unknown"
if ($record) {
    if ($record.patchSha256) { $recordPatchHash = ([string]$record.patchSha256).ToUpperInvariant() }
    if ($record.originalSha256) { $recordOriginalHash = ([string]$record.originalSha256).ToUpperInvariant() }
    if ($record.version) { $recordVersion = [string]$record.version }
    if ($record.gameDir -and ([string]$record.gameDir).TrimEnd('\') -ine $GameDirFull) {
        Warn "安装记录里的游戏目录与 -GameDir 不一致：$($record.gameDir) / $GameDirFull；按 -GameDir 处理。"
    }
}
if (-not $recordOriginalHash) { $recordOriginalHash = $OriginalSha256 }

Say "游戏目录：$GameDirFull"
Say "补丁版本：$recordVersion"

$targetExists = Test-Path -LiteralPath $target -PathType Leaf
$backupExists = Test-Path -LiteralPath $backup -PathType Leaf

# ---------------------------------------------------------------------------
# 4. 已经还原过 / 无法还原
# ---------------------------------------------------------------------------
if (-not $backupExists) {
    if ($targetExists -and ((Get-Sha256 $target) -eq $recordOriginalHash)) {
        Say "$TargetName 已经是原版文件：本补丁未安装（或已经卸载过），无需还原。"
        if (Test-Path -LiteralPath $recordPath) {
            if (-not $DryRun) { Remove-Item -LiteralPath $recordPath -Force; Say "已删除残留的安装记录。" }
            else { Say "将删除残留的安装记录：$recordPath" }
        }
        exit 0
    }
    Fail ("找不到备份文件 $BackupName，无法自动还原（游戏目录：$GameDirFull）。`n" +
          "      手动恢复办法：`n" +
          "        1) 打开 Steam 库 -> 右键游戏 -> 属性 -> 已安装文件 -> 验证游戏文件完整性；`n" +
          "        2) Steam 会重新下载原版 $TargetName 覆盖当前文件；`n" +
          "        3) 完成后可删除 a11y\ 目录。`n" +
          "      如果确认当前 $TargetName 就是原版（例如手动还原过），本脚本不会改动文件。")
}

# ---------------------------------------------------------------------------
# 5. 校验当前 DLL 确实是本补丁
# ---------------------------------------------------------------------------
$targetHash = $null
if ($targetExists) {
    $targetHash = Get-Sha256 $target
    if ([string]::IsNullOrEmpty($recordPatchHash)) {
        if (-not $Force) {
            Fail ("没有安装记录里的补丁哈希，无法确认 $TargetName 是本补丁装的。`n" +
                  "      确认要强制还原请加 -Force（会删除当前 $TargetName）。")
        }
        Warn "没有安装记录哈希，按 -Force 强制还原。"
    } elseif ($targetHash -ne $recordPatchHash) {
        if (-not $Force) {
            Fail ("游戏目录里的 $TargetName 不是本补丁安装的（哈希不符），拒绝删除。`n" +
                  "      期望 $recordPatchHash`n" +
                  "      实际 $targetHash`n" +
                  "      确认要强制还原请加 -Force。")
        }
        Warn "哈希不符（实际 $targetHash），按 -Force 强制还原。"
    } else {
        Say "哈希校验通过：$targetHash 是本补丁装的。"
    }
}

# ---------------------------------------------------------------------------
# 6. 干跑
# ---------------------------------------------------------------------------
if ($DryRun) {
    Say "将执行："
    if ($targetExists) { Say "  1) 删除补丁文件 $TargetName" }
    else { Say "  1) $TargetName 不存在，跳过删除" }
    Say "  2) 改名还原 $BackupName -> $TargetName"
    Say "  3) 校验还原后的 SHA256（期望 $recordOriginalHash）"
    Say "  4) 删除安装记录 $recordPath"
    Say "  5) 删除 a11y\nvdaControllerClient.dll（补丁组件）"
    if ($PurgeLogs) { Say "  6) 删除整个 a11y\ 目录（-PurgeLogs）" }
    else { Say "  6) 保留 a11y\ 里的日志（a11y.log / install.log）" }
    Write-Host ""
    Write-Host "干跑结束，未改动任何文件。" -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------------------
# 7. 实际还原
# ---------------------------------------------------------------------------
try {
    if ($targetExists) {
        Say "删除补丁文件：$target"
        Remove-Item -LiteralPath $target -Force -ErrorAction Stop
    }
    Say "改名还原：$BackupName -> $TargetName"
    Rename-Item -LiteralPath $backup -NewName $TargetName -ErrorAction Stop

    $restoredHash = Get-Sha256 $target
    if ($restoredHash -ne $recordOriginalHash) {
        Warn "还原后的哈希与记录不一致：期望 $recordOriginalHash，实际 $restoredHash。"
        Warn "游戏可能已更新过原文件；如游戏打不开，请用 Steam 校验文件完整性。"
    } else {
        Say "哈希校验通过：$restoredHash"
    }

    if (Test-Path -LiteralPath $recordPath) {
        Remove-Item -LiteralPath $recordPath -Force
        Say "安装记录已删除。"
    }
    $nvdaCopied = Join-Path $a11yDir "nvdaControllerClient.dll"
    if (Test-Path -LiteralPath $nvdaCopied) {
        Remove-Item -LiteralPath $nvdaCopied -Force -ErrorAction SilentlyContinue
        Say "已删除 a11y\nvdaControllerClient.dll（补丁组件）。"
    }
    if ($PurgeLogs) {
        if (Test-Path -LiteralPath $a11yDir) {
            Remove-Item -LiteralPath $a11yDir -Recurse -Force -ErrorAction SilentlyContinue
            Say "已删除 a11y\ 目录（-PurgeLogs）。"
        }
    } else {
        try { Add-Content -LiteralPath $installLog -Value $script:log -Encoding UTF8 } catch { }
        Say "日志与配置保留在：$a11yDir"
    }
    Write-Host ""
    Write-Host "卸载完成，游戏目录已还原。" -ForegroundColor Green
    exit 0
} catch {
    if (-not (Test-Path -LiteralPath $target) -and (Test-Path -LiteralPath $backup)) {
        try {
            Copy-Item -LiteralPath $backup -Destination $target -Force -ErrorAction Stop
            Write-Host "[!] 改名失败，已复制备份为 $TargetName；请手动检查游戏目录。" -ForegroundColor Yellow
        } catch { }
    }
    Fail "卸载失败：$($_.Exception.Message)"
}
