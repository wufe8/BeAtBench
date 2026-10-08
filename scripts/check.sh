#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# 本地门禁：一次跑完「配置 + 构建 + ctest +（可选）GUI 无头冒烟」。
# 目的：把「手动 CI」压成一条命令，并让 pre-push hook / 开发会话有一致的入口。
#
# 用法:
#   scripts/check.sh [--quick] [--core] [--reuse] [--build-dir DIR] [--no-smoke]
#     --quick      跳过真实谱面测试（BB_SKIP_REAL=1，<1s 档）
#     --core       只走无 Qt 路径（不建 GUI、不跑冒烟）
#     --reuse      复用已有构建目录（不重新 configure）
#     --build-dir  自定义构建目录（默认 build-check）
#     --no-smoke   构建 + 测试，但不跑 GUI 冒烟
#
# 环境变量（可选，不设则自动探测）:
#   QT_PREFIX    Qt 6.11+ 安装目录（如 G:/Qt/6.11.1/mingw_64）
#   MINGW_BIN    MinGW bin 目录（Windows）
#   BUILD_DIR    同 --build-dir
#
# 约定：截图与 QML 日志写到 local/tmp/check/（不污染仓库根）。
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

QUICK=0
CORE_ONLY=0
REUSE=0
NO_SMOKE=0
BUILD_DIR="${BUILD_DIR:-$ROOT/build-check}"

while [ $# -gt 0 ]; do
  case "$1" in
    --quick) QUICK=1 ;;
    --core) CORE_ONLY=1 ;;
    --reuse) REUSE=1 ;;
    --no-smoke) NO_SMOKE=1 ;;
    --build-dir) shift; BUILD_DIR="$1" ;;
    --build-dir=*) BUILD_DIR="${1#*=}" ;;
    -h | --help) sed -n '3,22p' "$0"; exit 0 ;;
    *) echo "未知参数: $1（--help 看用法）" >&2; exit 2 ;;
  esac
  shift
done

fail() { echo "❌ $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }

# ---- 工具链探测（不设就猜常见位置，猜不到才报错）----
if [ -z "${QT_PREFIX:-}" ]; then
  QT_PREFIX="$(ls -d /c/Qt/6.11.*/mingw_64 /g/Qt/6.11.*/mingw_64 /opt/Qt/6.11.*/gcc_64 /usr/lib/x86_64-linux-gnu/cmake/Qt6 2>/dev/null | sort -V | tail -1 || true)"
fi
if [ -d "${MINGW_BIN:-}" ]; then
  export PATH="$MINGW_BIN:$PATH"
elif [ -d /g/Qt/Tools/mingw1310_64/bin ]; then
  export PATH="/g/Qt/Tools/mingw1310_64/bin:$PATH"
fi
if ! command -v ninja >/dev/null 2>&1 && [ -x /g/Qt/Tools/Ninja/ninja.exe ]; then
  export PATH="/g/Qt/Tools/Ninja:$PATH"
fi

if [ "$CORE_ONLY" -eq 0 ]; then
  { [ -n "${QT_PREFIX:-}" ] && [ -d "$QT_PREFIX" ]; } ||
    fail "找不到 Qt 6.11+：设 QT_PREFIX=<Qt 安装目录> 再跑，或改用 --core（无 Qt 路径）"
  # Windows：Qt 桥层测试与 GUI 的可执行文件要能找到 Qt DLL（否则 ctest 报 0xc0000135）；
  # Linux/macOS 靠 rpath 即可，这里加进去也无害。
  export PATH="$QT_PREFIX/bin:$PATH"
fi

# GCC 版本下限（C++20）：Git for Windows 自带的 6.3 会以晦涩错误失败，这里提前拦
if command -v g++ >/dev/null 2>&1; then
  gcc_major="$(g++ -dumpversion | cut -d. -f1)"
  [ "$gcc_major" -ge 10 ] || fail "g++ $gcc_major 太老（需 ≥ 10，C++20）；把 MinGW 的 bin 放进 PATH（或设 MINGW_BIN）"
fi

GEN="Ninja"
command -v ninja >/dev/null 2>&1 || GEN="Unix Makefiles"

echo "==> 仓库:    $ROOT"
echo "==> 构建目录: $BUILD_DIR"
echo "==> 生成器:  $GEN"
echo "==> Qt:      ${QT_PREFIX:-（无 Qt 路径）}"
echo "==> 模式:    $([ "$CORE_ONLY" -eq 1 ] && echo 'core（无 Qt）' || echo '全量 + GUI')$([ "$QUICK" -eq 1 ] && echo ' / 快速')"

START="$(date +%s)"

# ---- 1. 配置 ----
if [ "$REUSE" -eq 0 ] || [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  step "配置"
  cmake_args=(-S . -B "$BUILD_DIR" -G "$GEN" -DCMAKE_BUILD_TYPE=Debug
    -DBEATBENCH_BUILD_TESTS=ON
    -DBEATBENCH_BUILD_APP="$([ "$CORE_ONLY" -eq 1 ] && echo OFF || echo ON)"
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
  # ↑ CMAKE_POLICY_VERSION_MINIMUM：CMake 4 需要它放行 PortAudio v19.7.0 的 cmake_minimum_required(2.8)；
  #   根 CMakeLists 里也钳了一份（PR #1），这里传一份保证 master 上也能跑。
  [ "$CORE_ONLY" -eq 0 ] && cmake_args+=(-DCMAKE_PREFIX_PATH="$QT_PREFIX")
  cmake "${cmake_args[@]}"
else
  step "复用已有构建目录（跳过 configure）"
fi

# ---- 2. 构建 ----
step "构建"
cmake --build "$BUILD_DIR" --parallel

# ---- 3. 测试 ----
step "测试$([ "$QUICK" -eq 1 ] && echo '（BB_SKIP_REAL=1）')"
if [ "$QUICK" -eq 1 ]; then
  BB_SKIP_REAL=1 ctest --test-dir "$BUILD_DIR" --output-on-failure
else
  ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

# ---- 4. GUI 无头冒烟 ----
if [ "$CORE_ONLY" -eq 0 ] && [ "$NO_SMOKE" -eq 0 ]; then
  step "GUI 无头冒烟"
  EXE=""
  for cand in "$BUILD_DIR/app/beatbench.exe" "$BUILD_DIR/app/beatbench"; do
    [ -f "$cand" ] && EXE="$cand" && break
  done
  # macOS：产物在 .app 里，exe 同级没有 BeatBench QML 模块目录 → 显式给导入路径
  if [ -d "$BUILD_DIR/app/beatbench.app" ]; then
    EXE="$BUILD_DIR/app/beatbench.app/Contents/MacOS/beatbench"
    export QML2_IMPORT_PATH="$BUILD_DIR/app"
  fi
  [ -n "$EXE" ] || fail "找不到 GUI 可执行文件（构建没产出 app 目标？Qt 没找到？）"

  OUTDIR="$ROOT/local/tmp/check"
  mkdir -p "$OUTDIR"
  SHOT="$OUTDIR/smoke.png"
  LOG="$ROOT/beatbench-qml-errors.log"   # main.cpp 固定写到 CWD（仓库根，已 gitignore）
  rm -f "$SHOT" "$LOG"

  export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software
  "$EXE" --screenshot "$SHOT" --apply-skin Aurora >/dev/null 2>&1 || true

  [ -f "$SHOT" ] || { tail -20 "$LOG" 2>/dev/null >&2 || true; fail "冒烟：没有生成截图（日志：$LOG）"; }
  grep -q "皮肤已运行时切换：skins/Aurora" "$LOG" 2>/dev/null ||
    { tail -20 "$LOG" 2>/dev/null >&2 || true; fail "冒烟：日志里没有皮肤运行时切换（皮肤路径解析回归？）"; }
  # Qt 6.11 曾出现「attached properties must be accessed through a direct child of SplitView」：
  # 这类 QML 作用域问题只报警告、不影响启动，容易被漏掉，这里钉死。
  if grep -q "attached properties must be accessed" "$LOG" 2>/dev/null; then
    grep -n "attached properties must be accessed" "$LOG" >&2
    fail "冒烟：QML 附加属性作用域告警（见上）"
  fi
  echo "    截图: $SHOT（$(wc -c <"$SHOT") 字节）"
  echo "    QML 日志无附加属性告警 ✓"
  mv -f "$LOG" "$OUTDIR/" 2>/dev/null || true
fi

ELAPSED=$(( $(date +%s) - START ))
echo
echo "✅ 门禁通过（${ELAPSED}s）"
