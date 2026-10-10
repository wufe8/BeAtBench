#!/usr/bin/env bash
# 安装钉版 ccache 并做本 run 的初始配置（三 Qt job 共用）；同时按平台规格导出
# 有界构建并行度 CMAKE_BUILD_PARALLEL_LEVEL。sha256 校验防供应链漂移；
# RUNNER_OS 由 runner 注入。
set -euo pipefail

CCACHE_VERSION=4.14.1
case "$RUNNER_OS" in
  Linux)   asset="ccache-${CCACHE_VERSION}-linux-x86_64-glibc.tar.xz"; sum="ad63d19f5d09ea13f749651653a561c98a59994e673504a4266617b8754218f7" ;;
  macOS)   asset="ccache-${CCACHE_VERSION}-darwin.tar.gz";             sum="c279fa81e2e806b4d9e64d4a06cb569a84ef54151feef796e5d4f4e95a464563" ;;
  Windows) asset="ccache-${CCACHE_VERSION}-windows-x86_64.zip";        sum="6219f3865ca59aec41ee4b678df171d5d35855ecb2b6dbbbd20690b3a68af7b4" ;;
  *) echo "未知 RUNNER_OS：${RUNNER_OS}" >&2; exit 1 ;;
esac

tmp="$(mktemp -d)"
url="https://github.com/ccache/ccache/releases/download/v${CCACHE_VERSION}/${asset}"
curl -fsSL -o "$tmp/asset" "$url"

case "$RUNNER_OS" in
  macOS) actual="$(shasum -a 256 "$tmp/asset" | awk '{print $1}')" ;;
  *)     actual="$(sha256sum "$tmp/asset" | awk '{print $1}')" ;;
esac
if [ "$actual" != "$sum" ]; then
  echo "sha256 不匹配：期望 ${sum}，实际 ${actual}" >&2
  exit 1
fi

dest="$HOME/.local/ccache-dist"
mkdir -p "$dest" "$HOME/.local/bin"
case "$asset" in
  # windows 自带 bsdtar（System32\tar.exe）可解 zip；Git Bash 无 unzip
  *.zip) "$SYSTEMROOT/System32/tar.exe" -xf "$tmp/asset" -C "$dest" ;;
  *)     tar -xf "$tmp/asset" -C "$dest" ;;
esac

find "$dest" -type f \( -name ccache -o -name ccache.exe \) -exec cp {} "$HOME/.local/bin/" \;
chmod +x "$HOME/.local/bin/"ccache*
case "$RUNNER_OS" in
  Windows) cygpath -w "$HOME/.local/bin" >> "$GITHUB_PATH" ;;
  *)       echo "$HOME/.local/bin" >> "$GITHUB_PATH" ;;
esac

# cache_dir 用各平台默认值（linux/mac ~/.cache/ccache，windows %LOCALAPPDATA%\ccache），
# 与 workflow 里 actions/cache 的 path 一一对应；统计口径只算本 run，故 Build 前清零
ccache --set-config max_size=512M
ccache --set-config compiler_check=content
ccache --zero-stats
"$HOME/.local/bin/ccache" --version

# 构建并行度：有界自动。cmake --build --parallel 不带值时读 CMAKE_BUILD_PARALLEL_LEVEL，
# 未设则向 make 传裸 -j（无上限）——147 个编译单元同起跑曾把 3 vCPU/7GB 的 macos 压到
# 27–35 分钟（run 37964482001）。按平台规格取 min(CPU数, 上限)：macos 3（GitHub 规格），
# 其余 4（runner 规格）
case "$RUNNER_OS" in
  macOS) cap=3 ;;
  *)     cap=4 ;;
esac
case "$RUNNER_OS" in
  Windows) raw="${NUMBER_OF_PROCESSORS:-}" ;;
  *)       raw="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || true)" ;;
esac
case "$raw" in
  ''|*[!0-9]*) raw=$cap ;;
esac
if [ "$raw" -gt "$cap" ]; then raw=$cap; fi
echo "CMAKE_BUILD_PARALLEL_LEVEL=$raw" >> "$GITHUB_ENV"
