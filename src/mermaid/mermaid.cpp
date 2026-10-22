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

struct DiagramColors {
    const char* text;
    const char* node_fill;
    const char* node_border;
    const char* edge;
    const char* cluster_fill;
    const char* cluster_border;
    const char* edge_label;
};

DiagramColors ColorsFor(bool dark_mode) noexcept
{
    if (dark_mode) {
        return { "#d4d4d4", "#252526", "#d4d4d4", "#d4d4d4", "#2d2d2d", "#555555", "#3c3c3c" };
    }
    return { "#24292e", "#ffffff", "#24292e", "#24292e", "#f6f8fa", "#d0d7de", "#eaeef2" };
}

// LunaSVG は merman が埋め込む <style> を適用しないため、主要な要素だけを
// 明示的にスタイルする。Mermaid の図種ごとの細かな CSS はここでは扱わない。
std::string BuildLunaCss(bool dark_mode)
{
    const DiagramColors c = ColorsFor(dark_mode);
    std::string css;
    css.reserve(1600);
    css += "text{font-family:\"Microsoft YaHei UI\",\"Segoe UI\",sans-serif;font-size:14px;fill:";
    css += c.text;
    css += ";}";
    css += ".node rect,.node circle,.node ellipse,.node polygon,.node path,.actor,.labelBox,.note,"
           ".classGroup rect,.stateGroup rect{fill:";
    css += c.node_fill;
    css += ";stroke:";
    css += c.node_border;
    css += ";stroke-width:2px;}";
    css += ".cluster rect,.cluster polygon{fill:";
    css += c.cluster_fill;
    css += ";stroke:";
    css += c.cluster_border;
    css += ";}";
    css += ".edgePaths .path,.flowchart-link,.messageLine0,.messageLine1,.relation,.transition,"
           ".state-edge,.edgePath path{stroke:";
    css += c.edge;
    css += ";fill:none;stroke-width:2px;}";
    css += "marker path,.arrowMarkerPath,.arrowheadPath{fill:";
    css += c.edge;
    css += ";stroke:";
    css += c.edge;
    css += ";}";
    css += ".edgeLabel rect,.labelBkg{fill:";
    css += c.edge_label;
    css += ";opacity:0.9;}";
    css += ".edgeLabel text,.label text,.nodeLabel,.messageText,.loopText,.labelText{fill:";
    css += c.text;
    css += ";}";
    css += "line,.lifeline,.actor-line{stroke:";
    css += c.node_border;
    css += ";}";
    return css;
}

bool EncodePng(IWICImagingFactory* wic, const lunasvg::Bitmap& bitmap, PngBytes& png)
{
    if (!wic || bitmap.isNull()) {
        return false;
    }
    Microsoft::WRL::ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) {
        return false;
    }
    const UINT width = static_cast<UINT>(bitmap.width());
    const UINT height = static_cast<UINT>(bitmap.height());
    const UINT stride = static_cast<UINT>(bitmap.stride());
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WritePixels(height, stride, stride * height, bitmap.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return false;
    }

    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
        return false;
    }
    HGLOBAL memory = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &memory)) || !memory) {
        return false;
    }
    auto* data = static_cast<const uint8_t*>(GlobalLock(memory));
    if (!data) {
        return false;
    }
    const size_t size = static_cast<size_t>(stat.cbSize.QuadPart);
    auto bytes = std::make_shared<std::pmr::vector<uint8_t>>(data, data + size);
    GlobalUnlock(memory);
    png = std::move(bytes);
    return true;
}

std::pmr::wstring ToWide(std::string_view utf8)
{
    std::pmr::wstring wide;
    string_convert::Utf8ToWide(utf8, wide);
    return wide;
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

    if (file_cache_) {
        file_cache_->SetDpr(dpr_);
    }
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
        return (c.png ? c.png->size() : 0) + BitmapBytes(c.bitmap.Get());
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
    diagram_entry.width = result.width;
    diagram_entry.height = result.height;
    diagram_entry.png = result.png;
    diagram_entry.stale = false;
}

void MermaidRenderer::SetDiagramError(const RenderRequest& req, std::wstring_view msg)
{
    constexpr size_t MAX_ERROR_LEN = 200;
    auto sanitized = mermaid_util::SanitizeErrorMessage(msg, MAX_ERROR_LEN);
    if (sanitized.empty()) {
        sanitized = i18n::S().diagram_error;
    }
    InsertCache(req.code_hash, CachedResult{ .error = std::move(sanitized) });
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
        std::filesystem::path png_path;
        if (const auto key = file_cache_->LookupDiagram(diagram_entry.code_hash, max_width, dark_mode, fentry);
            key && file_cache_->LookupPath(*key, fentry, png_path)) {
            req.width_free = *key == width_free_hash;
            req.css_width = fentry.css_width;
            req.css_height = fentry.css_height;
            StartDiskLoad(std::move(req), std::move(png_path));
            return false;
        }
    }
    EnqueueNativeRender(std::move(req));
    return false;
}

void MermaidRenderer::RequestSvg(std::string_view code, float max_width, bool dark_mode, SvgCallback callback)
{
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
        SetDiagramError(req, L"Native renderer queue is unavailable");
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

    auto document = lunasvg::Document::loadFromData(svg.data(), svg.size());
    if (!document) {
        job.captured = true;
        job.error = L"Failed to parse Mermaid SVG";
        return;
    }
    const float css_width = document->width();
    const float css_height = document->height();
    if (!(css_width > 0.5f) || !(css_height > 0.5f)) {
        job.captured = true;
        job.error = L"Mermaid SVG has no size";
        return;
    }
    document->applyStyleSheet(BuildLunaCss(job.req.dark_mode));

    double scale = dpr_;
    const double max_side = static_cast<double>(MAX_RASTER_SIDE);
    const double requested_w = css_width * scale;
    const double requested_h = css_height * scale;
    if (std::max(requested_w, requested_h) > max_side) {
        scale *= max_side / std::max(requested_w, requested_h);
    }
    const int pixel_width = std::clamp(static_cast<int>(std::ceil(css_width * scale)), 1, static_cast<int>(MAX_RASTER_SIDE));
    const int pixel_height = std::clamp(static_cast<int>(std::ceil(css_height * scale)), 1, static_cast<int>(MAX_RASTER_SIDE));

    auto bitmap = document->renderToBitmap(pixel_width, pixel_height, 0x00000000);
    if (bitmap.isNull()) {
        job.captured = true;
        job.error = L"Failed to rasterize Mermaid SVG";
        return;
    }
    PngBytes png;
    if (!EncodePng(wic_factory_.Get(), bitmap, png)) {
        job.captured = true;
        job.error = L"Failed to encode Mermaid PNG";
        return;
    }
    auto decoded = wic_util::DecodeFromMemory(wic_factory_.Get(), *png, GUID_ContainerFormatPng);
    if (!decoded) {
        job.captured = true;
        job.error = L"Failed to decode Mermaid PNG";
        return;
    }
    job.bitmap = wic_util::DecodeToWicBitmap(wic_factory_.Get(), *decoded, { decoded->pixel_width, decoded->pixel_height });
    if (!job.bitmap) {
        job.captured = true;
        job.error = L"Failed to create Mermaid bitmap";
        return;
    }
    job.req.css_width = css_width;
    job.req.css_height = css_height;
    job.req.width_free = mermaid_util::IsWidthFree(css_width, job.req.max_width);
    job.png = std::move(png);
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
    job.png = std::make_shared<const std::pmr::vector<uint8_t>>(std::move(data));
    TaskScheduler::EnsureWorkerCom();
    if (const auto decoded = wic_util::DecodeFromMemory(wic_factory_.Get(), *job.png, GUID_ContainerFormatPng)) {
        job.bitmap = wic_util::DecodeToWicBitmap(wic_factory_.Get(), *decoded, { decoded->pixel_width, decoded->pixel_height });
    }
}

bool MermaidRenderer::ApplyDiskLoad(DiskLoad& r)
{
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    if (r.bitmap && render_target_) {
        render_target_->CreateBitmapFromWicBitmap(r.bitmap.Get(), &bitmap);
    }
    if (bitmap) {
        if (r.captured) {
            if (file_cache_) {
                file_cache_->StoreAsync(r.req.CacheKey(), r.req.css_width, r.req.css_height, r.png);
            }
        }
        InsertCache(r.req.CacheKey(), CachedResult{ std::move(bitmap), r.req.css_width, r.req.css_height, r.png });
        ReleaseInflight(r.req);
        if (r.native) {
            native_busy_ = false;
        }
        return true;
    }

    if (r.captured) {
        SetDiagramError(r.req, r.error);
        ReleaseInflight(r.req);
        if (r.native) {
            native_busy_ = false;
        }
        return true;
    }

    if (file_cache_ && !r.png) {
        file_cache_->OnReadFailed(r.req.CacheKey(), r.read_error);
    }
    // ディスクに無かった/壊れていた図はネイティブ描画へ回す。
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
