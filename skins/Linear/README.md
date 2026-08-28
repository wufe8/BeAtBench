# Linear 样例皮肤（L1 + L2）

在 L1（颜色 token 覆写）基础上，演示 **L2 布局改动幅度**：非颜色 token（密度/圆角/字体）覆写。
对应 doc/beatbench-ui-styles.html 主题⑥ Linear（暗色高对比、精致圆角、单紫 accent）。

## 效果

- `--skin skins/Linear` 启动：暗底 + 单紫 accent + 精致圆角（note 圆角 3、控件圆角 6、面板 10）；
- 密度微调：fsBase 12.5 / fsSmall 11.5 / fsTiny 10.5（较内置默认 13/12/11 略紧凑）；
- 等宽字体用 Cascadia Mono（Windows 11 自带；缺省回退由 Qt 处理）。

## 文件

| 文件 | 作用 |
|---|---|
| `skin.json` | 皮肤清单：name/version/api（本次 layers 含 `L1`/`L2`） |
| `theme.json` | L1 颜色 + L2 非颜色 token（radius/fs/font）覆写 |
| `keymap.json` | 动作 id → 快捷键文本（另存为=Ctrl+Shift+S、更多轨道=Ctrl+Shift+E） |

## 应用方式

```
beatbench.exe --skin skins/Linear
```

## 已知边界

- L2 目前覆盖面 = **token 层**（密度/圆角/字体/note 样式），即 doc 主题①③⑤⑥ 的关键差异点；
  真正的**布局结构改动**（工具条行增删/面板排列/L3 QML 壳）仍属深水区（doc/08 §6），本样例不做；
- `--skin` 只影响表现层，core/命令接口不变（doc/08 §3.3）。
