#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Public tracked docs: worktree by default; --staged checks the index.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# PATH presence is insufficient (e.g. a broken WindowsApps alias).
probe='import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)'
for candidate in python3 python py; do
  interpreter=("$candidate")
  if [[ "$candidate" == py ]]; then interpreter+=(-3); fi
  if "${interpreter[@]}" -c "$probe" >/dev/null 2>&1; then
    export PYTHONUTF8=1
    exec "${interpreter[@]}" scripts/check-docs.py "$@"
  fi
done
echo "[check-docs] 需要可执行的 Python 3.8+（python3 / python / py -3）" >&2
exit 1
