#include "mermaid.h"
#include "d2d_util.h"
#include "file_io.h"
#include "i18n.h"
#include "string_convert.h"
#include "wic_util.h"
#include <lunasvg.h>
#include <objidl.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

// EstimateWorkerDpr の GetDpiForMonitor 用。
#pragma comment(lib, "shcore.lib")

namespace {

constexpr wchar_t kYaHeiPath[] = L"C:\\Windows\\Fonts\\msyh.ttc";
constexpr wchar_t kSegoePath[] = L"C:\\Windows\\Fonts\\segoeui.ttf";
constexpr wchar_t kArialPath[] = L"C:\\Windows\\Fonts\\arial.ttf";
constexpr wchar_t kConsolasPath[] = L"C:\\Windows\\Fonts\\consola.ttf";

void AddFont(const wchar_t* family, const wchar_t* path)
{
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    char narrow_path[MAX_PATH * 3]{};
    char narrow_family[128]{};
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow_path, sizeof(narrow_path), nullptr, nullptr)) {
        return;
    }
    if (family && *family) {
        WideCharToMultiByte(CP_UTF8, 0, family, -1, narrow_family, sizeof(narrow_family), nullptr, nullptr);
    }
    lunasvg_add_font_face_from_file(narrow_family, false, false, narrow_path);
}

void RegisterFontsOnce()
{
    static std::once_flag once;
    std::call_once(once, [] {
        AddFont(L"Microsoft YaHei UI", kYaHeiPath);
        AddFont(L"Microsoft YaHei", kYaHeiPath);
        AddFont(L"Segoe UI", kSegoePath);
        AddFont(L"Arial", kArialPath);
        AddFont(L"Consolas", kConsolasPath);
        // 未知の font-family でも CJK を表示できるよう、空ファミリのフォールバックも入れる。
        AddFont(L"", kYaHeiPath);
    });
}

// merman_ffi.dll は exe と同じフォルダにある。DEPENDENTLOADFLAG で system32 以外の
// 依存 DLL 検索が制限されているため、exe フォルダを明示して先にロードする。
bool EnsureMermanDllLoaded()
{
    static const bool loaded = [] {
        wchar_t path[MAX_PATH]{};
        const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (len == 0 || len == MAX_PATH) {
            return false;
        }
        std::wstring dll(path, len);
        const auto slash = dll.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            return false;
        }
        dll.resize(slash + 1);
        dll += L"merman_ffi.dll";
        const HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module != nullptr;
    }();
    return loaded;
}

// merman が埋め込む <style> を LunaSVG がそのまま適用するので、色は themeVariables に
// 任せる。ここでは図の外にあるテキストへフォントを揃えるだけにする。
std::string BuildLunaCss()
{
    return "text{font-family:\"Microsoft YaHei UI\",\"Segoe UI\",sans-serif;font-size:14px;}";
}

bool RasterizeSvg(IWICImagingFactory* wic, std::string_view svg, float dpr, float max_width,
                  Microsoft::WRL::ComPtr<IWICBitmap>& out, float& css_width, float& css_height, std::wstring& error)
{
    // LunaSVG のフォントキャッシュは共有なので、ディスク読み込みと初回描画が重なっても直列化する。
    static std::mutex raster_mutex;
    const std::lock_guard lock(raster_mutex);
    if (!wic) {
        error = L"WIC is unavailable";
        return false;
    }
    auto document = lunasvg::Document::loadFromData(svg.data(), svg.size());
    if (!document) {
        error = L"Failed to parse Mermaid SVG";
        return false;
    }
    css_width = document->width();
    css_height = document->height();
    if (!(css_width > 0.5f) || !(css_height > 0.5f)) {
        error = L"Mermaid SVG has no size";
        return false;
    }
    document->applyStyleSheet(BuildLunaCss());

    double scale = std::clamp(dpr, 0.25f, 8.0f);
    // 横に収まらない図は表示幅に合わせて縮小する。4096px 固定で作ると巨大な図の
    // ラスタライズが支配的になり、SVG キャッシュから復元する意味が薄れる。
    if (max_width > 0.5f && css_width > max_width) {
        scale = std::min(scale, scale * static_cast<double>(max_width) / css_width);
    }
    constexpr double kMaxSide = 4096.0;
    const double requested_w = css_width * scale;
    const double requested_h = css_height * scale;
    if (std::max(requested_w, requested_h) > kMaxSide) {
        scale *= kMaxSide / std::max(requested_w, requested_h);
    }
    const int pixel_width = std::clamp(static_cast<int>(std::ceil(css_width * scale)), 1, 4096);
    const int pixel_height = std::clamp(static_cast<int>(std::ceil(css_height * scale)), 1, 4096);

    auto bitmap = document->renderToBitmap(pixel_width, pixel_height, 0x00000000);
    if (bitmap.isNull()) {
        error = L"Failed to rasterize Mermaid SVG";
        return false;
    }
    const UINT stride = static_cast<UINT>(bitmap.stride());
    const UINT buffer_size = stride * static_cast<UINT>(bitmap.height());
    if (FAILED(wic->CreateBitmapFromMemory(static_cast<UINT>(bitmap.width()), static_cast<UINT>(bitmap.height()),
                                           GUID_WICPixelFormat32bppPBGRA, stride, buffer_size, bitmap.data(), &out))) {
        error = L"Failed to create Mermaid WIC bitmap";
        return false;
    }
    return true;
}

std::pmr::wstring ToWide(std::string_view utf8)
{
    std::pmr::wstring wide;
    string_convert::Utf8ToWide(utf8, wide);
    return wide;
}

size_t WicBitmapBytes(IWICBitmap* bitmap)
{
    if (!bitmap) {
        return 0;
    }
    UINT width = 0;
    UINT height = 0;
    if (FAILED(bitmap->GetSize(&width, &height))) {
        return 0;
    }
    return static_cast<size_t>(width) * height * 4;
}

} // namespace

MermaidRenderer::~MermaidRenderer()
{
    Shutdown();
}

void MermaidRenderer::Init(HWND hwnd, IWICImagingFactory* wic, TaskScheduler& bg_scheduler, UINT disk_loaded_msg,
                           std::move_only_function<void()> on_update)
{
    hwnd_ = hwnd;
    wic_factory_ = wic;
    bg_scheduler_ = &bg_scheduler;
    disk_loaded_msg_ = disk_loaded_msg;
    on_update_ = std::move(on_update);
    dpr_ = EstimateWorkerDpr();
}

float MermaidRenderer::EstimateWorkerDpr() noexcept
{
    const HMONITOR monitor = MonitorFromPoint(POINT{ -32000, -32000 }, MONITOR_DEFAULTTONEAREST);
    UINT dpi_x = 0;
    UINT dpi_y = 0;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
        dpi_x = 0;
    }
    return DpiScaleFrom(static_cast<float>(dpi_x));
}

void MermaidRenderer::SetRenderTarget(ID2D1RenderTarget* render_target)
{
    render_target_ = render_target;
    cache_.Clear();
}

void MermaidRenderer::InsertCache(uint64_t hash, CachedResult result)
{
    cache_.Insert(hash, std::move(result));
    cache_.TrimToBudget(MAX_CACHE_BYTES, [](const CachedResult& c) static {
        return c.svg.size() + BitmapBytes(c.bitmap.Get()) + WicBitmapBytes(c.wic_bitmap.Get());
    });
}

const MermaidRenderer::CachedResult* MermaidRenderer::FindCached(uint64_t code_hash, uint64_t width_free_hash, float max_width)
{
    if (const auto* free = cache_.Find(width_free_hash); free && mermaid_util::IsWidthFree(free->width, max_width)) {
        return free;
    }
    return cache_.Find(code_hash);
}

void MermaidRenderer::ApplyResult(DiagramEntry& diagram_entry, const CachedResult& result)
{
    if (!result.error.empty()) {
        diagram_entry.ResetForRetry();
        diagram_entry.error = result.error;
        return;
    }
    diagram_entry.bitmap = result.bitmap;
    diagram_entry.wic_bitmap = result.wic_bitmap;
    diagram_entry.svg = result.svg;
    diagram_entry.width = result.width;
    diagram_entry.height = result.height;
    diagram_entry.stale = false;
}

void MermaidRenderer::SetDiagramError(const RenderRequest& req, DiagramError::Kind kind, std::wstring_view msg)
{
    // DrawTextCmd の 255 文字上限とプレースホルダ高さ (約3行) に収まる長さ。
    constexpr size_t MAX_ERROR_LEN = 200;
    InsertCache(req.code_hash, CachedResult{ .error = { kind, mermaid_util::SanitizeErrorMessage(msg, MAX_ERROR_LEN) } });
}

bool MermaidRenderer::RequestRender(const Node& node, DiagramEntry& diagram_entry, float max_width, bool dark_mode)
{
    if (node.type != NodeType::Diagram || !render_target_) {
        return false;
    }
    if (diagram_entry.code_hash == 0) {
        diagram_entry.code_hash = mermaid_util::DiagramCodeHash(node);
    }
    const uint64_t hash = mermaid_util::DiagramRequestHash(diagram_entry.code_hash, max_width, dark_mode);
    const uint64_t width_free_hash = mermaid_util::DiagramWidthFreeHash(diagram_entry.code_hash, dark_mode);
    if (const auto* cached = FindCached(hash, width_free_hash, max_width)) {
        ApplyResult(diagram_entry, *cached);
        return true;
    }
    if (!inflight_.insert(hash).second) {
        return false;
    }

    RenderRequest req{
        .code = std::pmr::string{ node.GetText() },
        .latex = node.code_language() == SyntaxLanguage::LatexMath,
        .max_width = max_width,
        .dark_mode = dark_mode,
        .code_hash = hash,
        .width_free_hash = width_free_hash,
    };
    if (file_cache_) {
        MermaidFileCache::CacheEntry fentry;
        std::filesystem::path svg_path;
        if (const auto key = file_cache_->LookupDiagram(diagram_entry.code_hash, max_width, dark_mode, fentry);
            key && file_cache_->LookupPath(*key, fentry, svg_path)) {
            req.width_free = *key == width_free_hash;
            req.css_width = fentry.css_width;
            req.css_height = fentry.css_height;
            StartDiskLoad(std::move(req), std::move(svg_path));
            return false;
        }
    }
    EnqueueNativeRender(std::move(req));
    return false;
}

void MermaidRenderer::RequestSvg(std::string_view code, float max_width, bool dark_mode, SvgCallback callback)
{
    const uint64_t code_hash = Fnv1a64(code);
    const uint64_t request_hash = mermaid_util::DiagramRequestHash(code_hash, max_width, dark_mode);
    const uint64_t width_free_hash = mermaid_util::DiagramWidthFreeHash(code_hash, dark_mode);
    if (const auto* cached = FindCached(request_hash, width_free_hash, max_width); cached && !cached->svg.empty()) {
        callback(ToWide(cached->svg), false);
        return;
    }

    std::string error;
    if (!EnsureMermanInitialized(error)) {
        callback({}, false);
        return;
    }
    std::string svg;
    if (merman_.RenderSvg(code, max_width, dark_mode, svg, error)) {
        callback(ToWide(svg), false);
    } else {
        callback({}, false);
    }
}

void MermaidRenderer::EnqueueNativeRender(RenderRequest req)
{
    if (shutting_down_) {
        ReleaseInflight(req);
        return;
    }
    pending_requests_.push(std::move(req));
    StartNextNativeRender();
}

void MermaidRenderer::StartNextNativeRender()
{
    if (native_busy_ || shutting_down_ || !bg_scheduler_ || pending_requests_.empty()) {
        return;
    }
    RenderRequest req = std::move(pending_requests_.front());
    pending_requests_.pop();
    native_busy_ = true;
    const bool posted = bg_scheduler_->Post([this, req = std::move(req), guard = latch_.Acquire()]() mutable {
        DiskLoad job;
        job.req = std::move(req);
        job.native = true;
        RenderNative(job);
        if (disk_results_.Push(std::move(job))) {
            ::PostMessageW(hwnd_, disk_loaded_msg_, 0, 0);
        }
    });
    if (!posted) {
        native_busy_ = false;
        SetDiagramError(req, DiagramError::Kind::Render, L"Native renderer queue is unavailable");
        ReleaseInflight(req);
        NotifyUpdate();
    }
}

bool MermaidRenderer::EnsureMermanInitialized(std::string& error)
{
    if (merman_ready_.load(std::memory_order_acquire)) {
        return true;
    }
    if (merman_init_failed_.load(std::memory_order_acquire)) {
        error = "Merman native renderer initialization previously failed";
        return false;
    }
    const std::lock_guard lock(merman_init_mutex_);
    if (merman_ready_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!EnsureMermanDllLoaded()) {
        error = "merman_ffi.dll could not be loaded";
        merman_init_failed_.store(true, std::memory_order_release);
        return false;
    }
    if (!merman_.Initialize(error)) {
        merman_init_failed_.store(true, std::memory_order_release);
        return false;
    }
    merman_ready_.store(true, std::memory_order_release);
    return true;
}

void MermaidRenderer::RenderNative(DiskLoad& job)
{
    TaskScheduler::EnsureWorkerCom();
    RegisterFontsOnce();

    std::string init_error;
    if (!EnsureMermanInitialized(init_error)) {
        job.captured = true;
        job.error = ToWide(init_error);
        return;
    }

    std::string code(job.req.code.data(), job.req.code.size());
    if (job.req.latex) {
        std::pmr::wstring wide = ToWide(code);
        wide = mermaid_util::BuildLatexFlowchartCode(wide);
        code = string_convert::WideToUtf8(wide);
    }

    std::string svg;
    std::string error;
    if (!merman_.RenderSvg(code, job.req.max_width, job.req.dark_mode, svg, error)) {
        job.captured = true;
        job.error = ToWide(error);
        return;
    }
    if (!wic_factory_) {
        job.captured = true;
        job.error = L"WIC is unavailable";
        return;
    }

    Microsoft::WRL::ComPtr<IWICBitmap> wic_bitmap;
    std::wstring raster_error;
    if (!RasterizeSvg(wic_factory_.Get(), svg, dpr_, job.req.max_width, wic_bitmap,
                      job.req.css_width, job.req.css_height, raster_error)) {
        job.captured = true;
        job.error = std::move(raster_error);
        return;
    }
    job.req.width_free = mermaid_util::IsWidthFree(job.req.css_width, job.req.max_width);
    job.bitmap = std::move(wic_bitmap);
    job.svg = std::move(svg);
    job.captured = true;
}

void MermaidRenderer::StartDiskLoad(RenderRequest req, std::filesystem::path png_path)
{
    const uint64_t hash = req.code_hash;
    if (!bg_scheduler_) {
        inflight_.erase(hash);
        return;
    }
    const bool posted = bg_scheduler_->Post([this, job = DiskLoad{ .req = std::move(req) }, path = std::move(png_path),
                                               guard = latch_.Acquire()]() mutable {
        LoadDiskJob(job, path);
        if (disk_results_.Push(std::move(job))) {
            ::PostMessageW(hwnd_, disk_loaded_msg_, 0, 0);
        }
    });
    if (!posted) {
        inflight_.erase(hash);
    }
}

void MermaidRenderer::LoadDiskJob(DiskLoad& job, const std::filesystem::path& path)
{
    auto data = ReadAllBytes(path.c_str(), &job.read_error);
    if (data.empty()) {
        return;
    }
    std::string svg(reinterpret_cast<const char*>(data.data()), data.size());
    std::wstring error;
    TaskScheduler::EnsureWorkerCom();
    RegisterFontsOnce();
    if (RasterizeSvg(wic_factory_.Get(), svg, dpr_, job.req.max_width, job.bitmap,
                     job.req.css_width, job.req.css_height, error)) {
        job.svg = std::move(svg);
    } else {
        job.error = std::move(error);
    }
}

bool MermaidRenderer::ApplyDiskLoad(DiskLoad& r)
{
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    if (r.bitmap && render_target_) {
        render_target_->CreateBitmapFromWicBitmap(r.bitmap.Get(), &bitmap);
    }
    if (bitmap) {
        if (r.captured && file_cache_ && !r.svg.empty()) {
            file_cache_->StoreAsync(r.req.CacheKey(), r.req.css_width, r.req.css_height, r.svg);
        }
        InsertCache(r.req.CacheKey(), CachedResult{
            .bitmap = std::move(bitmap),
            .wic_bitmap = r.bitmap,
            .width = r.req.css_width,
            .height = r.req.css_height,
            .svg = std::move(r.svg),
        });
        ReleaseInflight(r.req);
        if (r.native) {
            native_busy_ = false;
        }
        return true;
    }

    if (r.captured) {
        SetDiagramError(r.req, DiagramError::Kind::Render, r.error);
        ReleaseInflight(r.req);
        if (r.native) {
            native_busy_ = false;
        }
        return true;
    }

    if (file_cache_ && r.svg.empty()) {
        file_cache_->OnReadFailed(r.req.CacheKey(), r.read_error);
    }
    ReleaseInflight(r.req);
    EnqueueNativeRender(std::move(r.req));
    return false;
}

void MermaidRenderer::ProcessDiskLoads(std::chrono::steady_clock::time_point deadline)
{
    bool resolved = false;
    disk_results_.Drain(deadline, [&](DiskLoad& r) { resolved |= ApplyDiskLoad(r); });
    StartNextNativeRender();
    if (resolved) {
        NotifyUpdate();
    }
}

void MermaidRenderer::DropQueued()
{
    while (!pending_requests_.empty()) {
        ReleaseInflight(pending_requests_.front());
        pending_requests_.pop();
    }
}

void MermaidRenderer::Shutdown()
{
    if (shutting_down_) {
        return;
    }
    shutting_down_ = true;
    DropQueued();
    latch_.Wait();
    merman_.Shutdown();
    merman_ready_.store(false, std::memory_order_release);
    bg_scheduler_ = nullptr;
    native_busy_ = false;
}
