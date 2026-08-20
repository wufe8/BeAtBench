// SPDX-License-Identifier: GPL-3.0-only
// beatbench-cli：无 Qt 依赖的批处理入口（对齐稿 02 §6.1，P1 模式）。
// 与 GUI 共用 core 命令对象；子命令随里程碑加入（slice/package 见 doc/02 §7）。
#include <cstdio>
#include <string_view>

namespace {

constexpr std::string_view kVersion = "0.1.0";

void print_usage() {
    std::printf(
        "BeAtBench CLI %s\n"
        "用法: beatbench-cli <子命令> [参数]\n"
        "\n"
        "子命令:\n"
        "  check <file.bms>     谱面检查（lint 最小集）   [TODO M1]\n"
        "  convert <in> <out>   编码/往返写出转换          [TODO M1]\n"
        "  version              打印版本与许可信息\n",
        kVersion.data());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    const std::string_view cmd = argv[1];
    if (cmd == "version") {
        std::printf("beatbench-cli %s (GPL-3.0)\n", kVersion.data());
        return 0;
    }
    if (cmd == "check" || cmd == "convert") {
        std::printf("[TODO] 子命令 '%s' 于 M1 实现（依赖 core/bms codec 与 lint 最小集）。\n", argv[1]);
        return 0;
    }
    std::printf("未知子命令: %s\n\n", argv[1]);
    print_usage();
    return 2;
}
