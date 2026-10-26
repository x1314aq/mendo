#include "mermaid_svg.h"
#include <lunasvg.h>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace {

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
        AddFont(L"Microsoft YaHei UI", L"C:\\Windows\\Fonts\\msyh.ttc");
        AddFont(L"Microsoft YaHei", L"C:\\Windows\\Fonts\\msyh.ttc");
        AddFont(L"Segoe UI", L"C:\\Windows\\Fonts\\segoeui.ttf");
        AddFont(L"Arial", L"C:\\Windows\\Fonts\\arial.ttf");
        AddFont(L"Consolas", L"C:\\Windows\\Fonts\\consola.ttf");
        // 未知の font-family でも CJK を表示できるよう、空ファミリのフォールバックも入れる。
        AddFont(L"", L"C:\\Windows\\Fonts\\msyh.ttc");
    });
}

void AppendXmlText(std::string& svg, std::string_view text)
{
    for (const char c : text) {
        switch (c) {
        case '&': svg += "&amp;"; break;
        case '<': svg += "&lt;"; break;
        case '>': svg += "&gt;"; break;
        case '"': svg += "&quot;"; break;
        case '\'': svg += "&apos;"; break;
        default: svg += c; break;
        }
    }
}

size_t FindCssDelimiter(std::string_view text, size_t start, char delimiter)
{
    char quote = 0;
    int depth = 0;
    for (size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (quote) {
            if (c == '\\') {
                ++i;
            }
            else if (c == quote) {
                quote = 0;
            }
        }
        else if (c == '\'' || c == '"') {
            quote = c;
        }
        else if (c == '(' || c == '[') {
            ++depth;
        }
        else if (c == ')' || c == ']') {
            --depth;
        }
        else if (c == delimiter && depth == 0) {
            return i;
        }
    }
    return text.npos;
}

void AppendApplicableRules(lunasvg::Document& document, std::string_view css, std::string& result)
{
    size_t pos = 0;
    while (pos < css.size()) {
        const auto open = FindCssDelimiter(css, pos, '{');
        if (open == css.npos) {
            break;
        }
        const auto close = FindCssDelimiter(css, open + 1, '}');
        if (close == css.npos) {
            break;
        }
        const auto selectors = css.substr(pos, open - pos);
        std::string applicable;
        size_t start = 0;
        while (start < selectors.size()) {
            const auto comma = FindCssDelimiter(selectors, start, ',');
            const auto end = comma == selectors.npos ? selectors.size() : comma;
            const std::string selector{ selectors.substr(start, end - start) };
            if (!document.querySelectorAll(selector).empty()) {
                if (!applicable.empty()) {
                    applicable += ',';
                }
                applicable += selector;
            }
            start = end + 1;
        }
        if (!applicable.empty()) {
            result += applicable;
            result += css.substr(open, close - open + 1);
        }
        pos = close + 1;
    }
}

} // namespace

std::unique_ptr<lunasvg::Document> LoadMermaidSvg(std::string_view svg)
{
    RegisterFontsOnce();
    auto document = lunasvg::Document::loadFromData(svg.data(), svg.size());
    if (!document) {
        return nullptr;
    }
    // LunaSVG は未対応のセレクタで CSS 全体の解析を止める。適用できる規則を
    // まとめて再適用し、後続の配色・fill:none と CSS の優先順位を保つ。
    std::string css;
    size_t pos = 0;
    while ((pos = svg.find("<style>", pos)) != svg.npos) {
        pos += 7;
        const auto end = svg.find("</style>", pos);
        if (end == svg.npos) {
            break;
        }
        AppendApplicableRules(*document, svg.substr(pos, end - pos), css);
        pos = end + 8;
    }
    document->applyStyleSheet(css);
    return document;
}

std::mutex& MermaidSvgMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::optional<MermaidTextMetrics> MeasureMermaidText(std::string_view text, double font_size,
                                                   std::string_view font_weight, std::string_view font_style)
{
    if (!(font_size > 0.0) || !std::isfinite(font_size)) {
        return std::nullopt;
    }
    const std::lock_guard lock(MermaidSvgMutex());
    RegisterFontsOnce();
    MermaidTextMetrics metrics;
    metrics.line_count = 0;
    size_t start = 0;
    do {
        const auto end = text.find('\n', start);
        const auto line = text.substr(start, end == text.npos ? text.size() - start : end - start);
        std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\"><text font-family=\"Microsoft YaHei UI, Segoe UI, sans-serif\" font-size=\"";
        svg += std::to_string(font_size);
        svg += "\" font-weight=\"";
        AppendXmlText(svg, font_weight.empty() ? "normal" : font_weight);
        svg += "\" font-style=\"";
        AppendXmlText(svg, font_style.empty() ? "normal" : font_style);
        svg += "\">";
        AppendXmlText(svg, line);
        svg += "</text></svg>";
        auto document = lunasvg::Document::loadFromData(svg);
        if (!document) {
            return std::nullopt;
        }
        document->updateLayout();
        const auto elements = document->querySelectorAll("text");
        if (elements.empty()) {
            return std::nullopt;
        }
        const auto box = elements.front().getBoundingBox();
        metrics.width = std::max(metrics.width, static_cast<double>(std::max(0.0f, box.x + box.w) - std::min(0.0f, box.x)));
        metrics.height = std::max(metrics.height, static_cast<double>(box.h));
        ++metrics.line_count;
        if (end == text.npos) {
            break;
        }
        start = end + 1;
    } while (start <= text.size());
    metrics.height *= metrics.line_count;
    if (!text.empty() && !(metrics.width > 0.0)) {
        return std::nullopt;
    }
    return metrics;
}
