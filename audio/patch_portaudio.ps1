# SPDX-License-Identifier: GPL-3.0-only
# Patch PortAudio CMakeLists.txt for embedded (FetchContent) builds:
# 1. 子项目模式（CMAKE_SOURCE_DIR != CMAKE_CURRENT_LIST_DIR）时 upstream 跳过
#    PROJECT() → 无 LANGUAGES 声明 → CMake "cannot determine linker language"。
#    修复：在文件首行前插入 ENABLE_LANGUAGE(C)。
# 2. 幂等：已含标记则跳过。不改其它内容。
# 用法：powershell -File patch_portaudio.ps1 <绝对路径 CMakeLists.txt>
param([string]$TargetPath = "")
if ([string]::IsNullOrEmpty($TargetPath)) {
    # 兜底：用当前目录
    $TargetPath = Join-Path (Get-Location) "CMakeLists.txt"
}
$content = [System.IO.File]::ReadAllText($TargetPath)
if ($content -notmatch "BBB embedded patch") {
    $marker = "# BBB embedded patch -- enable C language for embedded builds`r`nENABLE_LANGUAGE(C)`r`n"
    $new = $marker + $content
    [System.IO.File]::WriteAllText($TargetPath, $new, [System.Text.UTF8Encoding]::new($false))
    Write-Output "patched: $TargetPath"
} else {
    Write-Output "already patched: $TargetPath"
}
