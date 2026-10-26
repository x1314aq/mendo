#include "merman_abi.h"
#include "mermaid_theme.h"
#include "mermaid_svg.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

std::string_view TextSlice(const MermanNativeSlice& slice)
{
    return slice.data ? std::string_view{ reinterpret_cast<const char*>(slice.data), slice.len } : std::string_view{};
}

MermanNativeStatus MeasureText(const MermanNativeTextMeasureRequest* request, MermanNativeTextMeasureResult* result, void*) noexcept
{
    *result = {};
    result->struct_size = MERMAN_NATIVE_STRUCT_SIZE(MermanNativeTextMeasureResult);
    if (request->text_measurement_protocol_version != MERMAN_TEXT_MEASUREMENT_PROTOCOL_VERSION ||
        request->operation != MERMAN_TEXT_MEASUREMENT_OPERATION_MERMAID_CALCULATE_TEXT_DIMENSIONS ||
        request->has_max_width || request->letter_spacing != 0.0 || request->word_spacing != 0.0) {
        return MERMAN_NATIVE_STATUS_OK;
    }
    try {
        const auto metrics = MeasureMermaidText(TextSlice(request->text), request->font_size,
                                                TextSlice(request->font_weight), TextSlice(request->font_style));
        if (metrics) {
            result->handled = 1;
            result->result_kind = MERMAN_TEXT_MEASUREMENT_RESULT_KIND_METRICS;
            result->width = metrics->width;
            result->height = std::max(metrics->height, request->line_height * metrics->line_count);
            result->line_count = metrics->line_count;
        }
    }
    catch (...) {
        // C ABI の外へ例外を出さず、計測できなければ merman の既定計測に任せる。
    }
    return MERMAN_NATIVE_STATUS_OK;
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
    config.text_measure = MeasureText;
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

bool MermanAbi::RenderSvg(std::string_view code, float max_width, std::string_view theme, std::string& svg, std::string& error)
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
    options += "\"site_config\":{\"htmlLabels\":false,\"theme\":\"";
    options += mermaid_theme::Normalize(theme);
    options += "\",\"look\":\"classic\",\"themeVariables\":{"
               "\"fontFamily\":\"Microsoft YaHei UI, Segoe UI, sans-serif\",\"fontSize\":\"14px\"";
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
