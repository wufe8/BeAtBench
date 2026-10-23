// SPDX-License-Identifier: GPL-3.0-only
// 原子写入：成功替换；失败保留原文件；不把 .tmp 留成目标。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "beatbench/core/io/AtomicWrite.hpp"

namespace {

std::string read_all(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::filesystem::path temp_dir() {
    const auto dir = std::filesystem::temp_directory_path() / "bb_atomic_write_test";
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST(AtomicWrite, CreatesAndOverwritesTarget) {
    const auto dir = temp_dir();
    const auto dest = dir / "chart.bms";
    std::filesystem::remove(dest);
    std::filesystem::remove(std::filesystem::path(dest.string() + ".tmp"));

    auto first = beatbench::io::atomic_write_file(dest, std::string_view("hello"));
    ASSERT_TRUE(first.ok) << first.error;
    EXPECT_EQ(read_all(dest), "hello");
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(dest.string() + ".tmp")));

    auto second = beatbench::io::atomic_write_file(dest, std::string_view("world!!"));
    ASSERT_TRUE(second.ok) << second.error;
    EXPECT_EQ(read_all(dest), "world!!");
}

TEST(AtomicWrite, FailedReplaceKeepsOriginalAndDropsTmp) {
    const auto dir = temp_dir();
    const auto dest = dir / "keep.bms";
    {
        std::ofstream out(dest, std::ios::binary);
        out << "ORIGINAL";
    }
    const auto tmp = dir / "ghost.tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "NEW";
    }
    // dest 改成目录 → rename/replace 失败；原内容应仍可由调用方路径策略保留。
    // 这里测 atomic_replace_file：失败删 tmp、不把 dest 换成新内容。
    std::filesystem::remove(dest);
    std::filesystem::create_directory(dest);
    const auto wr = beatbench::io::atomic_replace_file(tmp, dest);
    EXPECT_FALSE(wr.ok);
    EXPECT_FALSE(std::filesystem::exists(tmp));
    EXPECT_TRUE(std::filesystem::is_directory(dest));
    std::filesystem::remove_all(dest);
}

TEST(AtomicWrite, MissingParentFailsWithoutTarget) {
    const auto dest = temp_dir() / "no_such_dir" / "out.bms";
    const auto wr = beatbench::io::atomic_write_file(dest, std::string_view("x"));
    EXPECT_FALSE(wr.ok);
    EXPECT_FALSE(std::filesystem::exists(dest));
}
