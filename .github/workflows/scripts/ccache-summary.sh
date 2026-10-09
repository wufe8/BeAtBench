#!/usr/bin/env bash
# 把本次 run 的 ccache 统计写进 step summary（setup-ccache.sh 在 Build 前已 zero-stats）。
set -euo pipefail
if ! command -v ccache >/dev/null 2>&1; then
  echo "ccache 不可用，跳过统计" >&2
  exit 0
fi
{
  echo "### ccache（$RUNNER_OS）"
  echo ''
  echo '```'
  ccache --show-stats
  echo '```'
} >> "$GITHUB_STEP_SUMMARY"
