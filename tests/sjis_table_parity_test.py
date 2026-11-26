# SPDX-License-Identifier: GPL-3.0-only
"""独立检查紧凑表是否保留 v0.5.0 基线的全部 CP932 映射。

可用 --legacy-header 额外逐项比较：先用 git show 将 d8c6f3b 的
core/src/bms/sjis_table.hpp 导出到构建目录，再将该路径传入。不调用生成器，
不读取 TSV，也不依赖本机 codec 或 CI checkout 的历史深度。
"""

import argparse
import hashlib
import itertools
import re
import struct
from collections.abc import Iterable
from pathlib import Path

# 基线 d8c6f3b 的原始三张表，按下述统一形状序列化为小端 uint16 的 SHA-256。
BASELINE_SHA256 = (
    "e4ae1ba0e09de4a621c84ccd7e806a51c3b1360992a2a3ff44a0663713c75c05",
    "a62d1eb81715c348cd58ff9a0a360e2d85fd2042cf038b8e25120b0c3e1ac2e2",
    "7ed32e2bf445b720fd8449801c97e4567c1473177b7b2f058d14ea6f5e8ba04d",
)


def array_body(header: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\[[^\]]*\]\s*=\s*\{{(.*?)\}};", header, re.DOTALL)
    if match is None:
        raise ValueError(f"找不到 {name}")
    return match[1]


def values(body: str) -> list[int]:
    return [int(token.strip(), 0) for token in body.strip().removesuffix(",").split(",")]


def rows(body: str) -> list[list[int]]:
    pattern = r"\{([^{}]+)\}\s*,"
    if re.sub(pattern, "", body).strip():
        raise ValueError("映射表中含无法解析的条目")
    return [values(row) for row in re.findall(pattern, body)]


def dense(entries: Iterable[tuple[int, int]]) -> list[int]:
    # 双字节正表以完整 16 位字节对为索引，反表以完整 BMP 码点为索引。
    result = [0] * 0x10000
    for index, value in entries:
        if result[index] or not value:
            raise ValueError(f"重复或空映射: {index:04X}")
        result[index] = value
    return result


def normalize(path: Path, legacy: bool) -> tuple[list[int], list[int], list[int]]:
    header = path.read_text(encoding="utf-8")
    single = values(array_body(header, "kSjisSingle"))
    forward = array_body(header, "kSjisToUcs")
    reverse = array_body(header, "kUcsToSjis")
    if len(single) != 0x80:
        raise ValueError("单字节表尺寸不符")
    if legacy:
        fwd = dense(((lead << 8) | trail, cp) for lead, trail, cp in rows(forward))
        rev = {}
        for lead, trail, cp in rows(reverse):
            rev.setdefault(cp, (lead << 8) | trail)  # 旧版二分查找取首个。
        return single, fwd, dense(rev.items())

    # 独立枚举合法字节对，不调用生成器的行列计算；未收录的项统一为零。
    leads = [b for b in range(256) if 0x81 <= b <= 0x9F or 0xE0 <= b <= 0xFC]
    trails = [b for b in range(256) if 0x40 <= b <= 0xFC and b != 0x7F]
    byte_pairs = list(itertools.product(leads, trails))
    flat = values(forward)
    if len(flat) != len(byte_pairs):
        raise ValueError("紧凑正表尺寸不符")
    fwd = dense(((lead << 8) | trail, cp) for (lead, trail), cp in zip(byte_pairs, flat) if cp)
    rev = rows(reverse)
    if any(a[0] >= b[0] for a, b in zip(rev, rev[1:])):
        raise ValueError("反表未按 Unicode 码点严格升序排列")
    return single, fwd, dense(rev)


def fingerprint(table: list[int]) -> str:
    return hashlib.sha256(struct.pack(f"<{len(table)}H", *table)).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--legacy-header", type=Path)
    args = parser.parse_args()
    header = Path(__file__).resolve().parents[1] / "core/src/bms/sjis_table.hpp"
    actual = normalize(header, legacy=False)
    legacy = normalize(args.legacy_header, legacy=True) if args.legacy_header else None
    for i, (name, table, expected) in enumerate(zip(
        ("单字节表", "双字节正表", "双字节反表"), actual, BASELINE_SHA256
    )):
        actual_hash = fingerprint(table)
        if legacy is not None and table != legacy[i]:
            first = next(i for i, (a, b) in enumerate(zip(table, legacy[i])) if a != b)
            raise SystemExit(f"{name}在索引 {first:04X} 与原表不同")
        if actual_hash != expected:
            raise SystemExit(f"{name}与 v0.5.0 基线指纹不同: {actual_hash}")
        print(f"{name}：{sum(bool(v) for v in table)}/{len(table)} 与 v0.5.0 基线一致")


if __name__ == "__main__":
    main()
