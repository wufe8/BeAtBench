# Aurora 样例皮肤（L1）

最低可用的 L1 皮肤样例：只覆写 **theme.json**（颜色 token）+ **keymap.json**（快捷键）。
对应 doc/08 §3 分层皮肤系统的 L1（token 覆写）层；L2（layout.json）/ L3（QML 壳）未做。

## 效果

- `--skin skins/Aurora` 启动：暖色强调（amber）+ 深靛底 + 琥珀主色；
- 20 个颜色 token 被 `ThemeManager::loadTheme` 覆写（启动期、QML 加载前）；
- 2 个快捷键覆写（Ctrl+Shift+A=另存为、Ctrl+E=更多轨道）。

## 文件

| 文件 | 作用 |
|---|---|
| `skin.json` | 皮肤清单：name/version/api/author/layers/provides |
| `theme.json` | L1 token 覆写（颜色；支持 `#RRGGBB` / `#AARRGGBB`） |
| `keymap.json` | 动作 id → 快捷键文本（与皮肤无关，需 skin 携带时也可独立 `--keymap`） |

## 应用方式

```
beatbench.exe --skin skins/Aurora
```

加载优先级：`--keymap <path>` 显式 > `--skin dir/keymap.json` > 内置默认。
`theme.json` 只在 `--skin` 时读取；不加 `--skin` 完全用内置默认皮肤（行为不变）。

## 已知边界

- L1 只覆写**颜色** token；非颜色（radius/fs/fonts）与 L2 layout/L3 QML 壳后置（doc/08 §6）；
- `Theme.token` 是 `CONSTANT` 属性（QML 首帧按当前值求值），所以 theme.json 必须在
  `loadFromModule` 前应用——`main.cpp` 已如此（`--skin` 在构建 QPalette 前 `loadTheme`）；
- 皮肤**只影响表现层**；core/命令接口不受影响（doc/08 §3.3：功能永远在引擎+默认皮肤兜底）。
