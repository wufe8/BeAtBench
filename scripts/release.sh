#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# 本地一键发布（Windows 包）：版本检查 → 打包（scripts/package-release.sh）→ 打印产物与后续命令。
# 目的：把「让开发会话临时拼命令打 zip」固化成一条可复现的命令；CI 的 release.yml 调的是同一套脚本。
#
# 用法:
#   scripts/release.sh              # 按 CMakeLists 当前版本打包
#   scripts/release.sh --skip-smoke # 跳过 staging 内冒烟（调试打包脚本时用）
#
# 环境变量（可选，不设则自动探测）:
#   QT_ROOT / CXX_BIN / NINJA_BIN
set -euo pipefail

cd "$(dirname "$0")/.."

SKIP_SMOKE=0
for a in "$@"; do
  [ "$a" = "--skip-smoke" ] && SKIP_SMOKE=1
done

# ---- 版本检查（硬性）----
VER="$(grep -m1 -oE 'VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | awk '{print $2}')"
[ -n "$VER" ] || { echo "错误: 读不到 CMakeLists.txt 版本" >&2; exit 1; }
echo "==> 发布前版本检查 v$VER"
scripts/check-version.sh "v$VER" || { echo "错误: 版本一致性检查未通过，先补 CHANGELOG/README" >&2; exit 1; }

# ---- 工具链探测 ----
if [ -z "${QT_ROOT:-}" ]; then
  QT_ROOT="$(ls -d /c/Qt/6.11.*/mingw_64 /g/Qt/6.11.*/mingw_64 2>/dev/null | sort -V | tail -1 || true)"
fi
if [ -z "${CXX_BIN:-}" ]; then
  for cand in /g/Qt/Tools/mingw1310_64/bin/g++.exe /c/Qt/Tools/mingw1310_64/bin/g++.exe; do
    [ -x "$cand" ] && CXX_BIN="$cand" && break
  done
fi
if [ -z "${NINJA_BIN:-}" ]; then
  for cand in /g/Qt/Tools/Ninja/ninja.exe /c/Qt/Tools/Ninja/ninja.exe; do
    [ -x "$cand" ] && NINJA_BIN="$cand" && break
  done
fi
command -v ninja >/dev/null 2>&1 && [ -z "${NINJA_BIN:-}" ] && NINJA_BIN="$(command -v ninja)"
command -v g++ >/dev/null 2>&1 && [ -z "${CXX_BIN:-}" ] && CXX_BIN="$(command -v g++)"

[ -n "${QT_ROOT:-}" ] || { echo "错误: 找不到 Qt，请设 QT_ROOT" >&2; exit 1; }
[ -n "${CXX_BIN:-}" ] || { echo "错误: 找不到 MinGW g++，请设 CXX_BIN" >&2; exit 1; }
[ -n "${NINJA_BIN:-}" ] || { echo "错误: 找不到 ninja，请设 NINJA_BIN" >&2; exit 1; }

echo "==> Qt:    $QT_ROOT"
echo "==> CXX:   $CXX_BIN"
echo "==> Ninja: $NINJA_BIN"

# ---- 打包（唯一实现：package-release.sh）----
QT_ROOT="$QT_ROOT" CXX_BIN="$CXX_BIN" NINJA_BIN="$NINJA_BIN" \
  scripts/package-release.sh $([ "$SKIP_SMOKE" -eq 1 ] && echo --skip-smoke)

# ---- 产物与后续 ----
ZIP="$PWD/out/beatbench-v$VER-win64.zip"
echo
echo "==> 产物:"
ls -l "$ZIP" "$ZIP.sha256" 2>/dev/null | awk '{print "    "$5" bytes  "$9}'
echo
echo "==> 后续（二选一）:"
echo "    A. 走 CI 发布（推荐）：git tag v$VER && git push origin v$VER   # release.yml 自动建 Release"
echo "    B. 本地上传：gh release create v$VER \"$ZIP\" \"$ZIP.sha256\" \\"
echo "         --title \"BeAtBench v$VER\" --notes-file <(scripts/check-version.sh v$VER --print-notes)"
