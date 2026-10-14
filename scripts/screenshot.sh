#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# 无头截图助手（GUI 验收用）：固定离屏渲染并**解决中文显示为方框**的问题。
#
# 用法：scripts/screenshot.sh <输出.png> [额外参数…]
#   scripts/screenshot.sh local/screenshot/m7-project-page.png \
#       --project <歌曲文件夹> --page 3 --screenshot-delay 9000
#
# 为什么需要它（2026-10-14）：
#   Qt 的 offscreen 平台插件**没有原生字体库**（Windows 上不走 GDI/DirectWrite），
#   未指定字体目录时 Qt 只能退回内置的无字形字体 → 截图里中文全是方框。
#   `QT_QPA_FONTDIR` 给 offscreen 一个字体目录即可正常出字（Windows 用系统字体目录）。
#   Linux/macOS 的 offscreen 走 fontconfig/CoreText，无需该变量。
#
# 注意：截图是**离屏软件光栅**（QT_QUICK_BACKEND=software），与真实 GPU 渲染存在
#   抗锯齿/子像素差异；用于结构/文案验收，不要拿它做像素级回归基线。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-gui}"

if [ $# -lt 1 ]; then
  echo "用法: $0 <输出.png> [额外参数…]" >&2
  exit 2
fi

OUT="$1"; shift
case "$OUT" in /*) ;; *) OUT="$ROOT/$OUT" ;; esac
mkdir -p "$(dirname "$OUT")"

EXE=""
for cand in "$BUILD_DIR/app/beatbench" "$BUILD_DIR/app/beatbench.exe" \
            "$BUILD_DIR/app/beatbench.app/Contents/MacOS/beatbench"; do
  [ -f "$cand" ] && EXE="$cand" && break
done
[ -n "$EXE" ] || { echo "找不到 GUI 可执行文件（先构建 $BUILD_DIR）" >&2; exit 1; }

export QT_QPA_PLATFORM=offscreen
export QT_QUICK_BACKEND=software
# 字体：Windows 系统字体目录；存在才设（Linux/macOS 不设，走系统字体栈）
if [ -z "${QT_QPA_FONTDIR:-}" ]; then
  for d in "${WINDIR:-/c/Windows}/Fonts" /c/Windows/Fonts; do
    if [ -d "$d" ]; then export QT_QPA_FONTDIR="$d"; break; fi
  done
fi

"$EXE" --screenshot "$OUT" "$@" >/dev/null 2>&1 || true
[ -f "$OUT" ] || { echo "截图失败：$OUT" >&2; exit 1; }
echo "截图: $OUT（$(wc -c <"$OUT") 字节）${QT_QPA_FONTDIR:+（字体目录 $QT_QPA_FONTDIR）}"
