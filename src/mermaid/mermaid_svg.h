#pragma once
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace lunasvg {
class Document;
}

std::unique_ptr<lunasvg::Document> LoadMermaidSvg(std::string_view svg);

struct MermaidTextMetrics {
    double width = 0.0;
    double height = 0.0;
    size_t line_count = 1;
};

std::optional<MermaidTextMetrics> MeasureMermaidText(std::string_view text, double font_size,
                                                   std::string_view font_weight, std::string_view font_style);
// 計測と描画による共有フォントキャッシュへの同時アクセスを防ぐ。
std::mutex& MermaidSvgMutex();
