#Requires -Version 5.1
<#
  提交前守卫：检查暂存区（git index）里不应该进仓库的东西。

  用法：
    pwsh -File build\check-staged.ps1
    通过退出码 0；有违规退出码 1（提交前必须先处理）。

  检查项：
    1. 禁止提交构建产物 / 游戏资源 / 压缩包 / 日志（含扩展名与路径片段）；
    2. 单文件 > 20MB 直接拒绝；
    3. .ps1 含中文必须是 UTF-8 带 BOM（G5）；.cmd 必须纯 ASCII；
    4. 版本号三处一致（src\a11y.h / README / CHANGELOG 顶部）；
    5. P0 文档存在，且没把「导航模式」写进用户可见说明（第 0 条纪律）。
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $Root

function Write-Ok([string]$m)   { Write-Host "  [ok] $m" -ForegroundColor Green }
function Write-Warn([string]$m) { Write-Host "  [warn] $m" -ForegroundColor Yellow }
function Write-Err([string]$m)  { Write-Host "  [x] $m" -ForegroundColor Red }

$problems = New-Object System.Collections.Generic.List[string]
$warnings = New-Object System.Collections.Generic.List[string]

try {
    # -----------------------------------------------------------------------
    # 取暂存区文件列表（-z 避免中文文件名被转义；这里用普通列表即可）
    # -----------------------------------------------------------------------
    $stagedRaw = @(& git -c core.quotepath=false diff --cached --name-only 2>$null)
    if ($LASTEXITCODE -ne 0) {
        Write-Err "不是 git 仓库，或 git 不可用。"
        exit 1
    }
    $staged = @($stagedRaw | Where-Object { $_ -ne "" })
    if ($staged.Count -eq 0) {
        Write-Ok "暂存区为空，无需检查。"
        exit 0
    }
    Write-Host "检查 $($staged.Count) 个暂存文件……"

    $forbiddenPathFragments = @(
        'build/out/',
        'build/release_stage/',
        'tools/_cache/',
        'savedata/',
        'save/'
    )
    $forbiddenExt = @(
        '.obj', '.lib', '.exp', '.ilk', '.pdb', '.res',
        '.dll', '.exe', '.zip', '.7z', '.rar',
        '.log', '.pyc',
        '.xp3', '.sig',
        '.wav', '.ogg', '.mp3', '.wmv',
        '.png', '.jpg', '.jpeg'
    )
    $assetNamePatterns = @(
        '^(build_log|a11y_load|a11y_cmd|a11y_diag)',
        '_dump\.txt$', '_scan[0-9]*\.txt$', '_probe[0-9]*\.txt$'
    )

    foreach ($f in $staged) {
        $rel = $f.Replace('\', '/')
        $leaf = [System.IO.Path]::GetFileName($rel)
        $ext = [System.IO.Path]::GetExtension($rel).ToLowerInvariant()

        # dist/ 只放行说明文件，产物一律不入库。
        if ($rel.StartsWith('dist/') -and $rel -ne 'dist/README.md') {
            $problems.Add("禁止提交 dist 产物：$rel（dist/ 只允许 README.md）")
        }

        foreach ($frag in $forbiddenPathFragments) {
            if ($rel.StartsWith($frag) -or $rel.Contains("/$frag")) {
                $problems.Add("禁止提交构建/缓存目录：$rel")
            }
        }

        if ($forbiddenExt -contains $ext) {
            $problems.Add("禁止提交该类型文件：$rel（$ext）")
        }

        foreach ($p in $assetNamePatterns) {
            if ($leaf -match $p) {
                $problems.Add("疑似探针/日志输出：$rel")
            }
        }

        # 大文件：> 20MB 不许进仓库（AGENTS.md 第 1 节）。
        $size = 0
        try {
            if (Test-Path -LiteralPath $rel) {
                $size = (Get-Item -LiteralPath $rel).Length
            } else {
                $size = [int64](& git cat-file -s ":$rel" 2>$null)
            }
        } catch { $size = 0 }
        if ($size -gt 20MB) {
            $problems.Add(("单文件超过 20MB：{0}（{1:N1} MB）" -f $rel, ($size / 1MB)))
        }

        # .ps1 编码：含中文必须 UTF-8 带 BOM（G5）。
        if ($ext -eq '.ps1' -and (Test-Path -LiteralPath $rel)) {
            $bytes = [System.IO.File]::ReadAllBytes((Join-Path $Root $rel))
            $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
            $hasNonAscii = $false
            $start = 0
            if ($hasBom) { $start = 3 }
            for ($i = $start; $i -lt $bytes.Length; $i++) {
                if ($bytes[$i] -gt 0x7F) { $hasNonAscii = $true; break }
            }
            if ($hasNonAscii -and -not $hasBom) {
                $problems.Add("$rel 含非 ASCII 却没有 UTF-8 BOM（Framework G5：PowerShell 5.1 会按 GBK 误读中文）")
            } elseif ($hasNonAscii -and $hasBom) {
                try {
                    $null = [System.Text.Encoding]::GetEncoding('utf-8', [System.Text.EncoderFallback]::ExceptionFallback, [System.Text.DecoderFallback]::ExceptionFallback).GetString($bytes, 3, $bytes.Length - 3)
                } catch {
                    $problems.Add("$rel 标了 UTF-8 BOM 但字节不是有效 UTF-8")
                }
            } elseif (-not $hasBom) {
                $warnings.Add("$rel 是纯 ASCII、无 BOM（可接受，但如以后加中文必须补 BOM）")
            }
        }

        # .cmd 必须纯 ASCII（cmd.exe 按 OEM 代码页解析）。
        if ($ext -eq '.cmd' -and (Test-Path -LiteralPath $rel)) {
            $bytes = [System.IO.File]::ReadAllBytes((Join-Path $Root $rel))
            for ($i = 0; $i -lt $bytes.Length; $i++) {
                if ($bytes[$i] -gt 0x7F) {
                    $problems.Add("$rel 不是纯 ASCII（cmd.exe 会按 OEM 代码页误读，G5）")
                    break
                }
            }
        }
    }

    # -----------------------------------------------------------------------
    # 版本号一致性（三处：a11y.h / README 当前版本 / CHANGELOG 顶部）
    # -----------------------------------------------------------------------
    $version = $null
    $vh = Join-Path $Root 'src\a11y.h'
    if (Test-Path -LiteralPath $vh) {
        $vhText = Get-Content -LiteralPath $vh -Raw -Encoding UTF8
        $mv = [regex]::Match($vhText, '(?m)^\s*#\s*define\s+A11Y_VERSION\s+L?"?([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)"?')
        if ($mv.Success) { $version = $mv.Groups[1].Value }
        else { $problems.Add("src\a11y.h 存在但读不到四段纯数字 A11Y_VERSION（G7）") }
    } else {
        $warnings.Add("src\a11y.h 尚未提交（骨架阶段允许；发布前必须存在）")
    }
    $readme = Join-Path $Root 'README.md'
    if ((Test-Path -LiteralPath $readme) -and $version) {
        $rm = [regex]::Match((Get-Content -LiteralPath $readme -Raw -Encoding UTF8), '当前版本[：:]\s*\*{0,2}([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)')
        if ($rm.Success -and $rm.Groups[1].Value -ne $version) {
            $problems.Add("版本号不一致：a11y.h=$version，README=$($rm.Groups[1].Value)")
        }
    }
    $changelog = Join-Path $Root 'CHANGELOG.md'
    if ((Test-Path -LiteralPath $changelog) -and $version) {
        $cm = [regex]::Match((Get-Content -LiteralPath $changelog -Raw -Encoding UTF8), '(?m)^##\s+([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)\b')
        if ($cm.Success -and $cm.Groups[1].Value -ne $version) {
            $problems.Add("版本号不一致：a11y.h=$version，CHANGELOG=$($cm.Groups[1].Value)")
        }
    }

    # -----------------------------------------------------------------------
    # 第 0 条纪律：P0 判定文档必须存在；结论未填时只警告（骨架阶段）
    # -----------------------------------------------------------------------
    $judge = Join-Path $Root 'docs\交互形态判定.md'
    if (-not (Test-Path -LiteralPath $judge)) {
        $problems.Add("缺少 P0 文档：docs\交互形态判定.md（第 0 条纪律）")
    } else {
        $jt = Get-Content -LiteralPath $judge -Raw -Encoding UTF8
        $mj = [regex]::Match($jt, '\*\*结论\*\*[：:]\s*(.+)')
        if (-not $mj.Success) {
            $problems.Add("docs\交互形态判定.md 缺少「**结论**：」行")
        } else {
            $val = $mj.Groups[1].Value.Trim()
            if ($val -match '[ABC]') {
                Write-Ok "交互形态结论：$val"
            } elseif ($val -match '待实测') {
                $warnings.Add("交互形态判定仍是「待实测」（P2 前必须填完 A/B/C）")
            } else {
                $problems.Add("docs\交互形态判定.md 结论未填写（应为 A/B/C 或明确写「待实测」）")
            }
        }
    }

    # 用户可见说明里不得把「按 Tab 进入导航模式」当默认操作。
    $userDocs = @('README.md', 'docs\安装说明.md', 'docs\常见问题.md')
    $navPattern = '按\s*Tab[^\n]{0,20}(导航|朗读模式)|(导航模式|朗读模式)[^\n]{0,20}按\s*Tab'
    foreach ($d in $userDocs) {
        $p = Join-Path $Root $d
        if (Test-Path -LiteralPath $p) {
            $text = Get-Content -LiteralPath $p -Raw -Encoding UTF8
            if ($text -match $navPattern) {
                $problems.Add("$d 把「按 Tab 进入导航模式」写成默认操作（违反第 0 条纪律）")
            }
        }
    }

    # -----------------------------------------------------------------------
    # 汇总
    # -----------------------------------------------------------------------
    Write-Host ""
    foreach ($w in $warnings) { Write-Warn $w }
    if ($problems.Count -eq 0) {
        Write-Ok "暂存区检查通过（$($staged.Count) 个文件）。"
        exit 0
    }
    Write-Host ""
    Write-Err "发现 $($problems.Count) 个问题，禁止提交："
    foreach ($p in $problems) { Write-Err "  $p" }
    exit 1
} finally {
    Pop-Location
}
