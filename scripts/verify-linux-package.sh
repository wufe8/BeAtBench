#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Linux 包安装冒烟：在目标发行版容器内安装包，验证安装布局 + CLI + GUI（offscreen）。
# 刻意在空目录运行 GUI：皮肤走编译期数据目录兜底、QML 模块走系统导入目录，
# 确保不依赖 cwd（发行版安装的真实运行形态）。
#
# 用法: bash scripts/verify-linux-package.sh <deb|rpm|arch> <包文件或所在目录>
set -euo pipefail

FORMAT="${1:-}"
PKGARG="${2:-}"
[ -n "$FORMAT" ] && [ -n "$PKGARG" ] || { echo "用法: $0 <deb|rpm|arch> <包文件或目录>" >&2; exit 2; }

PKG="$PKGARG"
if [ -d "$PKG" ]; then
  case "$FORMAT" in
    deb) PKG="$(find "$PKG" -maxdepth 1 -name '*.deb' | head -1)" ;;
    rpm) PKG="$(find "$PKG" -maxdepth 1 -name '*.rpm' | head -1)" ;;
    arch) PKG="$(find "$PKG" -maxdepth 1 -name '*.pkg.tar.zst' | head -1)" ;;
    *) echo "错误: 未知格式 $FORMAT" >&2; exit 2 ;;
  esac
fi
[ -f "$PKG" ] || { echo "错误: 找不到包文件（$PKGARG）" >&2; exit 1; }
echo "==> verify-linux-package: 格式=$FORMAT 包=$PKG"

install_package() {
  case "$FORMAT" in
    deb)
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq
      apt-get install -y -qq --no-install-recommends "$PKG"
      # offscreen 平台插件（部分发行版单独分包；缺失时下面冒烟会失败）
      apt-get install -y -qq --no-install-recommends qt6-qpa-plugins 2>/dev/null || true
      ;;
    rpm)
      dnf install -y -q "$PKG"
      ;;
    arch)
      pacman -Sy --noconfirm >/dev/null
      pacman -U --noconfirm "$PKG"
      ;;
  esac
}

assert_installed_files() {
  test -f /usr/share/applications/beatbench.desktop
  test -f /usr/share/metainfo/io.github.wufe8.BeAtBench.metainfo.xml
  test -f /usr/share/beatbench/skins/Aurora/theme.json
  test -f /usr/share/licenses/beatbench/LICENSE
}

run_smoke() {
  echo "== CLI: beatbench-cli version"
  beatbench-cli version
  local work
  work="$(mktemp -d)"
  cd "$work"
  QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
    timeout 120 beatbench --screenshot smoke.png --apply-skin Aurora || true
  echo "== beatbench-qml-errors.log =="
  cat beatbench-qml-errors.log 2>/dev/null || echo "(无日志)"
  test -f smoke.png
  grep -q '皮肤已运行时切换：' beatbench-qml-errors.log
  grep -q '已运行时切换：.*Aurora' beatbench-qml-errors.log
}

install_package
assert_installed_files
run_smoke
echo "==> 安装冒烟通过: $PKG"
