#include "mermaid_file_cache.h"
#include "mermaid_util.h"
#include "task_scheduler.h"
#include "file_io.h"
#include "utility.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>

namespace {

#pragma pack(push, 1)
struct IndexHeader {
    uint32_t magic;
    uint32_t version;
    float reserved_dpr;
    uint32_t count;
};

struct IndexRecord {
    uint64_t key;
    float css_width;
    float css_height;
    uint32_t data_size;
    int64_t last_used;
};
#pragma pack(pop)

static_assert(sizeof(IndexHeader) == 16);
static_assert(sizeof(IndexRecord) == 28);

} // namespace

MermaidFileCache::~MermaidFileCache()
{
    Shutdown();
}

void MermaidFileCache::SetCacheDir(const std::filesystem::path& dir)
{
    cache_dir_ = dir;
}

void MermaidFileCache::SetLimits(size_t max_entries, uint64_t max_total_size)
{
    max_entries_ = max_entries;
    max_total_size_ = max_total_size;
}

std::filesystem::path MermaidFileCache::GetSvgPath(uint64_t key) const
{
    if (cache_dir_.empty()) {
        return {};
    }
    wchar_t name[24];
    const auto r = std::format_to_n(name, std::ranges::size(name) - 1, L"{:016x}.svg", key);
    return cache_dir_ / std::wstring_view{ name, static_cast<size_t>(r.out - name) };
}

std::filesystem::path MermaidFileCache::GetIndexPath() const
{
    if (cache_dir_.empty()) {
        return {};
    }
    return cache_dir_ / L"index.bin";
}

void MermaidFileCache::Init(TaskScheduler& scheduler)
{
    scheduler_ = &scheduler;
    index_.reset();
}

MermaidFileCache::IndexState& MermaidFileCache::Index()
{
    if (!index_) {
        index_ = LoadIndex();
        PruneOrphanFiles(*index_);
    }
    return *index_;
}

MermaidFileCache::IndexState MermaidFileCache::LoadIndex() const
{
    IndexState index;
    const auto path = GetIndexPath();
    if (path.empty()) {
        return index;
    }

    const auto buf = ReadAllBytes(path.c_str());
    const size_t buf_size = buf.size();
    if (buf_size < sizeof(IndexHeader)) {
        return index;
    }

    const uint8_t* p = buf.data();
    IndexHeader header;
    std::memcpy(&header, p, sizeof(header));
    p += sizeof(header);

    if (header.magic != MAGIC || header.version != VERSION) {
        return index;
    }
    if (header.count > DEFAULT_MAX_ENTRIES * 2) {
        return index;
    }

    index.entries.reserve(header.count);

    for (uint32_t i = 0; i < header.count; ++i) {
        if (static_cast<size_t>(p - buf.data()) + sizeof(IndexRecord) > buf_size) {
            break;
        }

        IndexRecord record;
        std::memcpy(&record, p, sizeof(record));
        p += sizeof(record);

        if (record.css_width <= 0.0f || record.css_height <= 0.0f ||
            !std::isfinite(record.css_width) || !std::isfinite(record.css_height) ||
            record.data_size == 0) {
            continue;
        }

        index.Add(record.key, { record.css_width, record.css_height, record.data_size, record.last_used });
    }
    index.dirty = false;
    return index;
}

void MermaidFileCache::IndexState::Add(uint64_t key, IndexEntry entry)
{
    entries.insert_or_assign(key, entry);
    total_size += entry.data_size;
    lru_seq = std::max(lru_seq, entry.last_used);
    dirty = true;
}

void MermaidFileCache::IndexState::Remove(Map::iterator it) noexcept
{
    total_size -= std::min<uint64_t>(total_size, it->second.data_size);
    entries.erase(it);
    dirty = true;
}

std::optional<uint64_t> MermaidFileCache::ParseSvgKey(const std::filesystem::path& path) noexcept
{
    if (path.extension() != L".svg") {
        return std::nullopt;
    }
    const std::wstring stem = path.stem().wstring();
    if (stem.size() != 16) {
        return std::nullopt;
    }
    uint64_t key = 0;
    for (const wchar_t c : stem) {
        uint32_t digit = 0;
        if (c >= L'0' && c <= L'9') {
            digit = static_cast<uint32_t>(c - L'0');
        }
        else if (c >= L'a' && c <= L'f') {
            digit = static_cast<uint32_t>(c - L'a' + 10);
        }
        else if (c >= L'A' && c <= L'F') {
            digit = static_cast<uint32_t>(c - L'A' + 10);
        }
        else {
            return std::nullopt;
        }
        key = (key << 4) | digit;
    }
    return key;
}

void MermaidFileCache::PruneOrphanFiles(const IndexState& index) const
{
    if (cache_dir_.empty()) {
        return;
    }
    std::error_code ec;
    for (std::filesystem::directory_iterator it(cache_dir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) || file_ec) {
            continue;
        }
        const auto path = it->path();
        if (path.extension() == L".png") {
            // v1 の PNG キャッシュは v2 の SVG 索引から参照されない。
            std::error_code remove_ec;
            std::filesystem::remove(path, remove_ec);
            continue;
        }
        const auto key = ParseSvgKey(path);
        if (!key || index.entries.contains(*key)) {
            continue;
        }
        std::error_code remove_ec;
        std::filesystem::remove(path, remove_ec);
    }
}

void MermaidFileCache::SetStoreCompletedCallback(std::function<void()> callback)
{
    store_completed_callback_ = std::move(callback);
}

void MermaidFileCache::NotifyStoreCompleted() noexcept
{
    if (store_notify_pending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (store_completed_callback_) {
        store_completed_callback_();
    }
}

void MermaidFileCache::SaveIndex()
{
    store_notify_pending_.store(false, std::memory_order_release);
    const auto path = GetIndexPath();
    if (path.empty() || !index_ || !index_->dirty) {
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    const uint32_t count = static_cast<uint32_t>(index_->entries.size());

    const size_t buf_size = sizeof(IndexHeader) + count * sizeof(IndexRecord);
    auto buf = std::make_unique_for_overwrite<uint8_t[]>(buf_size);
    uint8_t* p = buf.get();

    IndexHeader header{ MAGIC, VERSION, 1.0f, count };
    std::memcpy(p, &header, sizeof(header));
    p += sizeof(header);

    for (const auto& [key, entry] : index_->entries) {
        IndexRecord record{ key, entry.css_width, entry.css_height, entry.data_size, entry.last_used };
        std::memcpy(p, &record, sizeof(record));
        p += sizeof(record);
    }

    if (AtomicWriteAllBytes(path, buf.get(), buf_size, /* durable = */ false)) {
        index_->dirty = false;
    }
}

bool MermaidFileCache::LookupPath(uint64_t key, CacheEntry& entry, std::filesystem::path& svg_path)
{
    auto& index = Index();
    key = InternalKey(key);
    auto it = index.entries.find(key);
    if (it == index.entries.end()) {
        return false;
    }

    svg_path = GetSvgPath(key);
    if (svg_path.empty()) {
        return false;
    }

    entry.css_width = it->second.css_width;
    entry.css_height = it->second.css_height;

    index.Touch(it);
    return true;
}

void MermaidFileCache::OnReadFailed(uint64_t key, DWORD read_error)
{
    if (read_error != ERROR_FILE_NOT_FOUND && read_error != ERROR_PATH_NOT_FOUND) {
        return;
    }
    auto& index = Index();
    key = InternalKey(key);
    const auto it = index.entries.find(key);
    if (it == index.entries.end()) {
        return;
    }
    {
        const std::lock_guard lock(pending_mutex_);
        if (pending_writes_.contains(key)) {
            return;
        }
    }
    index.Remove(it);
}

std::optional<uint64_t> MermaidFileCache::LookupDiagram(uint64_t code_hash, float max_width, bool dark_mode, CacheEntry& entry)
{
    if (const uint64_t key = mermaid_util::DiagramWidthFreeHash(code_hash, dark_mode); LookupDimensions(key, entry) && mermaid_util::IsWidthFree(entry.css_width, max_width)) {
        return key;
    }
    if (const uint64_t key = mermaid_util::DiagramRequestHash(code_hash, max_width, dark_mode); LookupDimensions(key, entry)) {
        return key;
    }
    return std::nullopt;
}

bool MermaidFileCache::LookupDimensions(uint64_t key, CacheEntry& entry)
{
    const auto& index = Index();
    key = InternalKey(key);
    const auto it = index.entries.find(key);
    if (it == index.entries.end()) {
        return false;
    }
    entry.css_width = it->second.css_width;
    entry.css_height = it->second.css_height;
    return true;
}

void MermaidFileCache::StoreAsync(uint64_t key, float css_width, float css_height, std::string_view svg_data)
{
    if (svg_data.empty()) {
        return;
    }
    auto& index = Index();
    key = InternalKey(key);

    const uint32_t data_size = static_cast<uint32_t>(svg_data.size());

    if (const auto it = index.entries.find(key); it != index.entries.end()) {
        index.Remove(it);
    }

    EvictIfNeeded(index, data_size);

    index.Add(key, { css_width, css_height, data_size, index.NextLruSeq() });

    if (!scheduler_) {
        NotifyStoreCompleted();
        return;
    }

    {
        const std::lock_guard lock(pending_mutex_);
        const auto [it, inserted] = pending_writes_.insert_or_assign(key, false);
        if (!inserted) {
            return;
        }
    }

    const uint32_t gen = write_gen_.load();
    auto path = GetSvgPath(key);
    std::string data{ svg_data };
    const bool posted = scheduler_->Post([this, key, path = std::move(path), data = std::move(data), gen, latch_guard = latch_.Acquire()] {
        auto guard = ScopeGuard([this, key, &path] {
            bool evicted = false;
            {
                const std::lock_guard lock(pending_mutex_);
                const auto node = pending_writes_.extract(key);
                evicted = node && node.mapped();
            }
            if (evicted && !path.empty()) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        });

        if (write_gen_.load() != gen) {
            return;
        }

        if (path.empty()) {
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        (void)WriteAllBytes(path, data.data(), data.size());
        NotifyStoreCompleted();
    });
    if (!posted) {
        const std::lock_guard lock(pending_mutex_);
        pending_writes_.erase(key);
    }
}

void MermaidFileCache::EvictIfNeeded(IndexState& index, uint32_t new_data_size)
{
    while (!index.entries.empty() && (index.entries.size() >= max_entries_ || index.total_size + new_data_size > max_total_size_)) {
        const auto oldest = std::ranges::min_element(index.entries, {}, [](const auto& kv) static { return kv.second.last_used; });
        bool pending = false;
        {
            const std::lock_guard lock(pending_mutex_);
            if (const auto it = pending_writes_.find(oldest->first); it != pending_writes_.end()) {
                it->second = true;
                pending = true;
            }
        }
        if (!pending) {
            if (const auto svg_path = GetSvgPath(oldest->first); !svg_path.empty()) {
                std::error_code ec;
                std::filesystem::remove(svg_path, ec);
            }
        }
        index.Remove(oldest);
    }
}

void MermaidFileCache::Shutdown()
{
    write_gen_.fetch_add(1);
    latch_.Wait();
    scheduler_ = nullptr;
}
