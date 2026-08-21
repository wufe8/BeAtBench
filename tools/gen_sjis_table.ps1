# SPDX-License-Identifier: GPL-3.0-only
# 生成 core/src/bms/sjis_table.hpp（SJIS(CP932) <-> Unicode 映射表）。
# 用 .NET 的 Encoding 932 枚举全部有效序列，输出三平台一致的静态表。
# 用法: pwsh -File tools/gen_sjis_table.ps1
# 重新生成后需重编译 core（表变化会反映在 encoding.cpp）。

$ErrorActionPreference = 'Stop'
$enc = [System.Text.Encoding]::GetEncoding(932)

$pairs = [System.Collections.Generic.List[object]]::new()
# 双字节：首字节 0x81-0x9F、0xE0-0xFC；尾字节 0x40-0x7E、0x80-0xFC
# 收录条件：解码为单字符(>0x7F, 非 U+FFFD)，且编码回验 == 原字节序列（双射，保证往返无损）。
for ($lead = 0x81; $lead -le 0xFC; $lead++) {
    if ($lead -ge 0xA0 -and $lead -le 0xDF) { continue }  # 半角假名区为单字节
    for ($trail = 0x40; $trail -le 0xFC; $trail++) {
        if ($trail -eq 0x7F) { continue }
        $bytes = [byte[]]@([byte]$lead, [byte]$trail)
        $s = $enc.GetString($bytes)
        if ($s.Length -eq 1) {
            $cp = [int][char]$s[0]
            if ($cp -gt 0x7F -and $cp -ne 0xFFFD) {
                $back = $enc.GetBytes($s)
                if ($back.Length -eq 2 -and $back[0] -eq $lead -and $back[1] -eq $trail) {
                    $pairs.Add([pscustomobject]@{ Lead = $lead; Trail = $trail; Ucs = $cp })
                }
            }
        }
    }
}

# 单字节 0x80-0xFF（半角假名 0xA1-0xDF 等）；同样要求编码回验一致
# （注意：.NET 对孤立首字节会自动补 0x40 解码，回验可剔除这类伪映射）
$single = [int[]]::new(128)
for ($b = 0x80; $b -le 0xFF; $b++) {
    $s = $enc.GetString([byte[]]@([byte]$b))
    if ($s.Length -eq 1) {
        $cp = [int][char]$s[0]
        if ($cp -gt 0x7F -and $cp -ne 0xFFFD) {
            $back = $enc.GetBytes($s)
            if ($back.Length -eq 1 -and $back[0] -eq $b) { $single[$b - 0x80] = $cp }
        }
    }
}

# 正表：按 (lead, trail) 升序
$sorted = $pairs | Sort-Object Lead, Trail
# 反表：按 ucs 升序（同 ucs 多映射时取首个，即按 (ucs, lead, trail) 排序后逐项输出，
# 查找用 lower_bound 即可拿到稳定表示）
$rev = $pairs | Sort-Object Ucs, Lead, Trail

$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine('// SPDX-License-Identifier: GPL-3.0-only')
[void]$sb.AppendLine('// 本文件由 tools/gen_sjis_table.ps1 生成，请勿手改；重新生成请运行该脚本。')
[void]$sb.AppendLine('#pragma once')
[void]$sb.AppendLine('#include <cstddef>')
[void]$sb.AppendLine('#include <cstdint>')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('namespace beatbench::bms {')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('struct SjisPair {')
[void]$sb.AppendLine('    std::uint16_t lead;')
[void]$sb.AppendLine('    std::uint16_t trail;')
[void]$sb.AppendLine('    std::uint16_t ucs;')
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('/// 单字节 0x80-0xFF 的映射；0 = 无映射（索引 = 字节值 - 0x80）。')
[void]$sb.AppendLine('inline constexpr std::uint16_t kSjisSingle[128] = {')
for ($i = 0; $i -lt 128; $i += 8) {
    $row = ($single[$i..($i + 7)] | ForEach-Object { '0x{0:X4}' -f $_ }) -join ', '
    [void]$sb.AppendLine("    $row,")
}
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('/// 双字节正表：按 (lead, trail) 升序，二分查找。')
[void]$sb.AppendLine('inline constexpr SjisPair kSjisToUcs[] = {')
foreach ($p in $sorted) {
    [void]$sb.AppendLine(('    {{0x{0}, 0x{1}, 0x{2}}},' -f $p.Lead.ToString('X2'), $p.Trail.ToString('X2'), $p.Ucs.ToString('X4')))
}
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('inline constexpr std::size_t kSjisToUcsCount = sizeof(kSjisToUcs) / sizeof(kSjisToUcs[0]);')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('/// 反向表：按 ucs 升序（同 ucs 多个 SJIS 表示时保留首个），二分查找。')
[void]$sb.AppendLine('inline constexpr SjisPair kUcsToSjis[] = {')
foreach ($p in $rev) {
    [void]$sb.AppendLine(('    {{0x{0}, 0x{1}, 0x{2}}},' -f $p.Lead.ToString('X2'), $p.Trail.ToString('X2'), $p.Ucs.ToString('X4')))
}
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('inline constexpr std::size_t kUcsToSjisCount = sizeof(kUcsToSjis) / sizeof(kUcsToSjis[0]);')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('}  // namespace beatbench::bms')

$out = Join-Path $PSScriptRoot '..\core\src\bms\sjis_table.hpp'
$dir = Split-Path $out -Parent
[void](New-Item -ItemType Directory -Force -Path $dir)
[System.IO.File]::WriteAllText($out, $sb.ToString(), [System.Text.UTF8Encoding]::new($false))
Write-Host "generated: $out (双字节 $($sorted.Count) 项, 单字节 $(( $single | Where-Object { $_ -ne 0 } ).Count) 项)"
