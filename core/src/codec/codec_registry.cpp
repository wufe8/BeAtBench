// SPDX-License-Identifier: GPL-3.0-only
#include "beatbench/core/codec/CodecRegistry.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

namespace beatbench::codec {

namespace {

// 扩展名规范化：去点 + 小写（"BMS" → "bms"；".bms" → "bms"）
std::string norm_ext(std::string_view ext) {
    if (!ext.empty() && ext.front() == '.') ext.remove_prefix(1);
    std::string out(ext);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

void CodecRegistry::add(std::unique_ptr<Codec> codec) {
    if (!codec) return;
    const std::string id(codec->id());
    if (id.empty()) throw std::invalid_argument("codec id 不能为空");
    // 重复 id / 重复扩展名 → 抛错（协议稳定）
    const auto existing = ids();
    if (std::find(existing.begin(), existing.end(), id) != existing.end()) {
        throw std::invalid_argument("codec 已注册: " + id);
    }
    for (const auto ext : codec->extensions()) {
        const auto ne = norm_ext(ext);
        if (ne.empty()) continue;
        for (const auto& c : codecs_) {
            for (const auto e : c->extensions()) {
                if (norm_ext(e) == ne) {
                    throw std::invalid_argument("扩展名冲突: ." + ne + "（" + id + " 与 " +
                                                std::string(c->id()) + "）");
                }
            }
        }
    }
    codecs_.push_back(std::move(codec));
}

const Codec* CodecRegistry::by_id(std::string_view id) const {
    for (const auto& c : codecs_) {
        if (c->id() == id) return c.get();
    }
    return nullptr;
}

const Codec* CodecRegistry::by_extension(std::string_view ext) const {
    const auto ne = norm_ext(ext);
    for (const auto& c : codecs_) {
        for (const auto e : c->extensions()) {
            if (norm_ext(e) == ne) return c.get();
        }
    }
    return nullptr;
}

const Codec* CodecRegistry::by_path(const std::filesystem::path& path) const {
    return by_extension(path.extension().string());
}

std::vector<std::string> CodecRegistry::ids() const {
    std::vector<std::string> out;
    out.reserve(codecs_.size());
    for (const auto& c : codecs_) out.emplace_back(c->id());
    return out;
}

CodecRegistry& global_codec_registry() {
    static CodecRegistry instance = [] {
        CodecRegistry reg;
        register_builtin_codecs(reg);
        return reg;
    }();
    return instance;
}

}  // namespace beatbench::codec
