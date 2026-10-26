#include "merman_abi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// 図の見た目は mermaid-js の neutral に合わせる。LunaSVG は merman が埋め込む
// <style> をそのまま適用するので、色は themeVariables だけで決める。
// neutral は明るい前提の配色なので、暗所では同じ構成のまま色だけ差し替える。
std::string ThemeVariables(bool dark_mode)
{
    std::string vars =
        "\"fontFamily\":\"Microsoft YaHei UI, Segoe UI, sans-serif\","
        "\"fontSize\":\"14px\"";
    if (!dark_mode) {
        return vars;
    }
    vars +=
        ",\"background\":\"#1e1e1e\""
        ",\"mainBkg\":\"#2d2d2d\""
        ",\"primaryColor\":\"#2d2d2d\""
        ",\"primaryBorderColor\":\"#555555\""
        ",\"primaryTextColor\":\"#d4d4d4\""
        ",\"secondaryColor\":\"#333333\""
        ",\"tertiaryColor\":\"#333333\""
        ",\"nodeBorder\":\"#555555\""
        ",\"nodeTextColor\":\"#d4d4d4\""
        ",\"textColor\":\"#d4d4d4\""
        ",\"titleColor\":\"#d4d4d4\""
        ",\"lineColor\":\"#d4d4d4\""
        ",\"clusterBkg\":\"#252526\""
        ",\"clusterBorder\":\"#555555\""
        ",\"edgeLabelBackground\":\"#252526\""
        ",\"labelBackgroundColor\":\"#252526\""
        ",\"labelBoxBkgColor\":\"#2d2d2d\""
        ",\"labelBoxBorderColor\":\"#555555\""
        ",\"labelTextColor\":\"#d4d4d4\""
        ",\"loopTextColor\":\"#d4d4d4\""
        ",\"actorBkg\":\"#2d2d2d\""
        ",\"actorBorder\":\"#555555\""
        ",\"actorTextColor\":\"#d4d4d4\""
        ",\"actorLineColor\":\"#555555\""
        ",\"signalColor\":\"#d4d4d4\""
        ",\"signalTextColor\":\"#d4d4d4\""
        ",\"noteBkgColor\":\"#3c3c3c\""
        ",\"noteTextColor\":\"#d4d4d4\""
        ",\"noteBorderColor\":\"#555555\""
        ",\"activationBkgColor\":\"#3c3c3c\""
        ",\"activationBorderColor\":\"#555555\""
        ",\"sequenceNumberColor\":\"#1e1e1e\""
        ",\"sectionBkgColor\":\"#252526\""
        ",\"altSectionBkgColor\":\"#2d2d2d\""
        ",\"sectionBkgColor2\":\"#252526\""
        ",\"taskBkgColor\":\"#2d2d2d\""
        ",\"taskBorderColor\":\"#555555\""
        ",\"taskTextColor\":\"#d4d4d4\""
        ",\"taskTextDarkColor\":\"#d4d4d4\""
        ",\"taskTextOutsideColor\":\"#d4d4d4\""
        ",\"activeTaskBkgColor\":\"#3c3c3c\""
        ",\"activeTaskBorderColor\":\"#555555\""
        ",\"gridColor\":\"#3c3c3c\""
        ",\"scaleColor\":\"#d4d4d4\""
        ",\"classText\":\"#d4d4d4\""
        ",\"fillType0\":\"#2d2d2d\""
        ",\"fillType1\":\"#333333\""
        ",\"pieTitleTextColor\":\"#d4d4d4\""
        ",\"pieSectionTextColor\":\"#d4d4d4\""
        ",\"pieLegendTextColor\":\"#d4d4d4\"";
    return vars;
}

MermanNativeSlice Slice(const void* data, size_t len)
{
    MermanNativeSlice slice{};
    slice.struct_size = MERMAN_NATIVE_STRUCT_SIZE(MermanNativeSlice);
    slice.data = static_cast<const uint8_t*>(data);
    slice.len = len;
    return slice;
}

std::string ResultError(const MermanNativeResult& result, MermanNativeStatus status)
{
    if (result.metadata_or_error_json.data && result.metadata_or_error_json.len) {
        return std::string(reinterpret_cast<const char*>(result.metadata_or_error_json.data), result.metadata_or_error_json.len);
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "merman status %d", static_cast<int>(status));
    return buf;
}

} // namespace

MermanAbi::~MermanAbi()
{
    Shutdown();
}

bool MermanAbi::Initialize(std::string& error)
{
    std::lock_guard lock(mutex_);
    if (ready_) {
        return true;
    }

    MermanNativeApiRequest discovery{};
    discovery.struct_size = MERMAN_NATIVE_STRUCT_SIZE(MermanNativeApiRequest);
    discovery.expected_abi_version = MERMAN_NATIVE_ABI_VERSION;
    constexpr char kDigest[] = MERMAN_NATIVE_ABI_MINIMUM_PREFIX_LAYOUT_DIGEST;
    discovery.expected_minimum_prefix_layout_digest = Slice(kDigest, sizeof(kDigest) - 1);

    api_ = {};
    api_.struct_size = MERMAN_NATIVE_STRUCT_SIZE(MermanNativeApi);
    const MermanNativeStatus discovery_status = merman_get_native_api(&discovery, &api_);
    if (discovery_status != MERMAN_NATIVE_STATUS_OK) {
        error = "merman ABI discovery failed: status " + std::to_string(static_cast<int>(discovery_status));
        return false;
    }
    if (api_.struct_size < MERMAN_NATIVE_API_MINIMUM_PREFIX_SIZE || !api_.engine_new || !api_.execute_collect ||
        !api_.result_free || !api_.engine_try_close) {
        error = "merman ABI is missing required entry points";
        api_ = {};
        return false;
    }

    MermanNativeEngineConfig config{};
    config.struct_size = MERMAN_NATIVE_STRUCT_SIZE(MermanNativeEngineConfig);
    config.options_json = Slice(nullptr, 0);
    config.text_measure = nullptr;
    config.text_measure_user_data = nullptr;

    MermanNativeEngineToken engine = 0;
    MermanNativeResult result = MERMAN_NATIVE_RESULT_INIT;
    const MermanNativeStatus status = api_.engine_new(&config, &engine, &result);
    if (status != MERMAN_NATIVE_STATUS_OK || engine == 0) {
        error = "merman engine creation failed: " + ResultError(result, status);
        api_.result_free(&result);
        api_ = {};
        return false;
    }
    api_.result_free(&result);
    engine_ = engine;
    ready_ = true;
    return true;
}

void MermanAbi::Shutdown()
{
    std::lock_guard lock(mutex_);
    if (engine_ && api_.engine_try_close) {
        // 呼び出し側がレンダリング完了を待ってから呼ぶ。busy の場合はリークより
        // プロセス終了時の解放に任せる。
        (void)api_.engine_try_close(engine_);
    }
    engine_ = 0;
    ready_ = false;
    api_ = {};
}

bool MermanAbi::RenderSvg(std::string_view code, float max_width, bool dark_mode, std::string& svg, std::string& error)
{
    std::lock_guard lock(mutex_);
    if (!ready_ || engine_ == 0) {
        error = "merman engine is not initialized";
        return false;
    }

    const float width = std::max(1.0f, std::isfinite(max_width) ? max_width : 1.0f);
    char layout[96];
    std::snprintf(layout, sizeof(layout), "\"layout\":{\"container_width\":%.2f},", width);
    std::string options = "{\"version\":2,";
    options += layout;
    options += "\"site_config\":{\"htmlLabels\":false,\"theme\":\"neutral\",\"look\":\"classic\",\"themeVariables\":{";
    options += ThemeVariables(dark_mode);
    options += "}},\"svg\":{\"pipeline\":\"resvg-safe\"}}";

    MermanNativeOperationRequest request{};
    request.struct_size = MERMAN_NATIVE_STRUCT_SIZE(MermanNativeOperationRequest);
    request.operation = MERMAN_NATIVE_OPERATION_SVG;
    request.source = Slice(code.data(), code.size());
    request.uri = Slice(nullptr, 0);
    request.options_json = Slice(options.data(), options.size());

    MermanNativeResult result = MERMAN_NATIVE_RESULT_INIT;
    const MermanNativeStatus status = api_.execute_collect(engine_, &request, &result);
    if (status != MERMAN_NATIVE_STATUS_OK) {
        error = ResultError(result, status);
        api_.result_free(&result);
        return false;
    }
    if (!result.data.data || result.data.len == 0) {
        error = "merman returned an empty SVG";
        api_.result_free(&result);
        return false;
    }
    svg.assign(reinterpret_cast<const char*>(result.data.data), result.data.len);
    api_.result_free(&result);
    return true;
}
