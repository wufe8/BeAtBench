// SPDX-License-Identifier: GPL-3.0-only
// 采样解码缓存（M4.3a）：线程安全 LRU + 字节预算 + 并发命中合并。
//
// 解决的问题（doc/02 §8 + M4.3 立项）：BMS 谱面引用上千采样，wav/ogg 全量解码
// ≈ 数百 MB 不可全载 → LRU 常驻预算（默认 128MiB）+ 后台解码线程池前置。
// 本类 = 前缀「解码 → 缓存」；并发调用同一文件只解一次（in-flight 合并）。
// 与「试听/模拟播放/波形/离线渲染」共用同一份缓存（同文件不重复解码）。
//
// 线程模型（本类无内部线程池，未命中时在**调用方线程**阻塞解码）：
// - 任意后台线程可 get()/get_w()：命中 O(1)（+1 引用）；未命中 = 阻塞解码；
//   ⚠️ UI 线程与回调线程禁止直接调用（卡 UI/回调），见 AudioEngine 编排；
// - 并发同 key：第一个调用者成为「解码者」，其余 cv 等待（不重复解码）；
// - 失效（文件重绑/变更）：invalidate() 移除缓存条目 + 标记 in-flight 取消
//   （解码完成后不插入，结果释放）；
// - 逐出：插入超预算按 LRU 驱逐；只释放「缓存持有」引用——正被播放器/调用方
//   持有的采样存活（引用计数协议见 DecodedSample.hpp）。
//
// 引用协议（与 DecodeResult 一致）：
// - 新解码 sample 计数 = 1（解码线程 = 创建者持有）；
// - insert 时缓存 `ref()` 再 +1（缓存持有者）——调用方的创建者持有保留给
//   本次 get() 的调用者（返回裸指针，计数 1 = 调用方持有，与 DecodeResult 一致，
//   可直接所有权转移给 SamplePlayer::play）；
// - 命中路径：缓存持有 +1 给调用方返回；
// - 逐出/失效/clear：`decoded_sample_release`（unref，归零即 delete——仅当
//   无其它持有者时；正被播放的采样不受影响）。
#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "beatbench/audio/AudioDecoder.hpp"
#include "beatbench/audio/DecodedSample.hpp"

namespace beatbench::audio {

/// 解码结果（从缓存取回）：ok + 裸指针（**引用计数 = 1，调用方持有**，
/// 与 DecodeResult 协议一致——播放时可直接所有权转移给 SamplePlayer::play）。
struct SampleCacheResult {
    bool ok = false;
    DecodedSample* sample = nullptr;
    std::string message;
};

class SampleCache {
public:
    /// 解码器覆写（测试注入用；空 = 默认 decode_audio_file / decode_audio_file_w）。
    using DecodeFn = std::function<DecodeResult(const std::string&)>;
    using DecodeFnW = std::function<DecodeResult(const std::wstring&)>;

    static constexpr std::size_t kDefaultBudgetBytes = 128u * 1024u * 1024u;

    explicit SampleCache(std::size_t budgetBytes = kDefaultBudgetBytes);
    ~SampleCache();

    SampleCache(const SampleCache&) = delete;
    SampleCache& operator=(const SampleCache&) = delete;

    /// 覆写解码器（测试用）；反复调用覆盖。空 fn = 恢复默认解码。
    void setDecoders(DecodeFn fn, DecodeFnW fnW);

    /// 取采样（POSIX 窄字符；路径须 UTF-8）。**阻塞**（未命中时解码），
    /// 只允许后台线程调用（UI/回调会卡顿，见类注释）。
    SampleCacheResult get(const std::string& path);

    /// Windows 宽字符重载（含非 ASCII 路径必须走它，见 AudioDecoder.hpp）。
    SampleCacheResult get_w(const std::wstring& path);

    /// 失效：移除缓存条目（若有）+ 取消 in-flight（完成后不插入）。
    /// 窄字符路径（UTF-8）与宽字符路径须传同一条路径（内部统一转 UTF-8 键）。
    void invalidate(const std::string& pathUtf8);
    void invalidate_w(const std::wstring& path);

    /// 清空全部缓存（in-flight 一并取消）。
    void clear();

    /// 调整预算（字节）；改小立即逐出（至少保留 1 条）。
    void setBudget(std::size_t bytes);
    std::size_t budgetBytes() const { return m_budget; }
    /// 当前驻留 PCM 字节数（诊断/设置页显示）。
    std::size_t residentBytes() const;
    /// 当前缓存条目数。
    std::size_t count() const;

private:
    struct Entry {
        DecodedSample* sample = nullptr;      ///< 缓存持有 1 份引用（insert 时 ref）
        std::list<std::string>::iterator lru; ///< m_lru 节点（touch/evict 用）
        std::size_t bytes = 0;
        std::string key;
    };
    struct InFlight;  // 定义在 .cpp（shared_ptr 跨调用者共享）

    SampleCacheResult getImpl(const std::string& key,
                              const std::function<DecodeResult()>& decode);
    void insertLocked(const std::string& key, DecodedSample* s);  // 缓存 ref() 后持有
    void evictOne();
    void touch(Entry& e);

    std::size_t m_budget = kDefaultBudgetBytes;
    std::size_t m_resident = 0;                 ///< 驻留字节数（PCM 数据）
    mutable std::mutex m_mutex;
    std::list<std::string> m_lru;               ///< front = LRU, back = MRU
    std::unordered_map<std::string, Entry> m_cache;
    std::unordered_map<std::string, std::shared_ptr<InFlight>> m_inFlight;
    DecodeFn m_fn;                              ///< 窄字符解码器（空 = 默认）
    DecodeFnW m_fnW;                            ///< 宽字符解码器（空 = 默认）
};

}  // namespace beatbench::audio
