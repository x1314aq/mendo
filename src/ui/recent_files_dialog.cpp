#include "recent_files_dialog.h"
#include <algorithm>
#include <cmath>

using namespace recent_files_dialog_constants;

namespace {
// 一覧が変われば消える行もあるので、選択は毎回今の件数へ収める。
int ClampSelection(int selected, size_t count) noexcept
{
    if (count == 0) {
        return -1;
    }
    return std::clamp(selected < 0 ? 0 : selected, 0, static_cast<int>(count) - 1);
}
} // namespace

void RecentFilesDialog::CreateTextFormats()
{
    const wchar_t* const family = theme->font_family.c_str();
    const wchar_t* const locale = i18n::S().lang_locale;
    const float size = theme->pane_font_size;
    constexpr auto CENTER = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    fmt_title = CreateTextFormat(dwrite_factory,
                                 { .family = family, .weight = DWRITE_FONT_WEIGHT_SEMI_BOLD, .size = size * TITLE_FONT_SCALE, .locale = locale, .para_align = CENTER, .no_wrap = true });
    // ファイル名は末尾を、親フォルダは最後の区切りの手前を省く (一番近いフォルダ名を残す)。
    // 行の幅を超えた分は DWrite の省略記号に任せる。
    fmt_name = CreateTextFormat(
        dwrite_factory, { .family = family, .size = size, .locale = i18n::TextLocale(), .para_align = CENTER, .no_wrap = true, .ellipsis = true });
    fmt_dir = CreateTextFormat(dwrite_factory, { .family = family,
                                                 .size = size * DIR_FONT_SCALE,
                                                 .locale = i18n::TextLocale(),
                                                 .para_align = CENTER,
                                                 .no_wrap = true,
                                                 .ellipsis = true,
                                                 .ellipsis_keep_after = ELLIPSIS_KEEP_DIR[0] });
    CreateBaseTextFormats(ICON_FONT_SIZE);
}

void RecentFilesDialog::Rebuild()
{
    CreateTextFormats();
    selected_ = ClampSelection(selected_, files_ ? files_->size() : 0);
    ComputeLayout();
}

void RecentFilesDialog::ComputeLayout()
{
    const auto& ls = i18n::S();
    const bool empty = !files_ || files_->empty();
    const std::vector<std::wstring>* entries = files_ ? &files_->Entries() : nullptr;

    Layout l;
    // 幅は一番長い名前と親フォルダで決める。長いパスで画面を覆わないよう上限を設け、収まらない行は省略する。
    float content_w = std::max(Measure(ls.recent_files_title, fmt_title.Get()), Measure(ls.recent_files_clear, fmt_text.Get()));
    if (empty) {
        content_w = std::max(content_w, Measure(ls.recent_files_empty, fmt_text.Get()));
    }
    else {
        for (const auto& path : *entries) {
            const auto label = MakeRecentFileLabel(path);
            content_w = std::max({ content_w, Measure(label.name, fmt_name.Get()) + REMOVE_RESERVE, Measure(label.directory, fmt_dir.Get()) + REMOVE_RESERVE });
        }
    }
    const float button_w = std::max(BUTTON_MIN_WIDTH, Measure(ls.recent_files_clear, fmt_text.Get()) + 2 * BUTTON_PAD_X);
    const float w = std::clamp(std::ceil(content_w + 2 * PAD_X), MIN_WIDTH, MAX_WIDTH);
    width = w;
    l.title = { PAD_X, 0.0f, w - PAD_X, TITLE_HEIGHT };

    float y = TITLE_HEIGHT;
    if (empty) {
        l.empty = { PAD_X, y, w - PAD_X, y + EMPTY_ROW_HEIGHT };
        y += EMPTY_ROW_HEIGHT;
    }
    else {
        for (size_t i = 0; i < entries->size(); ++i) {
            l.rows.push_back({ 0.0f, y, w, y + ROW_HEIGHT });
            const float right = w - REMOVE_RIGHT_PAD;
            const float top = y + (ROW_HEIGHT - REMOVE_SIZE) * 0.5f;
            l.remove.push_back({ right - REMOVE_SIZE, top, right, top + REMOVE_SIZE });
            y += ROW_HEIGHT;
        }
    }

    l.footer_line_y = y;
    l.clear = { w - PAD_X - button_w, y + FOOTER_PAD_Y, w - PAD_X, y + FOOTER_PAD_Y + FOOTER_BUTTON_HEIGHT };
    // 一覧が空でも「一覧を消去」は置かない (消すものが無い)。
    height = empty ? y + EMPTY_BOTTOM_PAD : l.clear.bottom + FOOTER_PAD_Y;
    layout = std::move(l);
}

RecentFilesDialog::Hit RecentFilesDialog::HitTest(float x, float y) const noexcept
{
    for (size_t i = 0; i < layout.rows.size(); ++i) {
        if (!PointInRect(x, y, layout.rows[i])) {
            continue;
        }
        // × は行の中にあるので、行より先に見る。
        return PointInRect(x, y, layout.remove[i]) ? Hit{ HitKind::Remove, i } : Hit{ HitKind::Entry, i };
    }
    if (files_ && !files_->empty() && PointInRect(x, y, layout.clear)) {
        return { HitKind::Clear };
    }
    return {};
}

void RecentFilesDialog::Activate(const Hit& hit)
{
    switch (hit.kind) {
    case HitKind::None:
        return;
    case HitKind::Entry: {
        if (!files_ || hit.row >= files_->size()) {
            return;
        }
        // 開く前に閉じる。on_open は読み込みを始めるので、モーダルループを抜けてから本体に任せる。
        const std::wstring path = files_->Entries()[hit.row];
        done = true;
        if (on_open) {
            on_open(path);
        }
        return;
    }
    case HitKind::Remove:
        if (!files_ || hit.row >= files_->size()) {
            return;
        }
        if (on_remove) {
            on_remove(hit.row);
        }
        Rebuild();
        ResizeToFit();
        return;
    case HitKind::Clear:
        if (!files_ || files_->empty()) {
            return;
        }
        if (on_clear) {
            on_clear();
        }
        Rebuild();
        ResizeToFit();
        return;
    }
}

void RecentFilesDialog::ResizeToFit()
{
    if (!hwnd) {
        return;
    }
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    Place(CenteredIn(rc));
}

void RecentFilesDialog::MoveSelection(int delta) noexcept
{
    const size_t count = files_ ? files_->size() : 0;
    if (count == 0) {
        selected_ = -1;
        return;
    }
    selected_ = ClampSelection(selected_ + delta, count);
}

void RecentFilesDialog::SelectEdge(bool first) noexcept
{
    const size_t count = files_ ? files_->size() : 0;
    selected_ = count == 0 ? -1 : first ? 0 : static_cast<int>(count) - 1;
}

bool RecentFilesDialog::OnKeyDown(WPARAM key)
{
    switch (key) {
    case VK_UP:
        MoveSelection(-1);
        return true;
    case VK_DOWN:
        MoveSelection(1);
        return true;
    case VK_HOME:
        SelectEdge(true);
        return true;
    case VK_END:
        SelectEdge(false);
        return true;
    case VK_DELETE:
        if (selected_ >= 0) {
            Activate({ HitKind::Remove, static_cast<size_t>(selected_) });
        }
        return true;
    case VK_RETURN:
        if (selected_ < 0) {
            return false;
        }
        Activate({ HitKind::Entry, static_cast<size_t>(selected_) });
        return true;
    default:
        return false;
    }
}

bool RecentFilesDialog::UpdateHover(float x, float y)
{
    const Hit hit = HitTest(x, y);
    if (hit == hovered) {
        return false;
    }
    hovered = hit;
    return true;
}

void RecentFilesDialog::OnPress(UINT msg, float x, float y)
{
    if (msg != WM_LBUTTONDOWN) {
        return;
    }
    const Hit hit = HitTest(x, y);
    if (hit.kind == HitKind::Entry) {
        selected_ = static_cast<int>(hit.row);
    }
    Activate(hit);
}

void RecentFilesDialog::Show(HWND owner_hwnd, const RecentFilesDialogParams& params)
{
    RecentFilesDialog d;
    if (!d.Setup(params.env)) {
        return;
    }
    d.files_ = params.files;
    d.on_remove = params.on_remove;
    d.on_clear = params.on_clear;
    d.on_open = params.on_open;
    d.Rebuild();
    RECT owner_rc{};
    GetWindowRect(owner_hwnd, &owner_rc);
    d.RunModal(owner_hwnd, i18n::S().recent_files_title, d.CenteredIn(owner_rc));
}

void RecentFilesDialog::PaintContent()
{
    const auto& ls = i18n::S();
    const float w = width;

    DrawLabel(ls.recent_files_title, fmt_title.Get(), layout.title, brushes.text.Get());
    if (!files_ || files_->empty()) {
        DrawLabel(ls.recent_files_empty, fmt_text.Get(), layout.empty, brushes.sub.Get());
        return;
    }

    const auto& entries = files_->Entries();
    for (size_t i = 0; i < entries.size(); ++i) {
        const DipRect& row = layout.rows[i];
        const bool hovered_row = hovered.kind == HitKind::Entry && hovered.row == i;
        const bool hovered_remove = hovered.kind == HitKind::Remove && hovered.row == i;
        const bool selected = selected_ == static_cast<int>(i);
        if (hovered_row || selected) {
            FillHover({ HOVER_MARGIN_X, row.top + 1.0f, w - HOVER_MARGIN_X, row.bottom - 1.0f });
        }

        const auto label = MakeRecentFileLabel(entries[i]);
        const float text_left = PAD_X;
        const float text_right = layout.remove[i].left - PAD_X;
        const float name_top = row.top + NAME_TOP_PAD;
        const float name_bottom = name_top + NAME_LINE_HEIGHT;
        DrawLabel(label.name, fmt_name.Get(), { text_left, name_top, text_right, name_bottom }, brushes.text.Get());
        DrawLabel(label.directory, fmt_dir.Get(), { text_left, name_bottom + DIR_TOP_GAP, text_right, row.bottom - DIR_BOTTOM_PAD }, brushes.sub.Get());

        // × は行を指している間 (またはキーボードで選んでいる間) だけ出す。
        if (hovered_row || hovered_remove || selected) {
            if (hovered_remove) {
                FillHover(layout.remove[i]);
            }
            DrawLabel(GLYPH_REMOVE, fmt_icon.Get(), layout.remove[i], brushes.gray.Get());
        }
    }

    rt->DrawLine({ PAD_X, layout.footer_line_y }, { w - PAD_X, layout.footer_line_y }, brushes.border.Get(), 1.0f);
    if (hovered.kind == HitKind::Clear) {
        FillHover(layout.clear);
    }
    const D2D1_RECT_F border{ layout.clear.left + 0.5f, layout.clear.top + 0.5f, layout.clear.right - 0.5f, layout.clear.bottom - 0.5f };
    rt->DrawRoundedRectangle({ border, POPUP_HOVER_CORNER, POPUP_HOVER_CORNER }, brushes.border.Get(), 1.0f);
    DrawLabel(ls.recent_files_clear, fmt_text_center.Get(), layout.clear, brushes.text.Get());
}
