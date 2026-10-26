#pragma once
#include <string_view>

namespace mermaid_theme {

constexpr std::string_view Normalize(std::string_view theme) noexcept
{
    constexpr std::string_view kThemes[] = {
        "default", "base", "dark", "forest", "neutral", "neo", "neo-dark",
        "redux", "redux-dark", "redux-color", "redux-dark-color",
    };
    for (const auto name : kThemes) {
        if (theme == name) {
            return name;
        }
    }
    return "default";
}

constexpr std::string_view Resolve(std::string_view theme, bool dark_mode) noexcept
{
    theme = Normalize(theme);
    if (!dark_mode) {
        return theme;
    }
    if (theme == "neo") {
        return "neo-dark";
    }
    if (theme == "redux") {
        return "redux-dark";
    }
    if (theme == "redux-color") {
        return "redux-dark-color";
    }
    if (theme == "default" || theme == "base" || theme == "forest" || theme == "neutral") {
        return "dark";
    }
    return theme;
}

} // namespace mermaid_theme
