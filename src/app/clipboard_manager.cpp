#include "clipboard_manager.h"
#include "clipboard_util.h"
#include "document.h"
#include "file_dialog_service.h"
#include "file_io.h"
#include "i18n.h"
#include "mermaid_util.h"
#include "selection_html.h"
#include "wic_util.h"

namespace {

const Node* ValidateNode(const Document& doc, int node_index, NodeType type) noexcept
{
    const auto& nodes = doc.GetNodes();
    if (node_index < 0 || node_index >= static_cast<int>(nodes.size()) || nodes[node_index].type != type) {
        return nullptr;
    }
    return &nodes[node_index];
}

std::pmr::vector<uint8_t> EncodePng(IWICImagingFactory* wic, IWICBitmapSource* bitmap)
{
    if (!wic || !bitmap) {
        return {};
    }
    UINT width = 0;
    UINT height = 0;
    if (FAILED(bitmap->GetSize(&width, &height)) || width == 0 || height == 0) {
        return {};
    }
    auto converted = wic_util::ConvertBitmapSource(wic, bitmap, GUID_WICPixelFormat32bppBGRA);
    if (!converted) {
        return {};
    }
    Microsoft::WRL::ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        return {};
    }
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        return {};
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) {
        return {};
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WriteSource(converted.Get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return {};
    }
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0) {
        return {};
    }
    HGLOBAL memory = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &memory)) || !memory) {
        return {};
    }
    auto* data = static_cast<const uint8_t*>(GlobalLock(memory));
    if (!data) {
        return {};
    }
    std::pmr::vector<uint8_t> png(data, data + static_cast<size_t>(stat.cbSize.QuadPart));
    GlobalUnlock(memory);
    return png;
}

} // namespace

void ClipboardManager::Init(HWND hwnd, IMermaidRenderer* mermaid_renderer, IWICImagingFactory* wic, ToastCallback show_toast) noexcept
{
    hwnd_ = hwnd;
    mermaid_renderer_ = mermaid_renderer;
    wic_ = wic;
    show_toast_ = std::move(show_toast);
}

void ClipboardManager::CopyCodeBlock(const Document& doc, int node_index, bool dark) const
{
    const Node* node = ValidateNode(doc, node_index, NodeType::CodeBlock);
    if (!node) {
        return;
    }

    const std::pmr::string html_utf8 = BuildCodeBlockHtmlFragment(*node, dark);
    // 選択コピーと同じく、プレーンテキストの改行は CRLF にする。
    const auto len = static_cast<uint32_t>(node->GetText().size());
    const std::pmr::string plain = ExtractSelectedText(doc.GetNodes(), TextSelection::MakeOrdered(node_index, 0, node_index, len));
    WriteClipboardHtml(hwnd_, html_utf8, plain);
}

void ClipboardManager::SaveDiagramAsPng(const Document& doc, int node_index, IWICBitmapSource* bitmap) const
{
    if (!ValidateNode(doc, node_index, NodeType::Diagram) || !bitmap) {
        return;
    }

    const auto png = EncodePng(wic_, bitmap);
    if (png.empty()) {
        return;
    }

    const auto filename = file_dialog_service::SavePngFileDialog(hwnd_, L"diagram.png");
    if (filename.empty()) {
        return;
    }

    const bool ok = AtomicWriteAllBytes(filename.c_str(), png.data(), png.size());
    show_toast_(ok ? i18n::S().toast_image_saved : i18n::S().toast_image_save_failed);
}

UniqueGlobalMem ClipboardManager::BuildDib(IWICBitmapSource* bitmap) const
{
    if (!bitmap || !wic_) {
        return {};
    }
    auto converted = wic_util::ConvertBitmapSource(wic_, bitmap, GUID_WICPixelFormat32bppBGRA);
    if (!converted) {
        return {};
    }
    UINT w = 0;
    UINT h = 0;
    if (FAILED(converted->GetSize(&w, &h)) || w == 0 || h == 0) {
        return {};
    }
    const size_t total = DibTotalBytes(w, h);
    if (total == 0) {
        return {};
    }
    return AllocGlobalFilled(total, [&](void* p) {
        auto* base = static_cast<uint8_t*>(p);
        WriteDibHeader(base, w, h);
        const UINT stride = w * 4;
        return SUCCEEDED(converted->CopyPixels(nullptr, stride, stride * h, base + sizeof(BITMAPINFOHEADER)));
    });
}

void ClipboardManager::EmitCopyResult(bool ok) const
{
    show_toast_(ok ? i18n::S().toast_diagram_copied : i18n::S().toast_diagram_copy_failed);
}

void ClipboardManager::CopyDiagramToClipboard(const Document& doc, int node_index, IWICBitmapSource* bitmap, float md_width, bool dark)
{
    const Node* node_ptr = ValidateNode(doc, node_index, NodeType::Diagram);
    if (!node_ptr) {
        return;
    }
    const auto& node = *node_ptr;

    // LaTeX は flowchart ラッパなので SVG は意味を成さない。画像 (CF_DIB) のみを同期コピーする。
    if (!IsSvgExportable(node.code_language())) {
        ++copy_generation_;
        EmitCopyResult(WriteClipboardDiagram(hwnd_, BuildDib(bitmap), {}));
        return;
    }

    const uint64_t key = mermaid_util::NodeDiagramHash(node, md_width, dark);
    if (const auto* hit = svg_cache_.Find(key)) {
        ++copy_generation_;
        EmitCopyResult(WriteClipboardDiagram(hwnd_, BuildDib(bitmap), *hit));
        return;
    }
    if (copy_in_flight_ || !mermaid_renderer_) {
        return;
    }

    const uint64_t gen = ++copy_generation_;
    show_toast_(i18n::S().toast_diagram_copying);
    copy_in_flight_ = true;

    UniqueGlobalMem dib = BuildDib(bitmap);

    // clang-format off
    mermaid_renderer_->RequestSvg(
        node.GetText(),
        md_width,
        dark,
        [this, key, gen, dib = std::move(dib)](std::pmr::wstring svg, bool cancelled) mutable {
            copy_in_flight_ = false;
            if (gen != copy_generation_) {
                return;
            }
            if (cancelled) {
                EmitCopyResult(false);
                return;
            }
            const bool ok = WriteClipboardDiagram(hwnd_, std::move(dib), svg);
            if (ok && !svg.empty()) {
                svg_cache_.Insert(key, std::move(svg));
            }
            EmitCopyResult(ok);
        }
    );
    // clang-format on
}
