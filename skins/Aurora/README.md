# Aurora 样例皮肤（L1）

最低可用的 L1 皮肤样例：只覆写 **theme.json**（颜色 token）+ **keymap.json**（快捷键）。
对应 doc/skin-system.md 三、 分层皮肤系统的 L1（token 覆写）层；L2（layout.json）/ L3（QML 壳）未做。

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

加载优先级：用户 QSettings 改绑 > `--keymap` / 皮肤 `keymap.json`（二者都进皮肤层）> 注册默认。
`--keymap` 与 `--skin` 不再跳过用户键位。`theme.json` 只在 `--skin` 或运行时换肤时读取。

## 已知边界

- L1 只覆写**颜色** token；非颜色（radius/fs/fonts）与 L2 layout/L3 QML 壳后置（doc/skin-system.md 五、）；
- Theme token 已是 NOTIFY（运行时换肤会重算）；启动 `--skin` 仍在 `loadFromModule` 前 `loadTheme`；
- 皮肤**只影响表现层**；core/命令接口不受影响（doc/skin-system.md 三、3.：功能永远在引擎+默认皮肤兜底）。
