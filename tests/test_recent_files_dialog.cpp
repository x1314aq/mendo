#include <gtest/gtest.h>
#include "recent_files_dialog.h"
#include "test_helpers.h"
#include "theme.h"
#include <dwrite.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace recent_files_dialog_constants;
using Hit = RecentFilesDialog::Hit;
using HitKind = RecentFilesDialog::HitKind;

// CreateTextFormats / ComputeLayout は DWrite だけを使う。D2D は Show で描くときにしか要らない。
class RecentFilesDialogTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        dialog_.dwrite_factory = SharedDWriteFactory();
        ASSERT_NE(dialog_.dwrite_factory, nullptr);
        dialog_.theme = &theme_;
        for (int i = 0; i < 3; ++i) {
            files_.Add(L"C:\\docs\\sub\\note" + std::to_wstring(i) + L".md");
        }
        dialog_.files_ = &files_;
        dialog_.Rebuild();
    }

    Hit HitAt(const DipRect& rect) const
    {
        const auto [x, y] = RectCenter(rect);
        return dialog_.HitTest(x, y);
    }

    ScopedLanguage language_{ &i18n::kJa };
    Theme theme_ = GetLightTheme();
    RecentFiles files_;
    RecentFilesDialog dialog_;
};

TEST_F(RecentFilesDialogTest, RowsStackUnderTitleAndAboveFooter)
{
    const auto& l = dialog_.layout;
    ASSERT_EQ(l.rows.size(), files_.size());
    EXPECT_GE(dialog_.width, MIN_WIDTH);
    EXPECT_LE(dialog_.width, MAX_WIDTH);
    EXPECT_GE(l.rows.front().top, l.title.bottom);
    for (size_t i = 1; i < l.rows.size(); ++i) {
        EXPECT_FLOAT_EQ(l.rows[i].top, l.rows[i - 1].bottom);
    }
    EXPECT_LE(l.rows.back().bottom, l.footer_line_y);
    EXPECT_GT(l.clear.top, l.footer_line_y);
    EXPECT_LE(l.clear.right, dialog_.width - PAD_X);
    EXPECT_LT(l.clear.bottom, dialog_.height);
    // × は行の中に収まる。
    for (size_t i = 0; i < l.rows.size(); ++i) {
        EXPECT_LE(l.remove[i].right, l.rows[i].right);
        EXPECT_GE(l.remove[i].top, l.rows[i].top);
        EXPECT_LE(l.remove[i].bottom, l.rows[i].bottom);
    }
}

TEST_F(RecentFilesDialogTest, HitTestFindsRowRemoveAndClear)
{
    const auto& l = dialog_.layout;
    EXPECT_EQ(dialog_.HitTest(dialog_.width - 4.0f, 4.0f), Hit{});
    for (size_t i = 0; i < l.rows.size(); ++i) {
        EXPECT_EQ(HitAt({ l.rows[i].left + PAD_X, l.rows[i].top, l.rows[i].left + PAD_X + 10.0f, l.rows[i].top + 10.0f }), (Hit{ HitKind::Entry, i }));
        EXPECT_EQ(HitAt(l.remove[i]), (Hit{ HitKind::Remove, i }));
    }
    EXPECT_EQ(HitAt(l.clear), (Hit{ HitKind::Clear }));
}

TEST_F(RecentFilesDialogTest, EmptyListShowsNoRowsAndNoClearButton)
{
    RecentFiles empty;
    dialog_.files_ = &empty;
    dialog_.Rebuild();
    EXPECT_TRUE(dialog_.layout.rows.empty());
    EXPECT_TRUE(dialog_.layout.remove.empty());
    EXPECT_EQ(dialog_.Selected(), -1);
    EXPECT_EQ(dialog_.HitTest(RectCenter(dialog_.layout.empty).x, RectCenter(dialog_.layout.empty).y), Hit{});
    // 一覧を消去は押せない。
    EXPECT_EQ(HitAt(dialog_.layout.clear), Hit{});
}

TEST_F(RecentFilesDialogTest, RemoveCallsBackAndKeepsDialogOpen)
{
    std::vector<size_t> removed;
    dialog_.on_remove = [&](size_t i) {
        removed.push_back(i);
        files_.RemoveAt(i);
    };
    dialog_.Activate({ HitKind::Remove, 1 });
    EXPECT_EQ(removed, std::vector<size_t>{ 1 });
    EXPECT_EQ(files_.size(), 2u);
    EXPECT_FALSE(dialog_.done);
    // 組み直した後の行数も追随する。
    EXPECT_EQ(dialog_.layout.rows.size(), 2u);
}

TEST_F(RecentFilesDialogTest, ClearCallsBackAndEmptiesLayout)
{
    int cleared = 0;
    dialog_.on_clear = [&] {
        ++cleared;
        files_.Clear();
    };
    dialog_.Activate({ HitKind::Clear });
    EXPECT_EQ(cleared, 1);
    EXPECT_TRUE(dialog_.layout.rows.empty());
    EXPECT_EQ(dialog_.Selected(), -1);
    EXPECT_FALSE(dialog_.done);
}

TEST_F(RecentFilesDialogTest, OpeningRowClosesAndReportsPath)
{
    std::wstring opened;
    dialog_.on_open = [&](std::wstring_view path) { opened = path; };
    const std::wstring expected = files_.Entries()[2];
    dialog_.Activate({ HitKind::Entry, 2 });
    EXPECT_EQ(opened, expected);
    EXPECT_TRUE(dialog_.done);
}

TEST_F(RecentFilesDialogTest, KeyboardMovesSelectionAndOpens)
{
    std::wstring opened;
    dialog_.on_open = [&](std::wstring_view path) { opened = path; };
    EXPECT_EQ(dialog_.Selected(), 0);
    EXPECT_TRUE(dialog_.OnKeyDown(VK_DOWN));
    EXPECT_EQ(dialog_.Selected(), 1);
    EXPECT_TRUE(dialog_.OnKeyDown(VK_END));
    EXPECT_EQ(dialog_.Selected(), 2);
    // 端で止まる。
    dialog_.OnKeyDown(VK_DOWN);
    EXPECT_EQ(dialog_.Selected(), 2);
    EXPECT_TRUE(dialog_.OnKeyDown(VK_HOME));
    EXPECT_EQ(dialog_.Selected(), 0);
    // 使わないキーは土台に回す。
    EXPECT_FALSE(dialog_.OnKeyDown(VK_ESCAPE));

    dialog_.selected_ = 1;
    const std::wstring expected = files_.Entries()[1];
    EXPECT_TRUE(dialog_.OnKeyDown(VK_RETURN));
    EXPECT_EQ(opened, expected);
    EXPECT_TRUE(dialog_.done);
}

TEST_F(RecentFilesDialogTest, DeleteRemovesSelectedRow)
{
    std::vector<size_t> removed;
    dialog_.on_remove = [&](size_t i) {
        removed.push_back(i);
        files_.RemoveAt(i);
    };
    dialog_.selected_ = 1;
    EXPECT_TRUE(dialog_.OnKeyDown(VK_DELETE));
    EXPECT_EQ(removed, std::vector<size_t>{ 1 });
    EXPECT_EQ(files_.size(), 2u);
    EXPECT_FALSE(dialog_.done);
}

TEST_F(RecentFilesDialogTest, ClickingRowSelectsAndOutsideCloses)
{
    const auto click = [this](int x, int y) { dialog_.HandleMessage(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y)); };
    const auto [rx, ry] = RectCenter(dialog_.layout.rows[2]);
    click(static_cast<int>(rx), static_cast<int>(ry));
    EXPECT_TRUE(dialog_.done);
    EXPECT_EQ(dialog_.selected_, 2);

    // 範囲外を押しても閉じる。
    dialog_.done = false;
    dialog_.on_open = [](std::wstring_view) {};
    click(-5, 10);
    EXPECT_TRUE(dialog_.done);
}

// どの言語の見出しでも、一覧の幅に収まる (幅は上限で頭打ちにする)。
TEST_F(RecentFilesDialogTest, WidthFitsLongestTextInEveryLanguage)
{
    for (const i18n::Strings* language : i18n::kAllStrings) {
        SCOPED_TRACE(std::wstring(language->lang_key));
        i18n::SetLanguage(language);
        dialog_.Rebuild();
        const float title_w = dialog_.Measure(language->recent_files_title, dialog_.fmt_title.Get());
        EXPECT_LE(title_w, dialog_.width - 2 * PAD_X + 0.5f);
        const float clear_w = dialog_.Measure(language->recent_files_clear, dialog_.fmt_text.Get());
        EXPECT_LE(clear_w, dialog_.layout.clear.right - dialog_.layout.clear.left + 0.5f);
    }
}

// 長いパスでも上限を超えて広がらない。
TEST_F(RecentFilesDialogTest, DoesNotExceedMaxWidthForVeryLongPaths)
{
    RecentFiles long_paths;
    for (int i = 0; i < 3; ++i) {
        std::wstring path = L"C:\\";
        for (int d = 0; d < 20; ++d) {
            path += L"very_long_directory_name_" + std::to_wstring(d) + L"\\";
        }
        path += L"file" + std::to_wstring(i) + L".md";
        long_paths.Add(path);
    }
    dialog_.files_ = &long_paths;
    dialog_.Rebuild();
    EXPECT_FLOAT_EQ(dialog_.width, MAX_WIDTH);
}
