#pragma once
#include "i18n.h"
#include "popup_window.h"
#include "recent_files.h"
#include "ui_types.h"
#include <cstddef>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

struct RecentFilesDialogParams {
    PopupEnv env;
    // 表示する一覧 (新しい順)。App が持つものを指し、外した結果はコールバックで App が書き換える。
    // ダイアログは毎回ここから読み直すので、写しを持たない。
    const RecentFiles* files = nullptr;
    std::function<void(size_t index)> on_remove;
    std::function<void()> on_clear;
    std::function<void(std::wstring_view path)> on_open;
};

namespace recent_files_dialog_constants {
inline constexpr float MIN_WIDTH = 340.0f;
inline constexpr float MAX_WIDTH = 560.0f;
inline constexpr float PAD_X = 16.0f;
inline constexpr float TITLE_HEIGHT = 44.0f;
inline constexpr float TITLE_FONT_SCALE = 1.25f;
// 1 行 = ファイル名 + 親フォルダの 2 段。
inline constexpr float ROW_HEIGHT = 44.0f;
inline constexpr float DIR_FONT_SCALE = 0.85f;
inline constexpr float NAME_TOP_PAD = 5.0f;
inline constexpr float NAME_LINE_HEIGHT = 19.0f;
inline constexpr float DIR_TOP_GAP = 1.0f;
inline constexpr float DIR_BOTTOM_PAD = 6.0f;
// 行末の × ボタンと、その分の本文の右余白。
inline constexpr float REMOVE_SIZE = 22.0f;
inline constexpr float REMOVE_RIGHT_PAD = 8.0f;
inline constexpr float REMOVE_RESERVE = REMOVE_SIZE + REMOVE_RIGHT_PAD * 2.0f;
inline constexpr float HOVER_MARGIN_X = 6.0f;
inline constexpr float EMPTY_ROW_HEIGHT = 72.0f;
inline constexpr float FOOTER_PAD_Y = 8.0f;
inline constexpr float FOOTER_BUTTON_HEIGHT = 28.0f;
inline constexpr float EMPTY_BOTTOM_PAD = 16.0f;
inline constexpr float BUTTON_PAD_X = 14.0f;
inline constexpr float BUTTON_MIN_WIDTH = 88.0f;
inline constexpr float ICON_FONT_SIZE = 12.0f;

inline constexpr wchar_t GLYPH_REMOVE[] = L"\xE711";

// ファイル名は末尾を、親フォルダは最後の区切りより手前を省く (一番近いフォルダ名を残す)。
inline constexpr wchar_t ELLIPSIS_KEEP_DIR[] = L"\\";
} // namespace recent_files_dialog_constants

// 最近開いたファイルの一覧。本体と同じ色で描くため自前描画で、開くたびに作る。
// テストはウィンドウを作らずに各段階を直接呼ぶ。
struct RecentFilesDialog : PopupWindow<RecentFilesDialog> {
    static constexpr wchar_t kClassName[] = L"mendoRecentFilesDialog";

    // 閉じるまで戻らない (入力はダイアログが取る)。
    static void Show(HWND owner_hwnd, const RecentFilesDialogParams& params);

    enum class HitKind : uint8_t {
        None,
        Entry,
        Remove,
        Clear,
    };
    struct Hit {
        HitKind kind = HitKind::None;
        // Entry / Remove のときの行。
        size_t row = 0;
        bool operator==(const Hit&) const = default;
    };

    // 大きさは PopupWindow::width / height。
    struct Layout {
        DipRect title{};
        // 一覧の並び。entries と同じ添字で、空なら空。
        std::vector<DipRect> rows;
        // 行の × ボタン。rows と同じ添字。
        std::vector<DipRect> remove;
        // 一覧が空のときの文言。
        DipRect empty{};
        float footer_line_y = 0.0f;
        DipRect clear{};
    };

    // 一覧と文言から、大きさ・行の矩形・書式を組み直す。空の一覧でも呼べる。
    void Rebuild();
    void CreateTextFormats();
    void ComputeLayout();
    Hit HitTest(float x, float y) const noexcept;
    // 押した/キーで決めた項目を実行する。開くならダイアログを閉じる。
    void Activate(const Hit& hit);
    // 一覧が変わった後に、窓の大きさを今の中心のまま合わせる。テストはウィンドウを作らずに呼ぶ。
    void ResizeToFit();

    // 選択中の行 (キーボード)。一覧が空なら -1。
    constexpr int Selected() const noexcept
    {
        return selected_;
    }
    void MoveSelection(int delta) noexcept;
    void SelectEdge(bool first) noexcept;

    // PopupWindow から呼ばれる。
    void PaintContent();
    bool UpdateHover(float x, float y);
    void OnPress(UINT msg, float x, float y);
    // 方向キー・Delete・Enter を扱う。false なら土台の既定 (Esc/Enter で閉じる) に回す。
    bool OnKeyDown(WPARAM key);

    Hit hovered{};
    int selected_ = 0;
    const RecentFiles* files_ = nullptr;
    std::function<void(size_t)> on_remove;
    std::function<void()> on_clear;
    std::function<void(std::wstring_view)> on_open;
    Layout layout{};

    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_title;
    // ファイル名と親フォルダ。収まらない行は省略記号で切る。
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_name;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_dir;
};
