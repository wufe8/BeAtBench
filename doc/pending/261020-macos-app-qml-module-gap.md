# 挂账：macOS .app 缺 QML 模块目录，双击无法启动

## 背景

macOS CI 产物（`.app`）内不含 `BeatBench/` QML 模块目录，双击启动失败；
当前运行方式是 `QML2_IMPORT_PATH` 指向构建树（CI 无头冒烟同款）。
模块入包属 macOS 打包范畴（windeployqt 的对应物在 macOS 侧未实现）。

## 现状

- macOS 无预编译包；仅 CI 验证全量构建 + 单测 + GUI 无头冒烟（非阻塞门禁）。
- `.app` 打包、签名公证整体未做（doc/packaging.md 六、 边界）。

## 出口条件

macOS 打包立项时将 `BeatBench/` QML 模块目录纳入 `.app`（对齐 Windows 包的
内嵌策略），双击可启动后删除本条。
