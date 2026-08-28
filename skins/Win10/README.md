# Win10 内置皮肤（L1 直角/扁平风）

Windows 10 风格 L1 皮肤：扁平 + **直角**（radius/buttonRadius/boxRadius/noteRadius 全 0）+
中性灰底 + Win10 蓝 accent + Segoe UI 字体。与其他全圆角皮肤（默认/Aurora/Linear/OsuLight）
形成**直角对比**，打破「三个皮肤全在圆角」的单一性。

## 效果

- `--skin skins/Win10`（或菜单「视图→皮肤→Win10」）：整壳改 Win10 扁平直角风；
- 直角示范：所有控件圆角 0（按钮/复选框/输入框/note 全直角）、面板圆角 0；
- 扁平风：surface2/surface3 中性灰、边框细、无圆角，Win10 蓝 primary/accent。

## 文件

| 文件 | 作用 |
|---|---|
| `skin.json` | 皮肤清单：name/version/api（layers L1） |
| `theme.json` | L1 token：颜色（中性灰+Win10 蓝）+ 非颜色（全 0 圆角、Segoe UI 字体、fs 密度） |
| `keymap.json` | 快捷键覆写（另存为=Ctrl+Shift+S、更多轨道=Ctrl+Shift+E） |

## 应用方式

```
beatbench.exe --skin skins/Win10
# 或运行时：菜单 视图 → 皮肤 → Win10
```

## 说明

- 圆角全 0 走 token（radiusSm/buttonRadius/boxRadius/noteRadius），非硬编码——L1 皮肤即可切换直角风。
- 字体 Segoe UI / Cascadia Mono（Win 自带；缺省回退由 Qt 处理）。
