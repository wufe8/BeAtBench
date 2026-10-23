#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# 格式检查：clang-format 全量 dry-run（CI format job 与 scripts/check.sh 共用同一入口）。
#
#   - 排除清单以根目录 .clang-format-ignore 为准（third_party 与生成文件 sjis_table.hpp），
#     此处再显式过滤一遍，保证不依赖 clang-format 对 ignore 文件的解析。
#   - 版本钉在 23.1.3（与 CI 一致）：不同版本的格式决策有漂移，本机版本不符时只警告
#     不拦截（避免开发者环境差异造成假红）；CLANG_FORMAT=<路径> 可覆盖二进制。
set -euo pipefail

cd "$(dirname "$0")/.."

CF="${CLANG_FORMAT:-clang-format}"
PINNED="23.1.3"

if ! command -v "$CF" >/dev/null 2>&1; then
  echo "❌ 找不到 clang-format（CLANG_FORMAT=<路径> 可指定；CI 用 pipx install clang-format==$PINNED）" >&2
  exit 1
fi

ver="$("$CF" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 || true)"
if [ "$ver" != "$PINNED" ]; then
  echo "⚠️  clang-format $ver ≠ 钉版 $PINNED：格式结果可能有漂移（以 CI 为准）" >&2
fi

violations="$(mktemp /tmp/check-format.XXXXXX)"
trap 'rm -f "$violations"' EXIT

# git ls-files -z 输出 NUL 分隔（bash 变量存不了 NUL，所以必须整段走管道）；
# grep -zv 显式排除 ignore 清单，|| true 兜住「无文件可查」的合法空集
if ! git ls-files -z '*.cpp' '*.hpp' |
  { grep -zv -e '^third_party/' -e '^core/src/bms/sjis_table\.hpp$' || true; } |
  xargs -0 -r "$CF" --dry-run -Werror 2>"$violations"; then
  echo "❌ 格式检查未通过（修复：git ls-files -z '*.cpp' '*.hpp' | xargs -0 clang-format -i）" >&2
  head -20 "$violations" >&2
  exit 1
fi
