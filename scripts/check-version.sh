#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# 版本一致性检查：打 tag / 发布前，把「版本号散落各处」的漏改挡在本地。
# 背景：过去出现过「先发布、回头再补 commit 改 README/文档」的情况（见 doc/10）。
#
# 用法:
#   scripts/check-version.sh v0.3.2                  # 检查某个 tag/版本
#   scripts/check-version.sh 0.3.2 --strict          # 警告也算失败（release 流水线用）
#   scripts/check-version.sh v0.3.2 --print-notes    # 只输出该版本的 CHANGELOG 段落
#   scripts/check-version.sh v0.3.2 --qt-version 6.11
#
# 硬性检查（失败即退出 1）:
#   1. CMakeLists.txt 的 project(... VERSION x.y.z) 与目标版本一致
#   2. CHANGELOG.md 有 "## [x.y.z]" 小节，且小节里至少有一条内容
#   3. README.md 里出现的发行物名 beatbench-vX.Y.Z-win64 必须与目标版本一致
# 警告（--strict 时视为失败）:
#   4. 被跟踪文档里的 Qt 版本示例与当前 Qt minor 不一致（如残留 /c/Qt/6.8.0）
#   5. CHANGELOG 之外的文档里残留其它版本的发行物名
set -euo pipefail

cd "$(dirname "$0")/.."

STRICT=0
PRINT_NOTES=0
QT_MINOR="6.11"
ARG=""

while [ $# -gt 0 ]; do
  case "$1" in
    --strict) STRICT=1 ;;
    --print-notes) PRINT_NOTES=1 ;;
    --qt-version) shift; QT_MINOR="${1:-}" ;;
    --qt-version=*) QT_MINOR="${1#*=}" ;;
    -h | --help) sed -n '4,18p' "$0"; exit 0 ;;
    -*) echo "未知参数: $1（--help 看用法）" >&2; exit 2 ;;
    *) ARG="$1" ;;
  esac
  shift
done

[ -n "$ARG" ] || { sed -n '4,18p' "$0" >&2; exit 2; }
VER="${ARG#v}"
case "$VER" in
  [0-9]*.[0-9]*.[0-9]*) ;;
  *) echo "版本格式不对: $ARG（期望 vX.Y.Z 或 X.Y.Z）" >&2; exit 2 ;;
esac

extract_notes() {
  awk -v v="$VER" '
    $0 ~ "^## \\[" v "\\]" { inside = 1; next }
    inside && /^## \[/ { exit }
    inside { print }
  ' CHANGELOG.md
}

if [ "$PRINT_NOTES" -eq 1 ]; then
  extract_notes
  exit 0
fi

FAIL=0
WARN=0
fail() { echo "  ❌ $*"; FAIL=$((FAIL + 1)); }
warn() { echo "  ⚠️  $*"; WARN=$((WARN + 1)); }

echo "==> 版本一致性检查：v$VER（Qt minor $QT_MINOR）"

# ---- 1. CMakeLists 版本 ----
CMAKE_VER="$(grep -m1 -oE 'VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | awk '{print $2}' || true)"
if [ -z "$CMAKE_VER" ]; then
  fail "CMakeLists.txt 里读不到 project(... VERSION x.y.z)"
elif [ "$CMAKE_VER" != "$VER" ]; then
  fail "CMakeLists.txt 版本是 $CMAKE_VER，与目标 v$VER 不一致"
else
  echo "  ✓ CMakeLists.txt: $CMAKE_VER"
fi

# ---- 2. CHANGELOG 小节 ----
if ! grep -qE "^## \[$VER\]" CHANGELOG.md; then
  fail "CHANGELOG.md 缺少小节 “## [$VER]”"
elif [ -z "$(extract_notes | grep -E '^[[:space:]]*-[[:space:]]' || true)" ]; then
  fail "CHANGELOG.md 的 [$VER] 小节是空的"
else
  echo "  ✓ CHANGELOG.md: 有 [$VER] 小节且有内容"
fi

# ---- 3. README 发行物名 ----
mapfile -t readme_assets < <(git grep -hoE 'beatbench-v[0-9]+\.[0-9]+\.[0-9]+-win64' -- README.md | sort -u || true)
if [ "${#readme_assets[@]}" -gt 0 ]; then
  if printf '%s\n' "${readme_assets[@]}" | grep -qx "beatbench-v$VER-win64"; then
    echo "  ✓ README.md: 下载名已是 beatbench-v$VER-win64"
  else
    fail "README.md 里的发行物名是 ${readme_assets[*]}，与目标 v$VER 不一致（下载链接要跟着改）"
  fi
fi

# ---- 4. Qt 版本示例残留（警告）----
while IFS= read -r line; do
  [ -n "$line" ] || continue
  case "$line" in
    *"$QT_MINOR"*) continue ;;
  esac
  warn "Qt 版本示例可能过期：$line"
done < <(git grep -nE '(/c/Qt|/g/Qt|C:/Qt|G:/Qt)/6\.[0-9]+|Qt 6\.[0-9]+' -- '*.md' || true)

# ---- 5. 其它文档里的旧发行物名（警告）----
while IFS= read -r line; do
  [ -n "$line" ] || continue
  warn "其它文档残留旧发行物名：$line"
done < <(git grep -nE 'beatbench-v[0-9]+\.[0-9]+\.[0-9]+-win64' -- '*.md' ':!CHANGELOG.md' ':!README.md' || true)

echo
if [ "$FAIL" -ne 0 ]; then
  echo "❌ 版本一致性检查未通过（硬性问题 $FAIL 处，警告 $WARN 处）"
  exit 1
fi
if [ "$STRICT" -eq 1 ] && [ "$WARN" -ne 0 ]; then
  echo "❌ --strict：有 $WARN 处警告需要处理"
  exit 1
fi
if [ "$WARN" -ne 0 ]; then
  echo "✅ 硬性检查通过（另有 $WARN 处警告，见上；--strict 会让它们变成失败）"
else
  echo "✅ 版本一致性检查全部通过"
fi
