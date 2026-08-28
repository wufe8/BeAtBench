# OsuLight 内置皮肤（L1 亮色）

新增的**亮色**内置皮肤，对应 doc/beatbench-ui-styles.html 主题② osu!lazer 方向：
浅底 + 大圆角 + 多彩 note。初衷：默认 / Aurora / Linear 都偏深色、Linear 与默认观感接近，
皮肤菜单缺一个「一眼不同」的选项——OsuLight 补上（浅色）。

## 效果

- `--skin skins/OsuLight`（或菜单「视图→皮肤→OsuLight」运行时切换）：整壳换浅色
  （浅底 white/ff 面板 + 粉 primary + 蓝 accent + 多彩 note）；
- 控件形状细分示范：`buttonRadius`=8（按钮圆角）/ `boxRadius`=6（复选框/输入框圆角）/
  `radiusSm`=10（面板/hover）/ `radius`=12 / `noteRadius`=4——四种圆角**独立可调**。

## 文件

| 文件 | 作用 |
|---|---|
| `skin.json` | 皮肤清单：name/version/api（layers L1） |
| `theme.json` | L1 token：颜色（浅色）+ 非颜色（radius/fs 密度/独立按钮/复选框圆角） |
| `keymap.json` | 快捷键覆写（另存为=Ctrl+Shift+S、更多轨道=Ctrl+Shift+E） |

## 应用方式

```
beatbench.exe --skin skins/OsuLight
# 或运行时：菜单 视图 → 皮肤 → OsuLight
```

## 说明

- 全部颜色/形状走 ThemeManager token（QML 0 硬编码 hex，见 9b5fc65）；浅色下 BbSpinBox 箭头走
  `Theme.text`（可见）。
- 运行时切换与 `--skin` 启动切换同一路径（applyTheme → tokensChanged → QPalette 重建 + 重绘）。
