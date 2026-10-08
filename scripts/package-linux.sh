#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Linux 发行版包组装（deb / rpm / arch）：在目标发行版容器内运行（package.yml 调用）。
#
# 设计：文件布局单一来源 = app/CMakeLists 的 install 规则；本脚本只做
#   cmake Release 构建 → DESTDIR staging → 按格式加元数据（control / spec / PKGBUILD）。
# 产物按发行版惯例命名（内嵌版本/发行号/架构，便于 dpkg -i / dnf install / pacman -U）；
# workflow 以中性 artifact 名（beatbench-linux-deb 等）分发。
#
# 用法（容器内，通常 root）:
#   bash scripts/package-linux.sh <deb|rpm|arch> [--skip-deps]
# 环境变量:
#   SRC      源码根（默认脚本上级目录）
#   OUT      产物目录（默认 $PWD/out-linux；容器里一般设 OUT=/out 并挂载）
#   BUILD    构建目录（默认 /tmp/beatbench-build）
#   PKGROOT  staging 根（默认 /tmp/beatbench-pkgroot）
set -euo pipefail

SRC="${SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
FORMAT="${1:-}"
case "$FORMAT" in
  deb|rpm|arch) ;;
  *) echo "用法: $0 <deb|rpm|arch> [--skip-deps]" >&2; exit 2 ;;
esac
shift
SKIP_DEPS=0
for a in "$@"; do
  case "$a" in
    --skip-deps) SKIP_DEPS=1 ;;
    *) echo "未知参数: $a" >&2; exit 2 ;;
  esac
done

OUT="${OUT:-$(pwd)/out-linux}"
BUILD="${BUILD:-/tmp/beatbench-build}"
PKGROOT="${PKGROOT:-/tmp/beatbench-pkgroot}"
# 占位维护者：上游 GitHub 身份（公开邮箱未知）。上游可随时替换为正式维护者。
MAINTAINER="wufe8 <wufe8@users.noreply.github.com>"

VER="$(grep -m1 -oE 'VERSION [0-9]+\.[0-9]+\.[0-9]+' "$SRC/CMakeLists.txt" | cut -d' ' -f2)"
[ -n "$VER" ] || { echo "错误: 无法从 CMakeLists.txt 读取版本号" >&2; exit 1; }
DEB_ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
RPM_ARCH="$(uname -m)"
[ "$RPM_ARCH" = "x86_64" ] || { echo "错误: 仅支持 x86_64（计划已定），当前 $RPM_ARCH" >&2; exit 1; }

echo "==> package-linux: 格式=$FORMAT 版本=$VER"

install_build_deps() {
  if [ "$SKIP_DEPS" = 1 ]; then echo "==> 跳过构建依赖安装（--skip-deps）"; return; fi
  if [ -f /etc/debian_version ]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates dpkg-dev \
      qt6-base-dev qt6-declarative-dev qt6-declarative-dev-tools \
      qml6-module-qtqml qml6-module-qtqml-workerscript \
      qml6-module-qtquick qml6-module-qtquick-controls \
      qml6-module-qtquick-layouts qml6-module-qtquick-templates \
      qml6-module-qtquick-window qml6-module-qtquick-dialogs \
      libasound2-dev
  elif [ -f /etc/fedora-release ]; then
    dnf install -y -q gcc-c++ cmake ninja-build git rpm-build \
      qt6-qtbase-devel qt6-qtdeclarative-devel alsa-lib-devel
  elif [ -f /etc/arch-release ]; then
    pacman -Sy --noconfirm --needed base-devel cmake ninja git qt6-base qt6-declarative alsa-lib
  else
    echo "错误: 未知发行版（需 debian/fedora/arch 系），无法安装构建依赖" >&2
    exit 1
  fi
}

build_and_stage() {
  rm -rf "$BUILD" "$PKGROOT"
  cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBEATBENCH_BUILD_TESTS=OFF -DBEATBENCH_BUILD_APP=ON \
    -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build "$BUILD" --target beatbench beatbench-cli --parallel
  DESTDIR="$PKGROOT" cmake --install "$BUILD"
}

stage_assert() {
  local missing=0 f
  for f in \
    usr/bin/beatbench \
    usr/bin/beatbench-cli \
    usr/share/beatbench/skins/Aurora/theme.json \
    usr/share/applications/beatbench.desktop \
    usr/share/icons/hicolor/256x256/apps/beatbench.png \
    usr/share/metainfo/io.github.wufe8.BeAtBench.metainfo.xml \
    usr/share/licenses/beatbench/LICENSE ; do
    [ -e "$PKGROOT/$f" ] || { echo "错误: staging 缺少 $f" >&2; missing=1; }
  done
  find "$PKGROOT" -path '*/qt6/qml/BeatBench/qmldir' | grep -q . \
    || { echo "错误: staging 缺少 QML 模块 qmldir" >&2; missing=1; }
  [ "$missing" = 0 ] || exit 1
}

make_deb() {
  # Debian 政策：/usr/share/doc/<pkg>/copyright
  install -Dm644 "$SRC/LICENSE" "$PKGROOT/usr/share/doc/beatbench/copyright"

  # shlibs 依赖（含符号版本约束）。dpkg-shlibdeps 需要一个 debian/control 定位包；
  # 这里只是临时壳，真正的 control 随后写进包根 DEBIAN/。
  mkdir -p "$BUILD/debian" "$PKGROOT/DEBIAN"
  cat > "$BUILD/debian/control" <<EOF
Source: beatbench
Section: sound
Priority: optional
Maintainer: $MAINTAINER
Standards-Version: 4.6.2

Package: beatbench
Architecture: any
Depends: \${shlibs:Depends}
Description: BMS chart editor
 Placeholder control used only to run dpkg-shlibdeps.
EOF
  local shlibs
  shlibs="$(cd "$BUILD" && dpkg-shlibdeps -O \
      -e"$PKGROOT/usr/bin/beatbench" \
      -e"$PKGROOT/usr/bin/beatbench-cli" | sed 's/^shlibs:Depends=//')"

  # t64 备选：Debian 13+/Ubuntu 24.04+ 把部分库改名为 <lib>t64（time_t 迁移）。
  # 每个依赖附加 t64 备选；未改名的库在原发行版上第一备选即满足，额外备选无副作用。
  local deps
  deps="$(printf '%s\n' "$shlibs" | tr ',' '\n' \
    | sed -E 's/^[[:space:]]*([^ ]+)( \(.*\))?$/\1\2 | \1t64\2/' \
    | paste -sd, -)"
  # dlopen 的运行时依赖（ELF shlibs 不含）：QML 模块
  deps="$deps, qml6-module-qtqml, qml6-module-qtqml-workerscript, qml6-module-qtquick, qml6-module-qtquick-controls, qml6-module-qtquick-dialogs, qml6-module-qtquick-layouts, qml6-module-qtquick-templates, qml6-module-qtquick-window"

  mkdir -p "$PKGROOT/DEBIAN"
  cat > "$PKGROOT/DEBIAN/control" <<EOF
Package: beatbench
Version: $VER-1
Architecture: $DEB_ARCH
Maintainer: $MAINTAINER
Installed-Size: $(du -sk "$PKGROOT" | cut -f1)
Depends: $deps
Recommends: qt6-wayland
Suggests: fonts-noto-cjk | fonts-wqy-zenhei
Section: sound
Priority: optional
Homepage: https://github.com/wufe8/BeAtBench
Description: BMS chart editor
 BeAtBench is a cross-platform BMS chart editor: notes/long notes/mines,
 timing (BPM/STOP), meta information, samples, BGA, slice/keysound
 workflow, audio preview and lint checks.
EOF

  mkdir -p "$OUT"
  local file="beatbench_${VER}-1_${DEB_ARCH}.deb"
  dpkg-deb --build --root-owner-group "$PKGROOT" "$OUT/$file"
  (cd "$OUT" && sha256sum "$file" > "$file.sha256")
  echo "==> 产物: $OUT/$file"
}

make_rpm() {
  local top="$BUILD/rpmbuild"
  rm -rf "$top"
  mkdir -p "$top"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
  cat > "$top/SPECS/beatbench.spec" <<EOF
%global _stage_dir $PKGROOT

Name:           beatbench
Version:        $VER
Release:        1%{?dist}
Summary:        BMS chart editor
License:        GPL-3.0-only
URL:            https://github.com/wufe8/BeAtBench
BuildArch:      x86_64
Requires:       qt6-qtbase
Requires:       qt6-qtdeclarative
Suggests:       qt6-qtwayland
Suggests:       google-noto-sans-cjk-fonts
Packager:       $MAINTAINER

%description
BeAtBench is a cross-platform BMS chart editor: notes/long notes/mines, timing
(BPM/STOP), meta information, samples, BGA, slice/keysound workflow, audio
preview and lint checks.

%prep
%build
%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
cp -a %{_stage_dir}/. %{buildroot}/

%files
/usr/bin/beatbench
/usr/bin/beatbench-cli
%{_libdir}/qt6/qml/BeatBench
/usr/share/beatbench
/usr/share/applications/beatbench.desktop
/usr/share/doc/beatbench/README.md
/usr/share/doc/beatbench/CHANGELOG.md
/usr/share/icons/hicolor/*/apps/beatbench.png
/usr/share/licenses/beatbench/LICENSE
/usr/share/metainfo/io.github.wufe8.BeAtBench.metainfo.xml

%changelog
* $(date '+%a %b %d %Y') $MAINTAINER - $VER-1
- Build from CI package pipeline (prebuilt staging tree).
EOF
  rpmbuild -bb --define "_topdir $top" "$top/SPECS/beatbench.spec"
  local rpmfile
  rpmfile="$(find "$top/RPMS" -name '*.rpm' | head -1)"
  [ -n "$rpmfile" ] || { echo "错误: rpmbuild 未产出包" >&2; exit 1; }
  mkdir -p "$OUT"
  cp "$rpmfile" "$OUT/"
  (cd "$OUT" && sha256sum "$(basename "$rpmfile")" > "$(basename "$rpmfile").sha256")
  echo "==> 产物: $OUT/$(basename "$rpmfile")"
}

make_arch() {
  local work="$BUILD/arch"
  rm -rf "$work"
  mkdir -p "$work"
  cat > "$work/PKGBUILD" <<EOF
# Maintainer: $MAINTAINER
pkgname=beatbench
pkgver=$VER
pkgrel=1
pkgdesc='BMS chart editor'
arch=('x86_64')
url='https://github.com/wufe8/BeAtBench'
license=('GPL-3.0-only')
depends=('qt6-base' 'qt6-declarative' 'alsa-lib' 'hicolor-icon-theme')
optdepends=('qt6-wayland: Wayland 平台插件' 'noto-fonts-cjk: 中文界面字体')
options=('!debug')
source=()
sha256sums=()

package() {
  cp -a "$PKGROOT/." "\$pkgdir/"
}
EOF
  chmod 644 "$work/PKGBUILD"

  local runner=()
  if [ "$(id -u)" = 0 ]; then
    # makepkg 禁止以 root 运行：容器内建 builder 用户并让出目录
    id builder >/dev/null 2>&1 || useradd -m builder
    chown -R builder:builder "$work"
    runner=(runuser -u builder --)
  fi
  (cd "$work" && "${runner[@]}" makepkg --noconfirm --force)

  local pkgfile
  pkgfile="$(find "$work" -maxdepth 1 -name '*.pkg.tar.zst' | head -1)"
  [ -n "$pkgfile" ] || { echo "错误: makepkg 未产出包" >&2; exit 1; }
  mkdir -p "$OUT"
  cp "$pkgfile" "$OUT/"
  (cd "$OUT" && sha256sum "$(basename "$pkgfile")" > "$(basename "$pkgfile").sha256")
  echo "==> 产物: $OUT/$(basename "$pkgfile")"
}

install_build_deps
build_and_stage
stage_assert
case "$FORMAT" in
  deb) make_deb ;;
  rpm) make_rpm ;;
  arch) make_arch ;;
esac
echo "==> 完成，产物目录内容:"
ls -1 "$OUT"
