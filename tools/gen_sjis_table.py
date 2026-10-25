#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""生成 core/src/bms/sjis_table.hpp（SJIS(CP932) <-> Unicode 紧凑映射表）。

数据源 = tools/cp932_map.tsv（权威映射，来源见该文件头注释）；本脚本只做形态
生成，不重新枚举 codec——Python/.NET 对 NEC 选中区与 IBM 重复区的规范侧选择
不同，从 codec 再枚举会改变读取行为。当前脚本只处理 CP932：lead/trail 范围与
输出路径均硬编码；GBK/Big5/CP949 的泛化随 issue #42，不要直接复用本脚本。

输出形状：
- 双字节正表 kSjisToUcs：按 (lead, trail) 直接索引的 uint16 平铺数组（0 = 无映射），
  O(1) 查表；lead 0x81-0x9F/0xE0-0xFC 共 60 行 × trail 0x40-0xFC（去 0x7F）188 列。
- 反向表 kUcsToSjis：{ucs, sjis} 有序数组按 ucs 升序（同 ucs 多表示取最小 sjis），
  二分查找。表变化会反映在 encoding.cpp；重新生成后需重编译 core。
"""

import re
import sys
from collections import defaultdict
from pathlib import Path

LEADS = list(range(0x81, 0xA0)) + list(range(0xE0, 0xFD))  # 双字节 lead：0x81-0x9F + 0xE0-0xFC
TRAILS = [b for b in range(0x40, 0xFD) if b != 0x7F]  # 双字节 trail

# lead 字节 → 行号：0x81-0x9F 顺排（0-30），0xE0-0xFC 续排（31-59）。
LEAD_ROWS = {b: i for i, b in enumerate(LEADS)}
N_ROWS = len(LEADS)  # 60
N_COLS = len(TRAILS)  # 188
TRAIL_BASE = 0x40
# trail 字节 → 列号：0x40-0x7E 直排，0x80-0xFC 跳过 0x7F 续排（列内不含 0x7F）。
TRAIL_COLS = {b: b - TRAIL_BASE - (1 if b > 0x7F else 0) for b in TRAILS}


def load_map(tsv: Path) -> tuple[list[tuple[int, int, int]], list[int], int, int, int]:
    """读权威映射：返回 (双字节表, 单字节 128 项, 声明双字节数, 声明单字节数, 实际单字节数)。"""
    pairs: list[tuple[int, int, int]] = []
    single: dict[int, int] = {}
    declared_double = -1
    declared_single = -1
    section = None
    for raw in tsv.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            m = re.match(r"#\s*double_byte\s+(\d+)", line)
            if m:
                declared_double = int(m.group(1))
                section = "double"
                continue
            m = re.match(r"#\s*single_byte\s+(\d+)", line)
            if m:
                declared_single = int(m.group(1))
                section = "single"
                continue
            if line.startswith("# double_byte"):
                section = "double"
            elif line.startswith("# single_byte"):
                section = "single"
            continue
        if section == "double":
            lead, trail, ucs = line.split("\t")
            pairs.append((int(lead, 16), int(trail, 16), int(ucs, 16)))
        elif section == "single":
            byte, ucs = line.split("\t")
            single[int(byte, 16)] = int(ucs, 16)
    pairs.sort()
    seen = set()
    for lead, trail, _ in pairs:
        if (lead, trail) in seen:
            raise SystemExit(f"cp932_map.tsv 重复双字节条目: {lead:02X}{trail:02X}")
        seen.add((lead, trail))
    return (pairs, [single.get(b, 0) for b in range(0x80, 0x100)], declared_double,
            declared_single, len(single))


def fmt_u16_rows(values: list[int], per_row: int = 12) -> str:
    lines = []
    for i in range(0, len(values), per_row):
        row = ", ".join(f"0x{v:04X}" for v in values[i : i + per_row])
        lines.append(f"    {row},")
    return "\n".join(lines)


def main() -> int:
    check_only = "--check" in sys.argv[1:]
    tsv = Path(__file__).resolve().parent / "cp932_map.tsv"
    pairs, single, declared_double, declared_single, single_count = load_map(tsv)
    if declared_double >= 0 and len(pairs) != declared_double:
        raise SystemExit(
            f"cp932_map.tsv 双字节条目数异常: 实际 {len(pairs)} != 声明 {declared_double}")
    if declared_single >= 0 and single_count != declared_single:
        raise SystemExit(
            f"cp932_map.tsv 单字节条目数异常: 实际 {single_count} != 声明 {declared_single}")
    if len(single) != 128:
        raise SystemExit(f"cp932_map.tsv 单字节段异常: 生成数组 {len(single)} != 128")

    # 正表平铺：行 = lead（60），列 = trail（188）；0 = 无映射。
    fwd = [0] * (N_ROWS * N_COLS)
    for lead, trail, cp in pairs:
        fwd[LEAD_ROWS[lead] * N_COLS + TRAIL_COLS[trail]] = cp

    # 反表：按 ucs 升序，同 ucs 取最小 sjis（(lead<<8)|trail）。
    rev: dict[int, int] = defaultdict(lambda: 0xFFFF)
    for lead, trail, cp in pairs:
        sjis = (lead << 8) | trail
        rev[cp] = min(rev[cp], sjis)
    rev_sorted = sorted(rev.items())

    out = tsv.parent.parent / "core/src/bms/sjis_table.hpp"
    parts = []
    parts.append("// SPDX-License-Identifier: GPL-3.0-only")
    parts.append("// 本文件由 tools/gen_sjis_table.py 生成（数据源 tools/cp932_map.tsv），请勿手改；")
    parts.append("// 重新生成请运行该脚本。")
    parts.append("#pragma once")
    parts.append("#include <cstddef>")
    parts.append("#include <cstdint>")
    parts.append("")
    parts.append("namespace beatbench::bms {")
    parts.append("")
    parts.append("/// 单字节 0x80-0xFF 的映射；0 = 无映射（索引 = 字节值 - 0x80）。")
    parts.append("inline constexpr std::uint16_t kSjisSingle[128] = {")
    parts.append(fmt_u16_rows(single, 8))
    parts.append("};")
    parts.append("")
    parts.append("// 双字节正表（紧凑直接索引）：行 = lead，列 = trail，值 = UCS（0 = 无映射）。")
    parts.append("// lead 行号：0x81-0x9F 顺排 0-30，0xE0-0xFC 续排 31-59（半角假名 0xA1-0xDF 为单字节）；")
    parts.append("// trail 列号：b - 0x40 - (b > 0x7F ? 1 : 0)，0x7F 不收录（列内跳过）。")
    parts.append(f"inline constexpr std::size_t kSjisLeadRows = {N_ROWS};")
    parts.append(f"inline constexpr std::size_t kSjisTrailCols = {N_COLS};")
    parts.append("inline constexpr std::uint16_t kSjisTrailBase = 0x40;")
    parts.append("inline constexpr std::uint16_t kSjisToUcs[] = {")
    parts.append(fmt_u16_rows(fwd))
    parts.append("};")
    parts.append("")
    parts.append("/// 反向表：按 ucs 升序（同 ucs 多表示取最小 sjis = (lead<<8)|trail），二分查找。")
    parts.append("struct SjisRev {")
    parts.append("    std::uint16_t ucs;")
    parts.append("    std::uint16_t sjis;")
    parts.append("};")
    parts.append("inline constexpr SjisRev kUcsToSjis[] = {")
    for ucs, sjis in rev_sorted:
        parts.append(f"    {{0x{ucs:04X}, 0x{sjis:04X}}},")
    parts.append("};")
    parts.append(
        "inline constexpr std::size_t kUcsToSjisCount = sizeof(kUcsToSjis) / sizeof(kUcsToSjis[0]);"
    )
    parts.append("")
    parts.append("}  // namespace beatbench::bms")
    text = "\n".join(parts) + "\n"
    if check_only:
        current = out.read_text(encoding="utf-8") if out.exists() else ""
        if current != text:
            print(f"out of date: {out}（运行 tools/gen_sjis_table.py 重新生成）")
            return 1
        print(f"checked: {out}")
        return 0
    out.write_text(text, encoding="utf-8")
    print(f"generated: {out} (双字节 {len(pairs)} 项, 单字节 {sum(1 for v in single if v)} 项, "
          f"反表 {len(rev_sorted)} 项)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
