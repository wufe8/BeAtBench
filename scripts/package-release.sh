#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# 一键发布打包（Windows 64 位，Git Bash 下运行；doc/07 §2 发布部署说明）
#
# 步骤：
#   1. Release 构建（build-release）
#   2. staging 目录：exe + windeployqt 部署 + 应用 QML 模块 + 运行时/翻译/文档
#   3. 冒烟（staging 内自检）
#   4. staging 原子改名为 dist（out/beatbench-v<ver>-win64）
#   5. zip + sha256
#
# 用法:
#   scripts/package-release.sh [--skip-smoke]
#
# 环境变量（必填，脚本不再内置本机路径默认值）:
#   QT_ROOT       Qt 安装目录
#   CXX_BIN       MinGW 编译器 g++.exe 路径
#   NINJA_BIN     Ninja 可执行文件路径
# 可选环境变量:
#   BB_SMOKE_OPEN 冒烟时打开的谱面路径（例如 <你的谱面>.bms）
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

# ---- 可配置项 ----
QT_ROOT="${QT_ROOT:-}"
CXX_BIN="${CXX_BIN:-}"
NINJA_BIN="${NINJA_BIN:-}"

[ -n "$QT_ROOT" ] || { echo "错误: 请设置 QT_ROOT（Qt 安装目录）" >&2; exit 1; }
[ -n "$CXX_BIN" ] || { echo "错误: 请设置 CXX_BIN（MinGW g++.exe 路径）" >&2; exit 1; }
[ -n "$NINJA_BIN" ] || { echo "错误: 请设置 NINJA_BIN（Ninja 路径）" >&2; exit 1; }
BUILD="$ROOT/build-release"
OUT="$ROOT/out"

VER="$(grep -m1 -oE 'VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | cut -d' ' -f2)"
[ -n "$VER" ] || { echo "错误: 无法从 CMakeLists.txt 读取版本号" >&2; exit 1; }
DIST="beatbench-v${VER}-win64"

SMOKE=1
for a in "$@"; do
  [ "$a" = "--skip-smoke" ] && SMOKE=0
done

[ -d "$QT_ROOT" ] || { echo "错误: Qt 根不存在: $QT_ROOT（可用 QT_ROOT= 覆盖）" >&2; exit 1; }

echo "==> 版本: $VER"
echo "==> Qt:   $QT_ROOT"

# ---- 1. 配置（首次）+ 构建 ----
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  echo "==> 配置 $BUILD (Release/Ninja)"
  # CMake >= 4.0 移除 <3.5 兼容；portaudio(FetchContent).CMakeLists 仍写 VERSION 2.8，需显式放行。
  cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_ROOT" \
    -DCMAKE_CXX_COMPILER="$CXX_BIN" \
    -DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
    -DBEATBENCH_BUILD_TESTS=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
fi
echo "==> 构建 beatbench / beatbench-cli"
cmake --build "$BUILD" --target beatbench beatbench-cli --parallel

# ---- 2. staging 组装（事务性：全部在 staging 完成，最后原子改名到 dist）----
STAGE="$OUT/.pkg-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"

copystep() {
  echo "  [copy] $1 -> $2"
  cp "$1" "$2"
}

echo "==> 组装 $DIST（staging）"
copystep "$BUILD/app/beatbench.exe" "$STAGE/beatbench.exe"
copystep "$BUILD/cli/beatbench-cli.exe" "$STAGE/beatbench-cli.exe"

echo "==> windeployqt 部署 Qt 运行时"
"$QT_ROOT/bin/windeployqt.exe" --release --qmldir "$ROOT/app/qml" \
  --no-opengl-sw --no-system-d3d-compiler --no-compiler-runtime \
  "$STAGE/beatbench.exe" >/dev/null
# windeployqt 偶发把 exe 复制/改名成与 QML 模块同名文件（BeatBench，无扩展名），
# 且可能生成残留目录；此处一律清掉（应用模块稍后从构建目录重新拷贝）。
rm -rf "$STAGE/BeatBench"
# **无条件回拷 exe**：windeployqt 行为不稳定（同一命令不同会话结果不同），
# 构建产物永远在 build-release/app/，回拷保证 staging 一定有名副其实的 beatbench.exe。
cp -f "$BUILD/app/beatbench.exe" "$STAGE/beatbench.exe"
[ -f "$STAGE/beatbench.exe" ] || { echo "错误: windeployqt 后 beatbench.exe 丢失" >&2; exit 1; }

echo "==> 拷贝应用 QML 模块（BeatBench/）
   注意：windeployqt 只部署 Qt 自带 QML 模块；应用自己的 BeatBench 模块
   （qmldir + 类型注册 + qml/ 源码）在构建目录里，必须手动拷贝，否则引擎
   回退到资源 stub 报 \"Module 'BeatBench' contains no type named 'Main'\"。"
[ -d "$BUILD/app/BeatBench" ] || { echo "错误: $BUILD/app/BeatBench 不存在（构建未产出 QML 模块）" >&2; exit 1; }
# windeployqt 偶发把同名 BeatBench 留成 exe **文件**（见上）；cp -r 目标必须是目录，
# 此处再清理一次（文件/目录都 rm -rf），避免 "cannot overwrite non-directory"。
rm -rf "$STAGE/BeatBench"
cp -r "$BUILD/app/BeatBench" "$STAGE/BeatBench"

echo "==> 拷贝内置皮肤（skins/）
   皮肤（theme.json / keymap.json）不在 Qt 资源里，运行时由 ThemeManager 按工作目录
   相对路径 'skins/<Name>' 解析（skinDirResolved）；windeployqt 只部署 Qt 运行时，
   不会碰它。必须与 beatbench.exe 同级随包分发，否则「视图→皮肤」四项全部报
   \"缺 theme.json（皮肤目录未找到）\"（v0.1.0–v0.3.0 三版均漏，2026-09 修复）。"
[ -d "$ROOT/skins" ] || { echo "错误: $ROOT/skins 不存在（内置皮肤源目录缺失）" >&2; exit 1; }
rm -rf "$STAGE/skins"
cp -r "$ROOT/skins" "$STAGE/skins"
# 校验每个内置皮肤都有 theme.json（ThemeManager 的加载入口），防止空目录/半拷贝混进发布包。
skin_count=0
for d in "$STAGE"/skins/*/; do
  [ -d "$d" ] || continue
  [ -f "$d/theme.json" ] || { echo "错误: 内置皮肤缺少 theme.json: ${d#$STAGE/}" >&2; exit 1; }
  skin_count=$((skin_count + 1))
done
[ "$skin_count" -gt 0 ] || { echo "错误: skins/ 下没有任何皮肤目录" >&2; exit 1; }
echo "    已随包 $skin_count 个皮肤: $(cd "$STAGE/skins" && ls -d */ | tr -d '/' | tr '\n' ' ')"

echo "==> MinGW 运行库（libgcc/libstdc++/libwinpthread）"
for dll in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
  cp "$QT_ROOT/bin/$dll" "$STAGE/"
done

echo "==> 翻译裁剪（仅保留中文）"
(
  cd "$STAGE/translations"
  find . -name '*.qm' | grep -vE '^\./(qt_zh_CN|qt_zh_TW)\.qm$' | xargs --no-run-if-empty rm -f
  # main.cpp 运行时加载 qtbase_zh_CN（FileDialog 等内置控件）；windeployqt 不部署它，手动补
  cp "$QT_ROOT/translations"/qtbase_zh_CN.qm "$QT_ROOT/translations"/qtbase_zh_TW.qm \
     "$QT_ROOT/translations"/qtdeclarative_zh_CN.qm .
)

echo "==> 清理调试工具集（qmltooling 只在 QML 调试时用）"
rm -rf "$STAGE/qmltooling"

echo "==> README.txt / LICENSE"
cat > "$STAGE/README.txt" <<EOF
BeAtBench v$VER (M1-M6)
========================

一个基于 Qt 6 / QML 的 BMS 谱面编辑器（Windows 64 位，免安装）。

包含内容
--------
  beatbench.exe       图形界面编辑器（主程序，双击运行）
  beatbench-cli.exe   命令行工具（info / check / convert / version）
  skins/              内置皮肤（Aurora / Linear / OsuLight / Win10），菜单"视图 -> 皮肤"切换
  README.txt          本说明
  其余 DLL / qml /    运行所需的 Qt 运行时与插件，请勿删除

使用方法
--------
1. 双击 beatbench.exe 启动图形界面：
   - 打开谱面：菜单"文件 -> 打开"（或 Ctrl+O）
   - 也可直接把 .bms/.bml/.bme/.pms 拖进窗口（拖到 beatbench.exe 图标/双击关联文件同样可）
     音频(.wav/.ogg/.mp3/.flac) 与 MIDI(.mid/.midi) 拖入会自动进入"切音/对音"工作台
   - 编辑谱面：放置/选择/删除 note、LN、地雷，量化、镜像、旋转、复制粘贴（放置/移动有鼠标预览 ghost）
   - 时间轴：BPM / STOP 调整（右侧"时间轴"标签页）
   - 元信息、采样、BGA、lint 检查：左侧各标签页
   - 播放/试听：Space 播放/暂停，Ctrl+R 手动渲染；播放头红线 + A/B 循环 + 点秒标尺/右侧波形条 seek
2. 命令行工具（在资源管理器中打开本文件夹，再用终端运行）：
     beatbench-cli.exe info <谱面文件>
     beatbench-cli.exe check <谱面文件>
     beatbench-cli.exe convert <输入> <输出> [--encoding utf8|sjis]
     beatbench-cli.exe version

平台要求
--------
  Windows 10/11 64 位。无需安装，无需设置 PATH，所有 Qt 运行库已内置。

已知限制（详见项目 doc/04）
--------------------------
  - 音频波形需先渲染（Space / 载入自动渲染）一次才显示；编辑 note 会增量自动更新。
  - 切音工作台（M6）已可用；已知边界：#WAV 占用视图 UI、CSV 时标表、变速谱 BPM 自动待做。
  - 实时 keysound 调度（试玩）为后续 Phase D，尚未实现。
  - #RANDOM/#IF 块内容保存后不保证数据一致（已知限制，按需立项）。
  - 暂不支持 bmson 等其它谱面格式（v1 仅支持 .bms/.bme/.pms 文本）。

许可
----
  GPL-3.0（本程序源码见项目仓库 LICENSE 文件）。
  内置 Qt 6 运行时按 LGPL-3.0 分发（版本随构建所用 Qt；https://www.qt.io/licensing）。
EOF
cp "$ROOT/LICENSE" "$STAGE/LICENSE"

# ---- 3. 冒烟（staging 内自检）----
if [ "$SMOKE" = 1 ]; then
  echo "==> 冒烟：GUI 截图自检 + CLI version"
  # 冒烟前**无条件回拷**主 exe：windeployqt 多次偶发把 beatbench.exe 改名/移走成
  # BeatBench 文件；此处兜底保证 staging 与最终 zip 都有名副其实的 beatbench.exe。
  cp -f "$BUILD/app/beatbench.exe" "$STAGE/beatbench.exe"
  [ -x "$STAGE/beatbench.exe" ] || { echo "错误: staging 缺少可执行 beatbench.exe" >&2; exit 1; }
  ( cd "$STAGE" && ./beatbench.exe --screenshot "$OUT/.bb-smoke.png" \
      --apply-skin Aurora \
      ${BB_SMOKE_OPEN:+--open "$BB_SMOKE_OPEN"} ) || {
    echo "错误: GUI 启动失败，查看 $STAGE/beatbench-qml-errors.log" >&2; exit 1; }
  [ -f "$OUT/.bb-smoke.png" ] || { echo "错误: 冒烟截图未生成" >&2; exit 1; }
  # 皮肤冒烟：--apply-skin 走 ThemeManager::applySkinByName（与菜单「视图→皮肤」同路径）。
  # 皮肤未随包 / 工作目录解析不到时日志为「缺 theme.json（皮肤目录未找到）」+「应用失败」——
  # 此处硬失败，把「皮肤没打包进发布包」钉死在打包阶段（v0.3.0 漏打包即因缺此检查）。
  grep -q "皮肤已运行时切换：skins/Aurora" "$STAGE/beatbench-qml-errors.log" 2>/dev/null || {
    echo "错误: 内置皮肤未生效（skins/ 未随包或路径解析失败）；日志尾部：" >&2
    tail -5 "$STAGE/beatbench-qml-errors.log" 2>/dev/null >&2; exit 1; }
  ( cd "$STAGE" && ./beatbench-cli.exe version ) || { echo "错误: CLI 冒烟失败" >&2; exit 1; }
  rm -f "$OUT/.bb-smoke.png" "$STAGE/beatbench-qml-errors.log"
else
  echo "==> 跳过冒烟（--skip-smoke）"
fi

# ---- 4. staging 原子改名 -> dist ----
DIST_DIR="$OUT/$DIST"
rm -rf "$DIST_DIR"
mv "$STAGE" "$DIST_DIR"

# ---- 5. zip + sha256 ----
echo "==> 打包 zip"
if command -v zip >/dev/null 2>&1; then
  ( cd "$OUT" && rm -f "$DIST.zip" && zip -r -9 -q "$DIST.zip" "$DIST" )
elif command -v 7z >/dev/null 2>&1; then
  ( cd "$OUT" && rm -f "$DIST.zip" && 7z a -tzip -mx=9 "$DIST.zip" "$DIST" >/dev/null )
else
  echo "错误: 未找到 zip / 7z" >&2; exit 1
fi
( cd "$OUT" && sha256sum "$DIST.zip" > "$DIST.zip.sha256" )

echo "==> 完成"
echo "  ZIP:    $OUT/$DIST.zip"
echo "  目录:   $DIST_DIR/"
cat "$OUT/$DIST.zip.sha256"
