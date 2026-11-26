#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Qt 符号面下限校验：这个产物到底能跑在哪些 Qt 上？

原理（为什么需要它）
------------------
产物对 Qt 的依赖分两类：

1. **符号版本**（VERNEED 里的 `Qt_6.12` 之类，来自 QtCore 的 qt_version_tag 哨兵）——
   顶层 CMakeLists 的 `QT_NO_VERSION_TAGGING` 已经消掉，`scripts/package-linux.sh` 的
   `assert_portable_qt_links` 也在打包时机械拦这一类的回退；
2. **普通库导出符号**——由**构建时的 Qt 头文件**决定，`assert_portable_qt_links` **看不见**。
   实例（2026-10）：Qt 6.12.0 新增了一批**库导出**的 `QVariant` 右值构造函数；用 6.12 头文件
   编译时重载决议改选这些新重载，产物里就出现对 `_ZN8QVariantC1EO7QString` 等符号的引用，
   在仍是 6.11.2 的系统上启动即 `symbol lookup error`（v0.3.2 / v0.4.0 的 arch 包即此）。

本脚本就查第 2 类：把产物里**所有泛化 `Qt_6` 的未定义符号**，逐一拿到目标 Qt 版本的
libQt6 里找导出（沿产物自身的 `DT_NEEDED` 闭合，避免"库里有但产物根本不加载"的假通过）。
结论是一个数字：**该产物最低能在哪个 Qt 上启动**——它等于产物的**构建面**。

用法
----
    # ① 单个目标 Qt（该 Qt 的 libQt6*.so* 放一个目录里：容器拷出 / 发行版包解开都行）
    python3 scripts/check-qt-symbol-floor.py --qt-libs /path/to/qt/lib dist/*.deb

    # ② 多版本矩阵（--qt-root 下每个子目录一个版本，目录名即版本号）
    python3 scripts/check-qt-symbol-floor.py --qt-root /path/to/qt-ref \
        arch.pkg.tar.zst deb.pkg rpm.pkg

    # ③ 当门禁用：要求某个版本必须通过（不通过就 exit 1）
    python3 scripts/check-qt-symbol-floor.py --qt-libs /path/to/qt/lib --require 6.11.2 pkg.tar.zst

输入可以是：已解开的目录、`.pkg.tar.zst`、`.deb`、`.rpm`（rpm 时按压缩载荷魔数 + cpio 解析，
不需要 rpm2cpio）。只需要 stdlib + `readelf`（binutils）。

参考库从哪来（Arch 保留每个小版本的包，最省事）
-----------------------------------------------
    mkdir -p qt-ref && cd qt-ref
    base=https://archive.archlinux.org/packages/q
    for v in 6.4.2-1 6.5.3-1 6.6.3-1 6.7.3-2 6.8.3-1 6.9.2-1 6.10.2-1 6.11.2-3 6.12.0-2; do
      vv="${v%%-*}"
      for p in qt6-base qt6-declarative; do
        f="$p-$v-x86_64.pkg.tar.zst"
        mkdir -p "$vv"
        curl -fsSL -o "$vv/$f" "$base/$p/$f" || echo "跳过（该版本此 pkgrel 不存在）：$f"
      done
    done
    # 注：qt6-base 与 qt6-declarative 的 pkgrel 不一定相同，按需调整上面列表。
    # Debian/Ubuntu 的系统 Qt 也能当参考集：解开 qt6-base-dev 后取 /usr/lib/x86_64-linux-gnu/libQt6*。
"""
from __future__ import annotations

import argparse
import gzip
import io
import json
import lzma
import os
import re
import subprocess
import sys
import tarfile
import tempfile

ELF_MAGIC = b"\x7fELF"
SYM_RE = re.compile(
    r"^\s*\d+:\s+(?P<value>[0-9a-f]+)\s+(?P<size>\d+)\s+(?P<type>\S+)\s+"
    r"(?P<bind>\S+)\s+(?P<vis>\S+)\s+(?P<ndx>\S+)\s+(?P<name>\S+?)"
    r"(?:\s+\((?P<vindex>\d+)\))?\s*$"
)


# ------------------------------------------------------------------ ELF 读取

def readelf(bin_path: str, path: str, *flags: str) -> str:
    proc = subprocess.run([bin_path, "-W", *flags, path],
                          capture_output=True, text=True, errors="replace")
    return proc.stdout


def dyn_syms(bin_path: str, path: str):
    """-> (undef: {名字: 版本}, defined: {名字})，名字已去掉 @版本 后缀。"""
    undef, defined = {}, set()
    for line in readelf(bin_path, path, "--dyn-syms").splitlines():
        m = SYM_RE.match(line)
        if not m or m.group("name").endswith(":"):
            continue
        base, _, ver = m.group("name").partition("@")
        base = base.rstrip("@")
        if m.group("ndx") == "UND":
            undef[base] = ver or None
        else:
            defined.add(base)
    return undef, defined


def needed(bin_path: str, path: str) -> list[str]:
    out = []
    for line in readelf(bin_path, path, "--dynamic").splitlines():
        if "NEEDED" in line and "Shared library:" in line:
            out.append(line.split("[", 1)[1].split("]", 1)[0])
    return out


def soname(bin_path: str, path: str) -> str | None:
    for line in readelf(bin_path, path, "--dynamic").splitlines():
        if "SONAME" in line and "Library soname:" in line:
            return line.split("[", 1)[1].split("]", 1)[0]
    return None


def verneed_versions(bin_path: str, path: str) -> set[str]:
    vers = set()
    for line in readelf(bin_path, path, "-V").splitlines():
        vers |= set(re.findall(r"([A-Za-z_][\w.]*_\d[\w.]*|Qt_\d[\w.]*)", line))
    return vers


def is_elf_bytes(blob: bytes) -> bool:
    return blob[:4] == ELF_MAGIC


def is_elf_file(path: str) -> bool:
    try:
        with open(path, "rb") as fh:
            return fh.read(4) == ELF_MAGIC
    except OSError:
        return False


# ------------------------------------------------------------------ 产物解包

def _zstd_open(path: str):
    try:
        from compression import zstd          # Python ≥ 3.14 标准库
        return zstd.open(path, "rb")
    except ImportError:
        pass
    try:
        import zstandard                      # 第三方回退
    except ImportError as exc:                # pragma: no cover
        raise SystemExit("需要 zstd 支持：Python ≥ 3.14（compression.zstd）或 pip install zstandard") from exc
    return zstandard.ZstdDecompressor().stream_reader(open(path, "rb"))


def _tar_members(fileobj, keep=lambda name: True) -> dict[str, bytes]:
    out: dict[str, bytes] = {}
    with tarfile.open(fileobj=fileobj, mode="r|") as tf:
        for m in tf:
            if m.isfile() and keep(m.name):
                fh = tf.extractfile(m)
                out[m.name.lstrip("./")] = fh.read() if fh else b""
    return out


def read_pkg_tar_zst(path: str) -> dict[str, bytes]:
    with _zstd_open(path) as fh:
        return _tar_members(fh, lambda n: not n.startswith("."))


def _ar_members(path: str):
    data = open(path, "rb").read()
    if data[:8] != b"!<arch>\n":
        raise SystemExit(f"{path}: 不是 ar 归档（期望 .deb）")
    pos, out = 8, []
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        name = hdr[0:16].decode("ascii", "replace").strip()
        size = int(hdr[48:58].decode("ascii").strip())
        out.append((name, data[pos + 60:pos + 60 + size]))
        pos += 60 + size + (size & 1)
    return out


def read_deb(path: str) -> dict[str, bytes]:
    for name, body in _ar_members(path):
        if not name.startswith("data.tar"):
            continue
        if name.endswith(".xz"):
            raw = lzma.decompress(body)
        elif name.endswith(".gz"):
            raw = gzip.decompress(body)
        elif name.endswith(".zst"):
            from compression import zstd
            raw = zstd.decompress(body)
        else:
            raw = body
        return _tar_members(io.BytesIO(raw))
    raise SystemExit(f"{path}: deb 内没有 data.tar")


def _cpio_newc(raw: bytes) -> dict[str, bytes]:
    pos, out = 0, {}
    while pos + 110 <= len(raw) and raw[pos:pos + 6] == b"070701":
        f = [int(raw[pos + 6 + 8 * i:pos + 14 + 8 * i], 16) for i in range(13)]
        filesize, namesize = f[6], f[11]
        name = raw[pos + 110:pos + 110 + namesize - 1].decode("utf-8", "replace")
        pos += 110 + namesize
        pos += (-pos) % 4
        out[name] = raw[pos:pos + filesize]
        pos += filesize
        pos += (-pos) % 4
    return out


def read_rpm(path: str) -> dict[str, bytes]:
    data = open(path, "rb").read()
    magics = [(b"\x28\xb5\x2f\xfd", "zst"), (b"\xfd7zXZ\x00", "xz"), (b"\x1f\x8b\x08", "gz")]
    hit = min(((data.find(m), k) for m, k in magics if data.find(m) != -1), default=None)
    if hit is None:
        raise SystemExit(f"{path}: rpm 里找不到已知压缩载荷（可改用 rpm2cpio | cpio）")
    off, kind = hit
    payload = data[off:]
    if kind == "zst":
        from compression import zstd
        raw = zstd.decompress(payload)
    elif kind == "xz":
        raw = lzma.decompress(payload)
    else:
        raw = gzip.decompress(payload)
    return {k.lstrip("./"): v for k, v in _cpio_newc(raw).items()}


def artifact_elfs(path: str) -> dict[str, bytes]:
    """返回 {相对路径: 内容}，只保留 ELF 文件。"""
    if os.path.isdir(path):
        out = {}
        for root, _dirs, files in os.walk(path):
            for f in files:
                full = os.path.join(root, f)
                if is_elf_file(full):
                    out[os.path.relpath(full, path).replace("\\", "/")] = open(full, "rb").read()
        return out
    if path.endswith(".pkg.tar.zst"):
        members = read_pkg_tar_zst(path)
    elif path.endswith(".deb"):
        members = read_deb(path)
    elif path.endswith(".rpm"):
        members = read_rpm(path)
    else:
        raise SystemExit(f"{path}: 只认目录 / .pkg.tar.zst / .deb / .rpm")
    return {n: b for n, b in members.items() if is_elf_bytes(b)}


# ------------------------------------------------------------------ 目标 Qt

def index_lib_dir(libdir: str, bin_path: str) -> dict[str, set[str]]:
    """目录里的 libQt6*.so* → {SONAME: 导出集}。"""
    by_soname: dict[str, set[str]] = {}
    for root, _dirs, files in os.walk(libdir):
        for f in files:
            if "libQt6" not in f or (".so" not in f):
                continue
            full = os.path.join(root, f)
            if not is_elf_file(full):
                continue
            sn = soname(bin_path, full) or f
            _, defined = dyn_syms(bin_path, full)
            by_soname.setdefault(sn, set()).update(defined)
    return by_soname


def index_pkg_dir(dirpath: str, bin_path: str) -> dict[str, set[str]]:
    """目录里若干 qt6-*.pkg.tar.zst → {SONAME: 导出集}（Arch 参考集的常见形态）。"""
    by_soname: dict[str, set[str]] = {}
    fd, tmp = tempfile.mkstemp(prefix="cqs-elf-")
    os.close(fd)
    try:
        for name in sorted(os.listdir(dirpath)):
            if not name.endswith(".pkg.tar.zst"):
                continue
            for member, blob in read_pkg_tar_zst(os.path.join(dirpath, name)).items():
                if "libQt6" not in member or ".so" not in member:
                    continue
                with open(tmp, "wb") as fh:
                    fh.write(blob)
                sn = soname(bin_path, tmp) or os.path.basename(member)
                _, defined = dyn_syms(bin_path, tmp)
                by_soname.setdefault(sn, set()).update(defined)
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass
    return by_soname


def build_targets(args) -> dict[str, dict[str, set[str]]]:
    """-> {版本标签: {SONAME: 导出集}}；--qt-libs 时标签固定为 'target'。"""
    if args.qt_libs:
        return {args.qt_libs_label: _index_any(args.qt_libs, args.readelf)}
    targets = {}
    for ver in sorted(os.listdir(args.qt_root), key=lambda v: [int(x) for x in re.findall(r"\d+", v)] or [0]):
        d = os.path.join(args.qt_root, ver)
        if os.path.isdir(d):
            targets[ver] = _index_any(d, args.readelf)
    if not targets:
        raise SystemExit(f"{args.qt_root}: 下没有按版本命名的子目录")
    return targets


def _index_any(dirpath: str, bin_path: str) -> dict[str, set[str]]:
    if any(n.endswith(".pkg.tar.zst") for n in os.listdir(dirpath)):
        return index_pkg_dir(dirpath, bin_path)
    return index_lib_dir(dirpath, bin_path)


# ------------------------------------------------------------------ 判定

def check_artifact(path: str, targets: dict, bin_path: str, workdir: str) -> dict:
    os.makedirs(workdir, exist_ok=True)
    elfs = artifact_elfs(path)
    if not elfs:
        raise SystemExit(f"{path}: 没找到 ELF 文件")
    per_bin = {}
    for rel, blob in sorted(elfs.items()):
        tmp = os.path.join(workdir, os.path.basename(rel) or "bin")
        with open(tmp, "wb") as fh:
            fh.write(blob)
        undef, _ = dyn_syms(bin_path, tmp)
        need = needed(bin_path, tmp)
        glibc = sorted((v for v in verneed_versions(bin_path, tmp)
                        if re.match(r"^GLIBC_2\.\d+$", v)),
                       key=lambda s: [int(x) for x in s.split(".")[1:]])
        per_bin[rel] = {
            "qt_und": {s for s, v in undef.items() if (v or "").startswith("Qt_6")},
            "needed": need,
            "glibc_max": glibc[-1] if glibc else None,
        }
    rows = []
    for ver, by_soname in targets.items():
        missing = {}
        for rel, info in per_bin.items():
            avail: set[str] = set()
            for lib in set(info["needed"]) & set(by_soname):
                avail |= by_soname[lib]
            gap = sorted(info["qt_und"] - avail)
            if gap:
                missing[rel] = gap
        rows.append({"target": ver, "missing": missing})
    # 目标里缺哪些"本产物要加载的 Qt 库"——缺库会让结论偏严（那些库的符号一律算缺）
    first = next(iter(targets.values()))
    missing_libs = sorted({lib for info in per_bin.values() for lib in info["needed"]
                           if "libQt6" in lib and lib not in first})
    return {"path": path, "binaries": per_bin, "rows": rows, "missing_libs": missing_libs}


def main() -> int:
    ap = argparse.ArgumentParser(description="Qt 符号面下限校验（产物能跑在哪些 Qt 上）")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--qt-libs", metavar="DIR",
                     help="单个目标 Qt 的库目录（含 libQt6*.so*）")
    src.add_argument("--qt-root", metavar="DIR",
                     help="多版本矩阵：每个子目录一个版本，目录名即版本号")
    ap.add_argument("--qt-libs-label", default="target", help="--qt-libs 模式下的版本标签（默认 target）")
    ap.add_argument("--readelf", default="readelf", help="readelf 路径（默认取 PATH）")
    ap.add_argument("--require", action="append", default=[],
                    help="要求某个目标版本必须通过，否则 exit 1（可多次）")
    ap.add_argument("--json", metavar="FILE", help="把完整结果写成 JSON")
    ap.add_argument("--work", default=None, help="临时解包目录（默认系统临时目录）")
    ap.add_argument("artifacts", nargs="+", help="产物：目录 / .pkg.tar.zst / .deb / .rpm")
    args = ap.parse_args()

    targets = build_targets(args)
    print(f"目标 Qt：{', '.join(targets)}")

    work = args.work or os.path.join(os.environ.get("TMPDIR", "/tmp"), "check-qt-symbol-floor")
    report, verdict = {}, {}
    for artifact in args.artifacts:
        name = os.path.basename(artifact) or artifact
        res = check_artifact(artifact, targets, args.readelf,
                             os.path.join(work, name.replace("/", "_")))
        report[name] = {
            "path": res["path"],
            "missing_libs": res["missing_libs"],
            "binaries": {rel: {"qt_und": sorted(info["qt_und"]),
                               "needed": info["needed"],
                               "glibc_max": info["glibc_max"]}
                         for rel, info in res["binaries"].items()},
            "rows": res["rows"],
        }
        verdict[name] = {row["target"]: not row["missing"] for row in res["rows"]}
        print(f"\n== {name}")
        if res["missing_libs"]:
            print(f"   ⚠️ 目标里缺少本产物要加载的库：{', '.join(res['missing_libs'])}")
            print("      → 这些库的符号会被算作缺失、结论偏严；请把对应 Qt 包也放进目标集")
        for rel, info in res["binaries"].items():
            qt_n = len(info["qt_und"])
            if qt_n == 0:
                print(f"   {rel}: 不直接依赖 Qt（glibc 最高 {info['glibc_max'] or '-'}）")
                continue
            print(f"   {rel}: Qt 未定义符号 {qt_n} 个；glibc 最高 {info['glibc_max'] or '-'}")
        lowest_ok = None
        for row in res["rows"]:
            if row["missing"]:
                detail = "; ".join(f"{k}: 缺 {len(v)}（如 {v[0]}）" for k, v in row["missing"].items())
                print(f"     Qt {row['target']:<8} ✗ {detail}")
            else:
                lowest_ok = lowest_ok or row["target"]
                print(f"     Qt {row['target']:<8} ✓ 可跑")
        print(f"   → 最低可运行 Qt：{lowest_ok or '（表内无）'}")

    failed = []
    for req in args.require:
        if req not in targets:
            print(f"✗ --require {req}：不在测试目标 {', '.join(targets)} 里", file=sys.stderr)
            return 2
        bad = [n for n, v in verdict.items() if not v.get(req)]
        if bad:
            failed.append(f"{req}（未通过：{', '.join(bad)}）")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(report, fh, ensure_ascii=False, indent=1)
        print(f"\nJSON → {args.json}")
    if failed:
        print(f"\n✗ --require 不满足：{', '.join(failed)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
