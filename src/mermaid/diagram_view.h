#pragma once
#include "d2d_util.h"
#include "mermaid_util.h"
#include "tooltip.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <windows.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

struct ID2D1DeviceContext5;

namespace diagram_view {

// 全体表示の倍率。本文より拡大する窓なので、小さな図は等倍のまま置く。窓を余白より小さく潰したら最小の倍率で止める。
float FitZoom(float view_width, float view_height, float natural_width, float natural_height) noexcept;
// dir (+1 / -1) の向きの次の倍率。全体表示の倍率も止まる位置にし、そのすぐ隣の値は 1 回押しても見た目が変わらないので除く。
// 端なら nullopt。
std::optional<float> NextZoom(float zoom, float fit_zoom, int dir) noexcept;

// 画像の表示寸法 (DIP)。画像の 1 px を物理 1 px で描くので、本文の画像と 100% が揃う。
D2D1_SIZE_F ImageDipSize(UINT pixel_width, UINT pixel_height, float dpi_scale) noexcept;

struct DecodedImage {
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    UINT pixel_width = 0;
    UINT pixel_height = 0;
};

// 縮小デコードしても表示の原寸は保つ。ファイルを閉じた後も bitmap 単体で使える。
std::optional<DecodedImage> DecodeImage(IWICImagingFactory* wic, const std::wstring& path, UINT max_bitmap_dim);

// 高精度ホイールの端数を次のノッチに持ち越す。
struct WheelZoom {
    int remainder = 0;
    int Accumulate(int delta) noexcept;
};

} // namespace diagram_view

// MermaidRenderer が拡大表示の窓に頼む操作。本物の窓は開くと前面を取るので、テストは窓を作らない実装を差し込む。
class IDiagramViewer {
public:
    virtual ~IDiagramViewer() = default;
    // 本体と一緒に壊されていない。
    virtual bool IsOpen() const noexcept = 0;
    virtual bool HasDrawing() const noexcept = 0;
    virtual void BringToFront() const noexcept = 0;
    // タイトル・ボタンのツールチップ・読み込み中の文言を、表示中の言語にする。
    virtual void ApplyLanguage() = 0;
    virtual void SetDarkMode(bool dark) = 0;
    // 別の図を開いた。
    virtual void ShowLoading() = 0;
    // 図が出ていれば位置とボタンで変えた倍率を保つ (保存とテーマの切り替え)。全体表示のままなら新しい大きさに合わせ直す。
    virtual void SetDrawing(std::shared_ptr<const mermaid_util::DiagramDrawing> drawing) = 0;
    // 画像 (PNG など WIC が読める形式) を同じ窓に開く。読めなければ false (呼び出し側が窓を閉じる)。
    virtual bool LoadImage(const std::wstring& path) = 0;
    // 本体の描画デバイスを作り直した。次の描画から新しいデバイスで描く。
    virtual void SetDevice(ID3D11Device* d3d, ID2D1Device* device) = 0;
};

// 図と PNG 画像の拡大表示の窓。図の描画命令 (またはデコードした画像) を倍率を変えて描き、
// 右上のボタン・ホイール (スクロール。Ctrl を押しながらで倍率)・ドラッグ・キーで操作する。
// PopupWindow はキャプチャを取り、ホイールを本体へ渡し、非アクティブで閉じるので、見ながら操作するこの窓には使えない。
class DiagramView final : public IDiagramViewer {
public:
    // owner の中央に、owner の 9 割の大きさで「読み込み中」として開く。on_close は ×・Esc で呼ぶ (呼ばれた側がこれを壊す)。
    // 開けなければ null。
    // d3d と device は本体の描画デバイス。この窓のスワップチェーンを本体の D3D デバイスに作り、本体の D2D デバイスで描く。
    // 別のデバイス (既定の HwndRenderTarget) で描いていた頃は、開くたびに最初の描画まで 34〜40ms かかり、ドライバのデバイスの
    // 分の commit 約 30MB が閉じた後も残った。数式の形 (MathDrawing::Shape) はファクトリごとに作るので、本体と同じファクトリで
    // 描くと本体と交互に作り直さない。
    static std::unique_ptr<IDiagramViewer> Open(HWND owner, ID3D11Device* d3d, ID2D1Device* device, IDWriteFactory* dw, IWICImagingFactory* wic,
                                                bool dark, std::move_only_function<void()> on_close);
    // 窓も壊す。
    ~DiagramView() override;
    DiagramView(const DiagramView&) = delete;
    DiagramView& operator=(const DiagramView&) = delete;

    bool IsOpen() const noexcept override
    {
        return hwnd_ != nullptr;
    }
    // 図を出している (画像ではない)。
    bool HasDrawing() const noexcept override
    {
        return drawing_ != nullptr;
    }
    // 図か画像を出している。
    bool HasContent() const noexcept
    {
        return drawing_ != nullptr || image_wic_ != nullptr;
    }
    void BringToFront() const noexcept override;
    // タイトル・ボタンのツールチップ・読み込み中の文言を、表示中の言語にする。
    void ApplyLanguage() override;
    void SetDarkMode(bool dark) override;
    // 別の図を開いた。
    void ShowLoading() override;
    // 図が出ていれば位置とボタンで変えた倍率を保つ (保存とテーマの切り替え)。全体表示のままなら新しい大きさに合わせ直す。
    void SetDrawing(std::shared_ptr<const mermaid_util::DiagramDrawing> drawing) override;
    // 画像 (PNG など WIC が読める形式) を開く。UI スレッドで 1 度だけデコードする (1 回の操作で、大きさの上限で抑える)。
    // 読めなければ false (呼び出し側が窓を閉じる)。
    bool LoadImage(const std::wstring& path) override;
    void SetDevice(ID3D11Device* d3d, ID2D1Device* device) override;

private:
    static constexpr UINT_PTR kTooltipTimer = 2;
    // 並びは右上のバーの左から。
    enum class Button : uint8_t {
        None,
        ZoomOut,
        ZoomIn,
        Fit
    };

    DiagramView(ID3D11Device* d3d, ID2D1Device* device, IDWriteFactory* dw, IWICImagingFactory* wic, std::move_only_function<void()> on_close);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    // 扱ったメッセージは true を返し、result に戻り値を入れる。
    bool HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam, LRESULT& result);

    bool EnsureTarget();
    // 描画先とスワップチェーンを捨てる (デバイスロストとデバイスの差し替え)。次の描画で作り直す。
    void ReleaseTarget() noexcept;
    void Paint();
    void PaintBar();
    // 窓と中身の大きさから全体表示の倍率と、スクロールの範囲を決め直す。
    void Layout();
    void UpdateScrollBars();
    // 中身の原寸 (DIP)。図は CSS px、画像はピクセルを DPI の倍率で割った値。中身が無ければ 0。
    D2D1_SIZE_F ContentSize() const noexcept;
    // 画像の D2D ビットマップを描画先に載せる。描画先を作り直したら捨てるので、その都度作る。
    ID2D1Bitmap* ImageBitmap();
    // 画像の表示寸法 (DIP)。今の DPI で割るので、DPI が変わると Layout が新しい値で組み直す。
    D2D1_SIZE_F ImageSize() const noexcept
    {
        return diagram_view::ImageDipSize(image_pixel_width_, image_pixel_height_, Scale());
    }
    // 中身の左上を置く位置 (DIP)。中身が窓より小さい向きは中央に置く。
    D2D1_POINT_2F Origin() const noexcept;
    void ScrollTo(float x, float y);
    void ScrollBy(float dx, float dy)
    {
        ScrollTo(scroll_x_ + dx, scroll_y_ + dy);
    }
    // スクロールバーと同じ送り (SB_LINEUP など) で動かす。キーもこれを使う。
    void Scroll(int bar, WORD request);
    void Step(int dir);
    void Fit();
    float Zoom() const noexcept
    {
        return manual_zoom_.value_or(fit_zoom_);
    }
    // 窓の中央に見えている点を動かさずに倍率を変える。
    void SetZoom(float zoom);
    // anchor (窓の中の点、DIP) の下にある中身の点を動かさずに倍率を変える。ホイールはポインタの位置を渡す。
    void SetZoomAt(float zoom, D2D1_POINT_2F anchor);
    // ホイールのデルタで倍率を 1 ノッチずつ変える。anchor はポインタの位置。
    void ZoomByWheel(int delta, D2D1_POINT_2F anchor);
    Button HitButton(float x, float y) const noexcept;
    D2D1_RECT_F ButtonRect(Button button) const noexcept;
    D2D1_RECT_F BarRect() const noexcept;
    bool IsEnabled(Button button) const noexcept;
    // 押していない間に、ポインタの下のボタンを button にする。
    void HoverAt(Button button);
    // 塗るボタン (ポインタの下か、押している)。
    Button HighlightedButton() const noexcept;
    void Invalidate() const noexcept
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    float Scale() const noexcept;
    D2D1_POINT_2F ToDip(LPARAM lparam) const noexcept;

    // 窓を作り終えるまでと、本体と一緒に壊された後は null。
    HWND hwnd_ = nullptr;
    std::move_only_function<void()> on_close_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
    Microsoft::WRL::ComPtr<ID2D1Device> device_;
    WindowSwapTarget target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    // 図を記録する DeviceContext と記録したもの。描画先と同じデバイスから作り、描画先を作り直したら捨てる。
    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> record_dc_;
    Microsoft::WRL::ComPtr<ID2D1CommandList> commands_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_label_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_symbol_;

    std::shared_ptr<const mermaid_util::DiagramDrawing> drawing_;
    // デバイスを失ってもデコードし直さずに済むよう、CPU 側の画素も残す。
    Microsoft::WRL::ComPtr<IWICBitmapSource> image_wic_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> image_bitmap_;
    // 画像の原寸 (px)。DIP は DPI の倍率で割って求めるので、DPI が変わっても原寸はそのまま。
    UINT image_pixel_width_ = 0;
    UINT image_pixel_height_ = 0;
    // ホイールで倍率を変えるときの、次のノッチに持ち越す端数。
    diagram_view::WheelZoom wheel_zoom_;
    bool dark_ = false;
    Tooltip tooltip_;

    // 大きさと位置は DIP。
    float view_width_ = 0.0f;
    float view_height_ = 0.0f;
    float fit_zoom_ = 1.0f;
    // ボタンやキーで変えた倍率。無ければ全体表示で、窓の大きさに追従する。
    std::optional<float> manual_zoom_;
    // 図を余白込みで並べた面の、窓の左上が指す位置。
    float scroll_x_ = 0.0f;
    float scroll_y_ = 0.0f;
    // スクロールバーの出し入れで WM_SIZE から Layout に戻ってくる間は、外側の Layout に任せる。
    bool laying_out_ = false;

    // ポインタの段階。押している間 (Pressing / Dragging) はキャプチャを持つ。
    struct PointerIdle {};
    // 押した後も同じボタンの上ならツールチップを出し直さない (HoverAt は同じボタンでは何もしない)。
    struct PointerOver {
        Button button = Button::None;
    };
    // 離したときに同じボタンの上なら押したことにする。
    struct PointerPressing {
        Button button = Button::None;
    };
    // 図をつかんで動かしている。last は直前のポインタの位置で、動いた分だけスクロールする
    // (ドラッグ中にホイール・キー・倍率の変更・保存で動いた位置を、次の移動で戻さない)。
    struct PointerDragging {
        D2D1_POINT_2F last{};
    };
    std::variant<PointerIdle, PointerOver, PointerPressing, PointerDragging> pointer_;
    bool PointerDown() const noexcept
    {
        return std::holds_alternative<PointerPressing>(pointer_) || std::holds_alternative<PointerDragging>(pointer_);
    }
    bool tracking_leave_ = false;
};
