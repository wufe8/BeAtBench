#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# 文档护栏（pre-commit 自动调用；也可手动跑）：scripts/check-docs.sh
#
#   ① 挂账生命周期：doc/pending/ 文件名 YYMMDD-主题.md（小写连字符），正文必带出口条件
#   ② 指针有效性：markdown 散文中的仓库路径引用必须存在（代码块内的示例不查）
#   ③ 死机制防再犯：不得引用 local/doc（该机制已删除）
#   ④ 中文弯引号：非代码文本不出现直角引号「」；ASCII 直引号不得包中文
#
# 扫描面：根/模块 AGENTS.md、doc/**/*.md、README.md
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

command -v python3 >/dev/null || { echo "[check-docs] 需要 python3" >&2; exit 1; }

if rg -n "local/doc" AGENTS.md README.md doc/*.md doc/pending/*.md */AGENTS.md >/dev/null 2>&1; then
  echo "[check-docs] 发现 local/doc 引用（该机制已删除，见根 AGENTS.md §3）：" >&2
  rg -n "local/doc" AGENTS.md README.md doc/*.md doc/pending/*.md */AGENTS.md >&2
  exit 1
fi

python3 - <<'PYEOF'
import re
import sys
from pathlib import Path

errors = []

# ---- 收集文件 ----
files = [Path("AGENTS.md"), Path("README.md")]
files += sorted(Path(".").glob("*/AGENTS.md"))
files += sorted(Path("doc").rglob("*.md"))
files = [f for f in files if f.is_file()]

INLINE = re.compile(r"`[^`\n]*`")


def prose_lines(text: str):
    """逐行剥离代码围栏与行内代码，返回 (行号, 散文行)。"""
    out = []
    in_fence = False
    for i, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        out.append((i, INLINE.sub("", line)))
    return out


# ---- ① 挂账生命周期 ----
name_re = re.compile(r"^\d{6}-[a-z0-9-]+\.md$")
pending = Path("doc/pending")
if pending.is_dir():
    for f in sorted(pending.glob("*.md")):
        if not name_re.match(f.name):
            errors.append(f"{f}: 文件名不符合 YYMMDD-主题.md（小写字母数字连字符）")
        if "出口" not in f.read_text(encoding="utf-8"):
            errors.append(f"{f}: 正文缺少出口条件")

# ---- ② 指针有效性 ----
PREFIXES = ("core/", "audio/", "cli/", "app/", "tests/", "doc/", "scripts/",
            "skins/", "third_party/", "tools/", ".github/")
path_re = re.compile(r"`((?:%s)[^\s`]+)`" % "|".join(re.escape(p) for p in PREFIXES))
doc_ids = {p.name[:2] for p in Path("doc").glob("[0-9][0-9]-*.md")}

for f in files:
    text = f.read_text(encoding="utf-8")
    joined = "\n".join(line for _, line in prose_lines(text))
    # 行内代码已剥离，这里查散文中反引号包裹的路径需要原始行：
    # 改为对原始文本逐行（跳过围栏）跑 path_re
    in_fence = False
    for i, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        for m in path_re.finditer(line):
            target = m.group(1).rstrip(".,;、）")
            if "*" in target or "{" in target or "$" in target:
                continue
            pure = re.fullmatch(r"doc/(\d{2})", target)
            if pure:
                if pure.group(1) not in doc_ids:
                    errors.append(f"{f}:{i}: 引用了不存在的文档 {target}")
                continue
            if not Path(target).exists():
                errors.append(f"{f}:{i}: 指针悬空 `{target}`")
    # 散文（非反引号）中的 doc/NN 简写引用
    for i, line in prose_lines(text):
        for m in re.finditer(r"\bdoc/(\d{2})\b", line):
            if m.group(1) not in doc_ids:
                errors.append(f"{f}:{i}: 引用了不存在的文档 doc/{m.group(1)}")

# ---- ④ 中文弯引号 ----
cjk = re.compile(r"[\u4e00-\u9fff]")
corner = re.compile(r"[「」『』]")

for f in files:
    for i, line in prose_lines(f.read_text(encoding="utf-8")):
        if corner.search(line):
            errors.append(f"{f}:{i}: 直角引号 → 用弯引号 “ ”")
        for m in re.finditer(r'"([^"\n]*)"', line):
            if cjk.search(m.group(1)):
                errors.append(f'{f}:{i}: ASCII 直引号包中文 → 用弯引号: "{m.group(1)}"')

if errors:
    print(f"[check-docs] {len(errors)} 处问题：")
    for e in errors:
        print("  " + e)
    sys.exit(1)
print(f"[check-docs] 通过（{len(files)} 个文件）")
PYEOF
