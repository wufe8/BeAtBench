#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# 版本一致性检查：打 tag / 发布前，把「版本号散落各处」的漏改挡在本地。
# 背景：过去出现过「先发布、回头再补 commit 改 README/文档」的情况（见 doc/dev-workflow.md）。
#
# 用法:
#   scripts/check-version.sh v0.3.2                  # 检查某个 tag/版本
#   scripts/check-version.sh 0.3.2 --strict          # 警告也算失败（release 流水线用）
#   scripts/check-version.sh v0.3.2 --print-notes    # 只输出该版本的 CHANGELOG 段落
#   scripts/check-version.sh v0.3.2 --qt-version 6.11            # CI/打包所用 Qt minor
#   scripts/check-version.sh v0.3.2 --qt-version 6.11,6.12       # 允许多个 minor（逗号分隔）
#   scripts/check-version.sh v0.3.2 --qt-version 6.11 --qt-floor 6.4   # 文档同时宣传的 Qt 下限
#
# 硬性检查（失败即退出 1）:
#   1. CMakeLists.txt 的 project(... VERSION x.y.z) 与目标版本一致
#   2. CHANGELOG.md 有 "## [x.y.z]" 小节，且小节里至少有一条内容
#   3. README.md 的唯一正文版本标记与目标一致；安装示例不硬编码版本化资产名
#   4. core/.../Version.hpp 的 kVersion 与 CMakeLists 版本一致
#      （CLI `version`、JSON 协议 version、GUI「关于」都读它；v0.3.2 首发漏改过它）
# 警告（--strict 时视为失败）:
#   5. 被跟踪文档里的 Qt 版本示例与当前 Qt minor 不一致（如残留 /c/Qt/6.8.0）
#   6. CHANGELOG 之外的文档里残留其它版本的发行物名（含旧命名格式）
set -euo pipefail

cd "$(dirname "$0")/.."

STRICT=0
PRINT_NOTES=0
# Qt 版本矩阵的**单一来源**（文档里出现这些版本都算有效，其它 Qt 6.x 判为过期示例）：
#   6.11 = CI/打包所用 minor；6.12 = 有意记录的上游缺陷绕过说明；
#   6.4  = 源码 API 下限，不等同于各发行包的运行基线。
# 矩阵变化时只改这两行；pre-push 钩子与 release.yml 都用默认值，避免多处漂移。
QT_MINOR="6.11,6.12"
QT_FLOOR="6.4"
ARG=""

while [ $# -gt 0 ]; do
  case "$1" in
    --strict) STRICT=1 ;;
    --print-notes) PRINT_NOTES=1 ;;
    --qt-version) shift; QT_MINOR="${1:-}" ;;
    --qt-version=*) QT_MINOR="${1#*=}" ;;
    --qt-floor) shift; QT_FLOOR="${1:-}" ;;
    --qt-floor=*) QT_FLOOR="${1#*=}" ;;
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

echo "==> 版本一致性检查：v$VER（Qt minor $QT_MINOR$([ -n "$QT_FLOOR" ] && echo "，下限 $QT_FLOOR")）"

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
# 条目符号 '-' 与 '*' 都算：release-please 自动生成的 CHANGELOG 用 '*'，
# 只认 '-' 会把「有内容的版本小节」误判成空（首次自动发版会在这里卡住 release.yml）。
elif [ -z "$(extract_notes | grep -E '^[[:space:]]*[-*][[:space:]]' || true)" ]; then
  fail "CHANGELOG.md 的 [$VER] 小节是空的"
else
  echo "  ✓ CHANGELOG.md: 有 [$VER] 小节且有内容"
fi

# ---- 3. README 的唯一自动发布版本 ----
# 普通正文保留给 release-please，不能把 HTML 标记放进安装代码示例。
readme_marker_count="$({ grep -oF '<!-- x-release-please-version -->' README.md || true; } | wc -l)"
readme_versions=()
fence=''
fence_re='^ {0,3}(```+|~~~+)(.*)$'
version_re='^当前发布版本：v([0-9]+\.[0-9]+\.[0-9]+)<!-- x-release-please-version -->$'
while IFS= read -r line || [[ -n $line ]]; do
  line=${line%$'\r'}
  if [[ $line =~ $fence_re ]]; then
    token=${BASH_REMATCH[1]}
    rest=${BASH_REMATCH[2]}
    if [[ -z $fence ]]; then
      fence=$token
    elif [[ $token == "$fence"* && $rest != *[![:space:]]* ]]; then
      fence=''
    fi
  elif [[ -z $fence && $line =~ $version_re ]]; then
    readme_versions+=("${BASH_REMATCH[1]}")
  fi
done < README.md
if [ "$readme_marker_count" -ne 1 ] || [ "${#readme_versions[@]}" -ne 1 ]; then
  fail "README.md 必须在代码块之外保留唯一的“当前发布版本：vX.Y.Z”行和 release-please 标记"
elif [ "${readme_versions[0]}" != "$VER" ]; then
  fail "README.md 发布版本为 v${readme_versions[0]}，与目标 v$VER 不一致"
else
  echo "  ✓ README.md: v$VER（单一 release-please 标记）"
fi
# 安装示例不再硬编码版本化资产名；也拒绝旧命名（省略 v 或使用下划线）。
if grep -qE 'beatbench[_-]v?[0-9]+\.[0-9]+\.[0-9]+' README.md; then
  fail "README.md 含硬编码的版本化发行物名；安装步骤应使用实际下载的文件名"
fi

# ---- 4. Version.hpp 的 kVersion（CLI/JSON/关于页的版本来源） ----
VERSION_HPP="core/include/beatbench/core/Version.hpp"
hpp_ver="$(grep -m1 -oE 'kVersion = "[0-9]+\.[0-9]+\.[0-9]+"' "$VERSION_HPP" 2>/dev/null |
  grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || true)"
if [ -z "$hpp_ver" ]; then
  fail "读不到 $VERSION_HPP 的 kVersion"
elif [ "$hpp_ver" != "${CMAKE_VER:-$VER}" ]; then
  fail "Version.hpp 的 kVersion 是 $hpp_ver，与 CMakeLists 的 ${CMAKE_VER:-$VER} 不一致（包自报版本会错！）"
else
  echo "  ✓ Version.hpp: $hpp_ver"
fi

# ---- 4. Qt 版本示例残留（警告）----
# 文档会同时出现「CI/打包所用 minor」「宣传的 Qt 下限」，以及**有意记录**的其它版本
# （如绕开 Qt 6.12 缺陷的说明）；这些都应算有效。--qt-version 接受逗号分隔的多个 minor。
# 只有全都不匹配的 Qt 6.x 才判为过期（如残留示例路径 /c/Qt/6.8.0）。
qt_version_ok() {
  local v
  for v in ${QT_MINOR//,/ } $QT_FLOOR; do
    [ -n "$v" ] || continue
    case "$1" in *"$v"*) return 0 ;; esac
  done
  return 1
}
while IFS= read -r line; do
  [ -n "$line" ] || continue
  qt_version_ok "$line" && continue
  warn "Qt 版本示例可能过期：$line"
done < <(git grep -nE '(/c/Qt|/g/Qt|C:/Qt|G:/Qt)/6\.[0-9]+|Qt 6\.[0-9]+' -- '*.md' || true)

# ---- 5. 其它文档里的旧发行物名（警告）----
while IFS= read -r line; do
  [ -n "$line" ] || continue
  warn "其它文档残留旧发行物名：$line"
done < <(git grep -nE 'beatbench[_-]v?[0-9]+\.[0-9]+\.[0-9]+' -- '*.md' ':!CHANGELOG.md' ':!README.md' || true)

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
