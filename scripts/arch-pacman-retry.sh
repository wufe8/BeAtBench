#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# 由一次性 Arch 打包容器中的调用方 source；不改变快照、缓存或签名配置。

arch_pacman_retry() {
  local attempt status log delay
  for attempt in 1 2 3; do
    if log="$(LC_ALL=C pacman "$@" 2>&1)"; then
      printf '%s\n' "$log"
      return 0
    else
      status=$?
    fi
    printf '%s\n' "$log" >&2
    # 仅重试下载阶段：数据库同步或事务取件失败。签名校验、依赖解析和安装错误直接退出。
    if [ "$attempt" = 3 ] || ! grep -Fxq \
      -e 'error: failed to synchronize all databases (failed to retrieve some files)' \
      -e 'error: failed to commit transaction (failed to retrieve some files)' <<< "$log"; then
      return "$status"
    fi
    delay=$((attempt == 1 ? 3 : 10))
    echo "==> Arch 下载失败（第 $attempt/3 次），${delay} 秒后重试" >&2
    sleep "$delay"
  done
}
