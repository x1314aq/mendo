#pragma once
#include "worker_latch.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <windows.h>

class TaskScheduler;

// Mermaid ダイアグラムの SVG をディスクへ永続化するキャッシュ。
// 幅は mermaid_util::QuantizeWidth で量子化し、コード・テーマの組み合わせごとに SVG を保持する。
// SVG は解像度非依存なので DPR はキーに含めない。表示時に現在の DPR でラスタライズする。
// 書き出しは TaskScheduler を介してバックグラウンドで行う。
// インデックスはメモリ上で管理し、SaveIndex() でディスクへ書き出す。
class MermaidFileCache {
public:
    MermaidFileCache() = default;
    ~MermaidFileCache();

    MermaidFileCache(const MermaidFileCache&) = delete;
    MermaidFileCache& operator=(const MermaidFileCache&) = delete;

    struct CacheEntry {
        float css_width = 0.0f;
        float css_height = 0.0f;
    };

    // 索引の読み込みは初回利用まで遅らせ、図を含まない文書の起動を塞がない。
    void Init(TaskScheduler& scheduler);
    // インデックスにあれば SVG のパスと寸法を返し LRU を更新する。読み込みは呼び出し側が
    // 任意のスレッドで行い、失敗したら OnReadFailed で UI スレッドに戻して反映する。
    bool LookupPath(uint64_t key, CacheEntry& entry, std::filesystem::path& svg_path);
    // ファイルが確実に存在しない場合のみインデックスから除く。
    void OnReadFailed(uint64_t key, DWORD read_error);
    bool LookupDimensions(uint64_t key, CacheEntry& entry);
    std::optional<uint64_t> LookupDiagram(uint64_t code_hash, float max_width, bool dark_mode, CacheEntry& entry);
    // SVG は書き込みタスクが参照するためコピーする。
    void StoreAsync(uint64_t key, float css_width, float css_height, std::string_view svg_data);
    // SVG の書き込み完了を UI スレッドへ知らせる。worker から呼ばれるため、
    // コールバック内では PostMessage などスレッドセーフな処理だけを行うこと。
    void SetStoreCompletedCallback(std::function<void()> callback);
    void SaveIndex();
    void Shutdown();
    void SetCacheDir(const std::filesystem::path& dir);
    void SetLimits(size_t max_entries, uint64_t max_total_size);

    size_t EntryCount()
    {
        return Index().entries.size();
    }
    uint64_t TotalSize()
    {
        return Index().total_size;
    }

#ifdef MENDO_TESTING
    // テスト用: 外部キーから実ファイル名 (.svg) を導く。
    constexpr uint64_t InternalKeyForTest(uint64_t external_key) const noexcept
    {
        return InternalKey(external_key);
    }
#endif

private:
    static constexpr uint32_t MAGIC = 0x4D454D43u; // "MEMC"
    static constexpr uint32_t VERSION = 2;
    static constexpr size_t DEFAULT_MAX_ENTRIES = 4096;
    static constexpr uint64_t DEFAULT_MAX_TOTAL_SIZE = 1ULL * 1024 * 1024 * 1024; // 1GB

    struct IndexEntry {
        float css_width = 0.0f;
        float css_height = 0.0f;
        uint32_t data_size = 0;
        int64_t last_used = 0;
    };

    // 追い出しは満杯時だけなので、順序構造を持たず last_used の線形走査で最古を探す。
    struct IndexState {
        using Map = std::pmr::unordered_map<uint64_t, IndexEntry>;
        Map entries;
        uint64_t total_size = 0;
        int64_t lru_seq = 0;
        bool dirty = false;

        void Add(uint64_t key, IndexEntry entry);
        void Remove(Map::iterator it) noexcept;
        int64_t NextLruSeq() noexcept
        {
            return ++lru_seq;
        }
        void Touch(Map::iterator it) noexcept
        {
            it->second.last_used = NextLruSeq();
            dirty = true;
        }
    };

    std::filesystem::path GetSvgPath(uint64_t key) const;
    std::filesystem::path GetIndexPath() const;
    IndexState& Index();
    IndexState LoadIndex() const;
    // 索引に載っていない SVG と、旧形式の PNG をディレクトリから消す。
    void PruneOrphanFiles(const IndexState& index) const;
    static std::optional<uint64_t> ParseSvgKey(const std::filesystem::path& path) noexcept;
    void NotifyStoreCompleted() noexcept;
    void EvictIfNeeded(IndexState& index, uint32_t new_data_size);

    // SVG キャッシュでは DPR をキーに含めない。
    constexpr uint64_t InternalKey(uint64_t external_key) const noexcept
    {
        return external_key;
    }

    std::filesystem::path cache_dir_;

    std::optional<IndexState> index_;

    size_t max_entries_ = DEFAULT_MAX_ENTRIES;
    uint64_t max_total_size_ = DEFAULT_MAX_TOTAL_SIZE;

    TaskScheduler* scheduler_ = nullptr;
    std::atomic<uint32_t> write_gen_{ 0 };
    std::function<void()> store_completed_callback_;
    std::atomic<bool> store_notify_pending_{ false };

    mutable std::mutex pending_mutex_;
    std::pmr::unordered_map<uint64_t, bool> pending_writes_;

    WorkerLatch latch_;
};
