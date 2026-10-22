#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "lru_cache.h"
#include "merman_abi.h"
#include "mermaid_file_cache.h"
#include "mermaid_renderer_interface.h"
#include "mermaid_util.h"
#include "task_scheduler.h"
#include "utility.h"
#include "worker_latch.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <queue>
#include <unordered_set>

// Merman の C ABI で Mermaid SVG を作り、LunaSVG でラスタライズして
// ID2D1Bitmap と PNG を得るネイティブレンダラ。WebView2 は使わない。
class MermaidRenderer : public IMermaidRenderer {
public:
    MermaidRenderer() = default;
    ~MermaidRenderer() override;

    MermaidRenderer(const MermaidRenderer&) = delete;
    MermaidRenderer& operator=(const MermaidRenderer&) = delete;

    // wic は PNG のエンコード/デコードに使う。bg_scheduler は SVG 生成と
    // ラスタライズを UI スレッド外で行う。完了は disk_loaded_msg で通知する。
    void Init(HWND hwnd, IWICImagingFactory* wic, TaskScheduler& bg_scheduler, UINT disk_loaded_msg,
              std::move_only_function<void()> on_update);

    bool RequestRender(const Node& node, DiagramEntry& diagram_entry, float max_width, bool dark_mode) override;
    void RequestSvg(std::string_view code, float max_width, bool dark_mode, SvgCallback callback) override;
    void SetRenderTarget(ID2D1RenderTarget* render_target);
    void SetFileCache(MermaidFileCache* cache) noexcept
    {
        file_cache_ = cache;
    }
    void SetDpr(float dpr) noexcept
    {
        dpr_ = std::clamp(dpr, 0.25f, 8.0f);
    }
    static float EstimateWorkerDpr() noexcept;
    void ProcessDiskLoads(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kUploadSliceBudget);
    bool HasLeftoverDiskLoads() const noexcept
    {
        return disk_results_.HasLeftover();
    }
    void Shutdown();
    void DropQueued() override;
    // WebView2 時代のタイマー API。ネイティブ実装では何もしない。
    void OnInitRetryTimer()
    {}
    void OnIdleTimer()
    {}
    void SetEditingDiagrams(bool)
    {}

private:
    struct RenderRequest {
        std::pmr::string code;
        bool latex = false;
        float max_width = 0.0f;
        bool dark_mode = false;
        uint64_t code_hash = 0;
        uint64_t width_free_hash = 0;
        bool width_free = false;
        float css_width = 0.0f;
        float css_height = 0.0f;
        uint64_t CacheKey() const noexcept
        {
            return width_free ? width_free_hash : code_hash;
        }
    };

    struct CachedResult {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        float width = 0.0f;
        float height = 0.0f;
        PngBytes png;
        std::pmr::wstring error;
    };

    struct DiskLoad {
        RenderRequest req;
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        PngBytes png;
        bool captured = false;
        bool native = false;
        DWORD read_error = 0;
        std::pmr::wstring error;
    };

    static constexpr size_t MAX_CACHE_ENTRIES = 128;
    static constexpr size_t MAX_CACHE_BYTES = 64u * 1024 * 1024;
    static constexpr unsigned MAX_RASTER_SIDE = 4096;

    static void ApplyResult(DiagramEntry& diagram_entry, const CachedResult& result);
    void SetDiagramError(const RenderRequest& req, std::wstring_view msg);
    void ReleaseInflight(const RenderRequest& req) noexcept
    {
        inflight_.erase(req.code_hash);
    }
    void NotifyUpdate()
    {
        if (on_update_) {
            on_update_();
        }
    }

    void EnqueueNativeRender(RenderRequest req);
    void StartNextNativeRender();
    bool EnsureMermanInitialized(std::string& error);
    void RenderNative(DiskLoad& job);
    void StartDiskLoad(RenderRequest req, std::filesystem::path png_path);
    void LoadDiskJob(DiskLoad& job, const std::filesystem::path& path);
    bool ApplyDiskLoad(DiskLoad& r);
    void InsertCache(uint64_t hash, CachedResult result);
    const CachedResult* FindCached(uint64_t code_hash, uint64_t width_free_hash, float max_width);

    ID2D1RenderTarget* render_target_ = nullptr;
    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    std::move_only_function<void()> on_update_;
    MermanAbi merman_;
    std::atomic<bool> merman_ready_{ false };
    std::atomic<bool> merman_init_failed_{ false };
    std::mutex merman_init_mutex_;

    std::queue<RenderRequest, std::pmr::deque<RenderRequest>> pending_requests_;
    std::pmr::unordered_set<uint64_t> inflight_;
    bool native_busy_ = false;
    bool shutting_down_ = false;
    float dpr_ = 1.0f;

    LruCache<uint64_t, CachedResult, MAX_CACHE_ENTRIES> cache_;
    MermaidFileCache* file_cache_ = nullptr;
    TaskScheduler* bg_scheduler_ = nullptr;
    UINT disk_loaded_msg_ = 0;
    PostedInbox<DiskLoad> disk_results_;
    WorkerLatch latch_;
};
