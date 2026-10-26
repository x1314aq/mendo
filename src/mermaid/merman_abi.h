#pragma once
#include "merman.h"
#include <mutex>
#include <string>
#include <string_view>

// Merman 0.9.2 のプリビルド C ABI (SVG 出力) を薄くラップする。
// エンジンは 1 個を共有し、呼び出しは直列化する。
class MermanAbi {
public:
    MermanAbi() = default;
    ~MermanAbi();

    MermanAbi(const MermanAbi&) = delete;
    MermanAbi& operator=(const MermanAbi&) = delete;

    bool Initialize(std::string& error);
    void Shutdown();
    bool IsReady() const noexcept
    {
        return ready_;
    }

    // code は UTF-8。戻り値の svg は UTF-8 の SVG 文字列。
    bool RenderSvg(std::string_view code, float max_width, std::string_view theme, std::string& svg, std::string& error);

private:
    MermanNativeApi api_{};
    MermanNativeEngineToken engine_ = 0;
    bool ready_ = false;
    std::mutex mutex_;
};
