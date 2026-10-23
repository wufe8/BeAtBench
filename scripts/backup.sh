#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# 备份：给「只有一块盘」的工作流加一层异地副本。
#   ① 主仓库有未提交改动就先落一个 commit（即使不 push 也不丢）
#   ② 仅给主仓库打 git bundle 到备份目录（默认 OneDrive，无网也能跑）
#   本机 local/ 若是独立仓库，仅在其内部本地提交；本脚本不备份或推送它。
#   ③ 清理超过 --keep 份的旧 bundle
#   ④ --push 时额外 push 有远程的仓库；没远程的打印配置指引
#
# 用法:
#   scripts/backup.sh              # 提交未落盘改动 + 打 bundle 到 OneDrive
#   scripts/backup.sh --push       # 再 push 有远程的仓库
#   scripts/backup.sh --keep 20 --dir D:/backup
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

KEEP=10
DO_PUSH=0
if [ -n "${BACKUP_DIR:-}" ]; then
  DIR="$BACKUP_DIR"
else
  DIR=""
fi
while [ $# -gt 0 ]; do
  case "$1" in
    --push) DO_PUSH=1 ;;
    --keep) shift; KEEP="${1:-10}" ;;
    --keep=*) KEEP="${1#*=}" ;;
    --dir) shift; DIR="${1:-}" ;;
    --dir=*) DIR="${1#*=}" ;;
    -h | --help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
  shift
done

if [ -z "$DIR" ]; then
  for cand in "$HOME/OneDrive/BeAtBench-backup" "$HOME/Dropbox/BeAtBench-backup"; do
    d="$(dirname "$cand")"
    [ -d "$d" ] && DIR="$cand" && break
  done
fi
[ -n "$DIR" ] || DIR="$HOME/BeAtBench-backup"
mkdir -p "$DIR"

TS="$(date +%Y%m%d-%H%M%S)"
echo "==> 备份目录: $DIR"

backup_repo() {
  local path="$1" name="$2"
  [ -d "$path/.git" ] || { echo "  (跳过 $name：不是 git 仓库)"; return 0; }

  # ① 主仓库：有未提交改动就先落 commit（WIP 提交不寒碜）
  if [ -n "$(git -C "$path" status --porcelain)" ]; then
    git -C "$path" add -A
    git -C "$path" -c user.name="$(git -C "$path" config user.name || echo backup)" \
      -c user.email="$(git -C "$path" config user.email || echo backup@local)" \
      commit -q -m "wip: 自动备份 $TS"
    echo "  ✓ $name: 已提交未落盘改动"
  fi

  # ② bundle（异地副本，无网可用）
  local bundle="$DIR/$name-all-$TS.bundle"
  git -C "$path" bundle create -q "$bundle" --all
  echo "  ✓ $name: bundle $(du -h "$bundle" | cut -f1) → $(basename "$bundle")"

  # ③ 清理旧 bundle
  local old
  old="$(ls -1t "$DIR/$name-all-"*.bundle 2>/dev/null | tail -n +$((KEEP + 1)) || true)"
  if [ -n "$old" ]; then
    echo "$old" | while IFS= read -r f; do rm -f "$f"; done
    echo "  ✓ $name: 已清理 $(echo "$old" | wc -l) 个旧 bundle（保留最近 $KEEP 份）"
  fi

  # ④ push（可选）
  local remotes
  remotes="$(git -C "$path" remote)"
  if [ -z "$remotes" ]; then
    echo "  ⚠️  $name 没有配置远程 —— 现在只有本机 + 备份目录两份，建议加一个私有远程"
  elif [ "$DO_PUSH" -eq 1 ]; then
    local branch
    branch="$(git -C "$path" rev-parse --abbrev-ref HEAD)"
    git -C "$path" push -u "$(echo "$remotes" | head -1)" "$branch" && echo "  ✓ $name: 已 push $branch"
  fi
}

backup_repo "$ROOT" "main"

echo
echo "✅ 备份完成（$(ls -1 "$DIR" | wc -l) 个 bundle/文件在 $DIR）"
[ "$DO_PUSH" -eq 1 ] || echo "   提示：加 --push 可顺带推送有远程的仓库"
