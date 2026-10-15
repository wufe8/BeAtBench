#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# 无头截图助手（GUI 验收用）：固定离屏渲染并**解决中文显示为方框**的问题。
#
# 用法：scripts/screenshot.sh <输出.png> [额外参数…]
#   scripts/screenshot.sh local/screenshot/m7-project-page.png \
#       --project <歌曲文件夹> --page 3 --screenshot-delay 9000
# 环境：BUILD_DIR 选择构建目录；SCREENSHOT_TIMEOUT_SECONDS 默认 120 秒。
# 需要 GNU timeout（macOS 也支持 coreutils 的 gtimeout）。
# 每次 stdout/stderr 保留在 local/tmp；失败返回非零，不以旧截图冒充成功。
# 失败保留旧最终截图，只有成功才替换；调用者必须检查退出码，不能只看文件存在。
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
OUT_DIR="$(cd "$(dirname "$OUT")" && pwd -P)"
OUT="$OUT_DIR/$(basename "$OUT")"
[ ! -d "$OUT" ] || { echo "输出路径不能是目录：$OUT" >&2; exit 2; }

# 有界运行，不能让 --wait-render 等调试参数在失败时永久挂住。
TIMEOUT_SECONDS="${SCREENSHOT_TIMEOUT_SECONDS:-120}"
[[ "$TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]] || {
  echo "SCREENSHOT_TIMEOUT_SECONDS 必须是正整数" >&2; exit 2;
}
TIMEOUT_CMD="$(command -v timeout || command -v gtimeout || true)"
[ -n "$TIMEOUT_CMD" ] || {
  echo "缺少 timeout/gtimeout（GNU coreutils；macOS 可安装 coreutils）" >&2
  exit 2
}
# 运行时创建，不依赖 local/ 中任何预置文件。保留每次 stdout/stderr 供排错。
mkdir -p "$ROOT/local/tmp"
# X 放在模板末尾，兼容 BSD/macOS mktemp。
LOG="$(mktemp "$ROOT/local/tmp/screenshot-log-XXXXXX")"

EXE=""
for cand in "$BUILD_DIR/app/beatbench" "$BUILD_DIR/app/beatbench.exe" \
            "$BUILD_DIR/app/beatbench.app/Contents/MacOS/beatbench"; do
  [ -f "$cand" ] && EXE="$cand" && break
done
[ -n "$EXE" ] || { echo "找不到 GUI 可执行文件（先构建 $BUILD_DIR）；日志：$LOG" >&2; exit 1; }
EXE="$(cd "$(dirname "$EXE")" && pwd -P)/$(basename "$EXE")"
if [ -d "$BUILD_DIR/app/beatbench.app" ]; then
  export QML2_IMPORT_PATH="$(cd "$BUILD_DIR/app" && pwd -P)${QML2_IMPORT_PATH:+:$QML2_IMPORT_PATH}"
fi

export QT_QPA_PLATFORM=offscreen
export QT_QUICK_BACKEND=software
# 字体：Windows 系统字体目录；存在才设（Linux/macOS 不设，走系统字体栈）
if [ -z "${QT_QPA_FONTDIR:-}" ]; then
  for d in "${WINDIR:-/c/Windows}/Fonts" /c/Windows/Fonts; do
    if [ -d "$d" ]; then export QT_QPA_FONTDIR="$d"; break; fi
  done
fi

# 先写唯一临时图片，成功后才替换最终文件。旧截图永远不能让本次失败误报成功。
STAGE_DIR="$(mktemp -d "$OUT_DIR/.screenshot-XXXXXX")"
STAGED="$STAGE_DIR/frame.png"
cleanup() {
  # 删除前核实绝对路径的父目录、固定前缀和图片名，不递归删除。
  case "$STAGE_DIR" in "$OUT_DIR"/.screenshot-*)
    [ "$STAGED" = "$STAGE_DIR/frame.png" ] || return 1
    rm -f -- "${STAGED:?}"
    rmdir -- "${STAGE_DIR:?}"
  ;; esac
}
trap cleanup EXIT

# 从仓库根启动，保证 skins 等相对资源不受调用者 cwd 影响。
cd "$ROOT"
STATUS=0
"$TIMEOUT_CMD" --kill-after=5s "${TIMEOUT_SECONDS}s" "$EXE" --screenshot "$STAGED" "$@" >"$LOG" 2>&1 || STATUS=$?
if [ "$STATUS" -ne 0 ]; then
  echo "截图失败：程序退出码 $STATUS（124 表示超时）；日志：$LOG" >&2
  exit "$STATUS"
fi
[ -s "$STAGED" ] || { echo "截图失败：没有生成非空图片；日志：$LOG" >&2; exit 1; }
# 扩展名不等于图片有效，至少校验 PNG 签名（不宣称完整解码/视觉验收）。
SIGNATURE="$(od -An -tx1 -N8 "$STAGED" | tr -d ' \n\r')"
[ "$SIGNATURE" = '89504e470d0a1a0a' ] || {
  echo "截图失败：输出不是 PNG；日志：$LOG" >&2; exit 1;
}
# 两个绝对路径均已固定在 OUT_DIR，完成全部校验后才发布最终截图。
[ ! -d "$OUT" ] || { echo "输出路径变成了目录：$OUT" >&2; exit 2; }
case "$STAGED" in "$OUT_DIR"/.screenshot-*/frame.png) mv -f -- "$STAGED" "$OUT" ;; *) exit 2 ;; esac
[ -f "$OUT" ] || { echo "截图发布失败：$OUT；日志：$LOG" >&2; exit 1; }
BYTES="$(wc -c <"$OUT")"
echo "截图: $OUT（$BYTES 字节）${QT_QPA_FONTDIR:+（字体目录 $QT_QPA_FONTDIR）}"
echo "运行日志: $LOG"
