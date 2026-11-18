#Requires -Version 5.1
<#
  打包脚本：把一个目录打成 zip。

  为什么不用 Compress-Archive：
    Windows PowerShell 5.1 的 Compress-Archive 会把条目名写成反斜杠，
    Linux / macOS 解压时目录层级会坏掉（Framework 打包红线）。
    这里用 System.IO.Compression.ZipArchive，逐条加入，条目名统一正斜杠。

  用法：
    pwsh -File build\makezip.ps1 -Source build\release_stage\0.2.0.0 -Zip dist\LucyForAccessibility-0.2.0.0.zip

  成功退出码 0，失败 1。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Zip
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Die([string]$m) {
    Write-Host "  [x] $m" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
    Die "源目录不存在：$Source"
}
$srcFull = (Resolve-Path -LiteralPath $Source).Path.TrimEnd('\')
$zipFull = $PSCmdlet.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Zip)
$zipDir = [System.IO.Path]::GetDirectoryName($zipFull)
if ([string]::IsNullOrEmpty($zipDir)) { $zipDir = (Get-Location).Path }
if (-not (Test-Path -LiteralPath $zipDir)) {
    New-Item -ItemType Directory -Force -Path $zipDir | Out-Null
}
if (Test-Path -LiteralPath $zipFull) { Remove-Item -LiteralPath $zipFull -Force }

$dirs = @(Get-ChildItem -LiteralPath $srcFull -Recurse -Directory)
$files = @(Get-ChildItem -LiteralPath $srcFull -Recurse -File)
if ($files.Count -eq 0) { Die "源目录里没有文件：$srcFull" }

$fileCount = 0
$fs = [System.IO.File]::Open($zipFull, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
try {
    $archive = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        # 目录条目：名字必须以 / 结尾，便于空目录和解压器识别。
        foreach ($d in $dirs) {
            $rel = $d.FullName.Substring($srcFull.Length + 1).Replace('\', '/') + '/'
            $entry = $archive.CreateEntry($rel, [System.IO.Compression.CompressionLevel]::NoCompression)
            try { $entry.LastWriteTime = $d.LastWriteTime } catch { }
        }
        # 文件条目：条目名统一正斜杠，内容逐条写入。
        foreach ($f in $files) {
            $rel = $f.FullName.Substring($srcFull.Length + 1).Replace('\', '/')
            $entry = $archive.CreateEntry($rel, [System.IO.Compression.CompressionLevel]::Optimal)
            try { $entry.LastWriteTime = $f.LastWriteTime } catch { }
            $entryStream = $entry.Open()
            try {
                $data = [System.IO.File]::ReadAllBytes($f.FullName)
                $entryStream.Write($data, 0, $data.Length)
            } finally {
                $entryStream.Dispose()
            }
            $fileCount++
        }
    } finally {
        $archive.Dispose()
    }
} finally {
    $fs.Dispose()
}

# 回读校验：条目名不得含反斜杠，文件条目数必须与源目录一致。
$zipStream = [System.IO.File]::OpenRead($zipFull)
try {
    $readBack = New-Object System.IO.Compression.ZipArchive($zipStream, [System.IO.Compression.ZipArchiveMode]::Read)
    try {
        $names = @($readBack.Entries | ForEach-Object { $_.FullName })
        $bad = @($names | Where-Object { $_.Contains('\') })
        if ($bad.Count -gt 0) {
            Die "zip 生成后校验失败：条目名含反斜杠（$($bad -join ', ')）。"
        }
        $fileEntries = @($readBack.Entries | Where-Object { -not $_.FullName.EndsWith('/') })
        if ($fileEntries.Count -ne $fileCount) {
            Die "zip 生成后校验失败：文件条目 $($fileEntries.Count) 个，预期 $fileCount 个。"
        }
    } finally {
        $readBack.Dispose()
    }
} finally {
    $zipStream.Dispose()
}

$size = (Get-Item -LiteralPath $zipFull).Length
Write-Host "  [ok] zip: $zipFull" -ForegroundColor Green
Write-Host "       文件 $fileCount 个，目录 $($dirs.Count) 个，$size 字节，条目名全部正斜杠"
exit 0
