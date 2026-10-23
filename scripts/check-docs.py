#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Public tracked Markdown guard; --staged reads blobs and paths from the index.

No recursive filesystem scan: ignored local repositories are never documentation.
Fenced examples (backticks or tildes) are exempt. Heading anchors are not checked.
"""
import argparse
from datetime import datetime
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import subprocess
import sys
from urllib.parse import unquote

INLINE = re.compile(r"(`+)(.+?)\1")
PREFIXES = ("core/", "audio/", "cli/", "app/", "tests/", "doc/", "scripts/",
            "skins/", "third_party/", "tools/", ".github/", ".githooks/")
DEST = r'''(?:<([^>]+)>|([^\s)]+))(?:\s+["'][^\n]*?["'])?'''
LINK = re.compile(r'!?\[[^\]]*\]\(' + DEST + r'\)')
REFERENCE = re.compile(r'^\s{0,3}\[[^\]]+\]:\s*' + DEST)
BAD_PREFIX = "| 抽象 | 设计决策 | 格式差异收敛处 |"


def git(*args):
    return subprocess.check_output(["git", *args])


def public_doc(path):
    p = PurePosixPath(path)
    return (path in ("README.md", "AGENTS.md")
            or (p.name == "AGENTS.md" and p.parts[0] != "local")
            or (p.parts[0] == "doc" and p.suffix.lower() == ".md"))


def outside_fences(text):
    fence = None
    for number, line in enumerate(text.splitlines(), 1):
        marker = re.match(r"^\s{0,3}(`{3,}|~{3,})(.*)$", line)
        if fence:
            if (marker and marker[1][0] == fence[0] and len(marker[1]) >= len(fence)
                    and not marker[2].strip()):
                fence = None
            continue
        if marker:
            fence = marker[1]
            continue
        yield number, line


def check(staged=False):
    entries, unresolved = {}, set()
    for record in git("ls-files", "--stage", "-z").split(b"\0"):
        if not record:
            continue
        metadata, raw_path = record.split(b"\t", 1)
        mode, oid, stage = metadata.decode("ascii").split()
        path = raw_path.decode("utf-8")
        if stage != "0":
            unresolved.add(path)
        elif mode != "160000":  # Do not traverse nested repositories/submodules.
            entries[path] = oid
    errors = []
    if staged:
        errors.extend(f"{p}: index 中存在未解决的文档冲突" for p in sorted(unresolved) if public_doc(p))
    paths = set(entries)
    directories = {str(parent) for p in paths for parent in PurePosixPath(p).parents}
    doc_ids = {PurePosixPath(p).name[:2] for p in paths
               if re.match(r"^doc/\d{2}-[^/]+\.md$", p) and (staged or Path(p).is_file())}

    def exists(path):
        if path == ".." or path.startswith("../") or path.startswith("/"):
            return False
        if staged:
            return path in paths or path in directories
        return Path(path).exists()

    def target_error(target, source, relative=False):
        if not target or target.startswith(("#", "//")) or re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target):
            return None
        target = unquote(target.split("#", 1)[0].split("?", 1)[0])
        if not target:
            return None
        normalized = posixpath.normpath(posixpath.join(posixpath.dirname(source), target) if relative else target)
        if normalized == "local/doc" or normalized.startswith("local/doc/"):
            return "公开文档不可依赖本机笔记: " + target
        if not relative:
            short = re.fullmatch(r"doc/(\d{2})", target)
            if short:
                return None if short[1] in doc_ids else "不存在的文档: " + target
            if any(c in target for c in "*{$<"):
                return None  # Explicit templates, not concrete pointers.
        if not exists(normalized):
            return "指针悬空: " + target
        return None

    scanned = 0
    for path in sorted(paths | (unresolved if not staged else set())):
        if not public_doc(path):
            continue
        try:
            data = git("cat-file", "blob", entries[path]) if staged else Path(path).read_bytes()
            text = data.decode("utf-8-sig")
        except FileNotFoundError:
            continue  # Worktree deletion; staged deletions are absent from ls-files.
        except UnicodeDecodeError:
            errors.append(f"{path}: 文档必须为 UTF-8")
            continue
        scanned += 1
        lines = list(outside_fences(text))
        for number, line in lines:
            if re.match(r"^\s*(?:<{7}(?: |$)|={7}$|>{7}(?: |$)|\|{7}(?: |$))", line):
                errors.append(f"{path}:{number}: 遗留冲突标记")
        # PR31 accidentally prepended this table header to every line of doc/core-model.md.
        if sum(line.startswith(BAD_PREFIX) for _, line in lines) > 1:
            errors.append(f"{path}: 表头前缀污染全文")

        if path.startswith("doc/pending/"):
            name = PurePosixPath(path).name
            valid = re.fullmatch(r"\d{6}-[a-z0-9]+(?:-[a-z0-9]+)*\.md", name)
            try:
                datetime.strptime(name[:6], "%y%m%d")
            except ValueError:
                valid = False
            if not valid:
                errors.append(f"{path}: 文件名须为有效日期 YYMMDD-主题.md（小写字母数字连字符）")
            prose = "\n".join(INLINE.sub("", line) for _, line in lines)
            for heading in ("背景", "出口条件"):
                section = re.search(r"^#{1,6}\s+" + heading + r"[^\n]*\n(.*?)(?=^#{1,6}\s|\Z)", prose, re.M | re.S)
                if not section or not section[1].strip():
                    errors.append(f"{path}: 缺少非空的{heading}章节")

        for number, line in lines:
            targets = []
            for match in INLINE.finditer(line):
                value = match[2]
                if value.startswith(PREFIXES) or value.startswith("local/doc"):
                    # Existing docs use e.g. `doc/command-protocol.md §3`.
                    targets.append((value.split()[0], False))
                elif value in ("README.md", "AGENTS.md", "CMakeLists.txt"):
                    targets.append((value, False))
            prose = INLINE.sub("", line)
            if re.search(r"(?<![\w/])local/doc(?:/|\b)", prose):
                errors.append(f"{path}:{number}: 公开文档不可依赖本机笔记: local/doc")
            for match in LINK.finditer(prose):
                targets.append((match[1] or match[2], True))
            reference = REFERENCE.match(prose)
            if reference:
                targets.append((reference[1] or reference[2], True))
            targets.extend((short, False) for short in re.findall(r"\bdoc/\d{2}\b(?![-/])", prose))
            for target, relative in targets:
                error = target_error(target, path, relative)
                if error:
                    errors.append(f"{path}:{number}: {error}")
            if re.search(r"[「」『』]", prose):
                errors.append(f"{path}:{number}: 直角引号请改用中文弯引号 “ ”")
            if any(re.search(r"[\u4e00-\u9fff]", m) for m in re.findall(r'"([^"\n]*)"', prose)):
                errors.append(f"{path}:{number}: ASCII 直引号包中文，请用 “ ”")
    for error in errors:
        print("[check-docs] " + error)
    snapshot = "index" if staged else "worktree"
    print(f"[check-docs] {'失败' if errors else '通过'}（{snapshot}，{scanned} 个公开文档）")
    return 1 if errors else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--staged", action="store_true", help="check index instead of worktree")
    args = parser.parse_args()
    os.chdir(git("rev-parse", "--show-toplevel").decode("utf-8").strip())
    return check(args.staged)


if __name__ == "__main__":
    sys.exit(main())
