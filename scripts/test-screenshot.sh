#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# 截图包装器回归：不需要 Qt，不碰真实谱面；临时输入/日志留在 local/tmp。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$ROOT/local/tmp" "$ROOT/local/screenshot"
WORK="$(mktemp -d "$ROOT/local/tmp/screenshot-tests-XXXXXX")"
SHOTS="$(mktemp -d "$ROOT/local/screenshot/screenshot-tests-XXXXXX")"
mkdir -p "$WORK/build/app"
cat >"$WORK/build/app/beatbench" <<'FAKE'
#!/usr/bin/env bash
set -euo pipefail
[ "$1" = --screenshot ]
OUT="$2"
case "${FAKE_MODE:?}" in
  success) printf '\211PNG\r\n\032\nfixture' >"$OUT" ;;
  crash) printf '\211PNG\r\n\032\nfixture' >"$OUT"; exit 7 ;;
  missing) : ;;
  empty) : >"$OUT" ;;
  invalid) printf 'not an image' >"$OUT" ;;
  timeout) sleep 30 ;;
esac
printf 'fake screenshot diagnostic\n'
[ "$PWD" = "${EXPECTED_ROOT:?}" ]
[ "$#" = 3 ] && [ "$3" = 'argument with spaces' ]
FAKE
chmod +x "$WORK/build/app/beatbench"
export BUILD_DIR="$WORK/build" EXPECTED_ROOT="$ROOT"
export SCREENSHOT_TIMEOUT_SECONDS=5
COUNT=0
run_case() {
  local mode="$1" expected="$2" rc=0 out="$SHOTS/$1 with spaces.png"
  # 故意留下旧图：失败时必须保留原图但返回失败，不能因原图存在判通过。
  printf 'old screenshot' >"$out"
  FAKE_MODE="$mode" bash "$ROOT/scripts/screenshot.sh" "$out" 'argument with spaces' >"$WORK/$mode.log" 2>&1 || rc=$?
  [ "$rc" -eq "$expected" ] || { echo "FAIL $mode: expected $expected, got $rc ($WORK/$mode.log)"; exit 1; }
  if [ "$expected" -eq 0 ]; then
    [ "$(od -An -tx1 -N8 "$out" | tr -d ' \n\r')" = 89504e470d0a1a0a ]
  else
    [ "$(<"$out")" = 'old screenshot' ]
  fi
  COUNT=$((COUNT + 1))
  echo "PASS $mode (exit $rc)"
}
# 从非仓库 cwd 调用，验证资源根目录与带空格参数。
cd "$WORK"
run_case success 0
run_case crash 7
run_case missing 1
run_case empty 1
run_case invalid 1
SCREENSHOT_TIMEOUT_SECONDS=1 run_case timeout 124
SCREENSHOT_TIMEOUT_SECONDS=invalid run_case success 2
BUILD_DIR="$WORK/not-built" run_case missing 1
# 模拟 BSD mktemp 的约束：模板必须以 X 结尾，不允许 GNU 风格后缀。
export REAL_MKTEMP="$(command -v mktemp)"
mkdir -p "$WORK/bsd-bin"
cat >"$WORK/bsd-bin/mktemp" <<'BSD'
#!/usr/bin/env bash
[[ "${!#}" =~ XXXXXX$ ]] || exit 2
exec "$REAL_MKTEMP" "$@"
BSD
chmod +x "$WORK/bsd-bin/mktemp"
PATH="$WORK/bsd-bin:$PATH" run_case success 0
# mv 到现有目录会移动进目录，而不是替换指定文件；必须提前拒绝。
mkdir -p "$SHOTS/existing directory.png"
RC=0
FAKE_MODE=success bash "$ROOT/scripts/screenshot.sh" "$SHOTS/existing directory.png" 'argument with spaces' >"$WORK/directory.log" 2>&1 || RC=$?
[ "$RC" -eq 2 ] || { echo "FAIL output directory: expected 2, got $RC"; exit 1; }
COUNT=$((COUNT + 1))
echo 'PASS output directory (exit 2)'
# 失败/成功后不留下中间图片，最终旧图/新图按上述规则保留供诊断。
shopt -s nullglob
PENDING=("$SHOTS"/.screenshot-*)
[ "${#PENDING[@]}" -eq 0 ]
echo "$COUNT screenshot wrapper checks passed; logs: $WORK"
