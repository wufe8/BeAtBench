#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# 组合验证：把声明的分支按顺序合到 <remote>/master 的临时 worktree，构建、ctest、
# 可选 GUI 冒烟。用于 PR 不改 base 时的补偿控制，不替代单 PR 的 CI。
#
# 用法:
#   scripts/check-integration.sh [branch ...]
#   INTEGRATION_BRANCHES="branch-a branch-b" scripts/check-integration.sh
#   INTEGRATION_REMOTE=brightmeows scripts/check-integration.sh   # 分支在 fork 上
#   BEATBENCH_INTEGRATION_APP=0 scripts/check-integration.sh   # 无 Qt，只构建 core/tests
#
# 说明:
# - 分支列表必须显式传入；不维护默认列表，避免分支删除后静默跑错链。
# - 合并顺序 = 参数顺序；冲突直接失败，不自动解决。
# - 临时 worktree 在退出时删除；需要现场时用 --keep。
# - GUI 冒烟需要 QT_PREFIX；无 Qt 时显式设 BEATBENCH_INTEGRATION_APP=0。
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"
REMOTE="${INTEGRATION_REMOTE:-origin}"

KEEP=0
if [ "${1:-}" = "--keep" ]; then
    KEEP=1
    shift
fi

if [ "$#" -gt 0 ]; then
    branches=("$@")
elif [ -n "${INTEGRATION_BRANCHES:-}" ]; then
    read -r -a branches <<< "$INTEGRATION_BRANCHES"
else
    echo "缺少分支列表：传分支参数或设置 INTEGRATION_BRANCHES" >&2
    exit 2
fi

if [ "${#branches[@]}" -eq 0 ]; then
    echo "没有要合并的分支" >&2
    exit 2
fi

if ! git fetch "$REMOTE" --prune; then
    echo "fetch 失败: $REMOTE（拒绝用旧引用继续）" >&2
    exit 1
fi

tmp_base="$(mktemp -d "${TMPDIR:-/tmp}/beatbench-integration.XXXXXX")"
tmp="$tmp_base/wt"
cleanup() {
    if [ "$KEEP" -eq 1 ]; then
        echo "==> 保留临时 worktree: $tmp" >&2
        return
    fi
    cd "$ROOT"
    git worktree remove --force "$tmp" >/dev/null 2>&1 || true
    rm -rf "$tmp_base"
}
trap cleanup EXIT

echo "==> 临时 worktree: $tmp"
git worktree add --detach "$tmp" "$REMOTE/master" >/dev/null
cd "$tmp"

for b in "${branches[@]}"; do
    if ! git rev-parse --verify --quiet "$REMOTE/$b" >/dev/null; then
        echo "❌ 分支不存在: $REMOTE/$b" >&2
        exit 1
    fi
    echo "==> merge $b"
    if ! git -c user.name="beatbench-integration" -c user.email="beatbench-integration@local" \
        merge --no-ff --no-edit "$REMOTE/$b"; then
        echo "❌ 合并冲突: $b（用 --keep 保留现场）" >&2
        exit 1
    fi
done

build_dir="$tmp/build-integration"
app=ON
if [ "${BEATBENCH_INTEGRATION_APP:-1}" = "0" ]; then
    app=OFF
fi

cmake_args=(
    -S . -B "$build_dir" -G Ninja
    -DCMAKE_BUILD_TYPE=Debug
    -DBEATBENCH_BUILD_TESTS=ON
    -DBEATBENCH_BUILD_APP="$app"
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
)

if [ "$app" = "ON" ]; then
    if [ -z "${QT_PREFIX:-}" ]; then
        echo "❌ 需要 QT_PREFIX 才能构建 GUI；或设 BEATBENCH_INTEGRATION_APP=0" >&2
        exit 1
    fi
    cmake_args+=(-DCMAKE_PREFIX_PATH="$QT_PREFIX")
    export PATH="$QT_PREFIX/bin:$PATH"
fi

cmake "${cmake_args[@]}"
cmake --build "$build_dir" --parallel
BB_SKIP_REAL=1 ctest --test-dir "$build_dir" --output-on-failure

if [ "$app" = "ON" ]; then
    exe="$build_dir/app/beatbench"
    if [ ! -x "$exe" ]; then
        echo "❌ 找不到 GUI 可执行文件: $exe" >&2
        exit 1
    fi
    shot="$tmp/integration-smoke.png"
    log="$tmp/beatbench-qml-errors.log"
    rm -f "$shot" "$log"
    (
        cd "$tmp"
        QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
            QML2_IMPORT_PATH="$build_dir/app" \
            "$exe" --screenshot "$shot" --apply-skin Aurora >/dev/null 2>&1 || true
    )
    if [ ! -f "$shot" ]; then
        echo "❌ 冒烟截图缺失: $shot" >&2
        exit 1
    fi
    if ! grep -q "皮肤已运行时切换：" "$log" 2>/dev/null; then
        echo "❌ 冒烟日志缺皮肤切换: $log" >&2
        exit 1
    fi
    echo "==> GUI 冒烟通过"
fi

echo "==> 集成检查通过：${branches[*]}"
