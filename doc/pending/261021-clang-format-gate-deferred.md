# clang-format 全量格式化门禁

## 背景

2026-10-21 代码质量审计实测：仓库从未机器格式化，以候选风格（4 空格缩进 / 100 列 /
指针右贴，已对齐现有代码的可辨识习惯）试排，约 90% 的行会被重写
（`core/src/bms/bms_parser.cpp` 788/838 行、`core/include/beatbench/core/edit/EditorSession.hpp`
375/420 行）；且本机与 CI 的 clang-format 大版本不同（23 对 18），不钉版本会互相打架。
当时决策：冲击与收益不成比，缓做，等大重构窗口。

## 出口条件

满足其一即关账：
1. 进入大重构窗口，接受一次性格式化提交（全仓 diff + blame 断裂），`.clang-format`
   配置先小范围过目再铺开；
2. 或决定放弃工具化，改在 AGENTS.md 记“格式靠人工”的正面约定并删除本挂账。

落地时的钉版要求：PyPI `clang-format` wheel（三平台预编译）或 CI 侧 llvm apt 钉大版本；
先立 `.clang-format`，一次性格式化提交，门禁用 `--dry-run --Werror`。
