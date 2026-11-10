#include <gtest/gtest.h>
#include "document.h"
#include "recent_files.h"
#include <string>
#include <vector>

namespace {

std::vector<std::wstring> Paths(std::wstring_view dir, int count)
{
    std::vector<std::wstring> paths;
    for (int i = 0; i < count; ++i) {
        paths.push_back(std::wstring{ dir } + L"\\file" + std::to_wstring(i) + L".md");
    }
    return paths;
}

// 一覧は Add でだけ作る (設定から読み戻す経路は無い)。
void AddAll(RecentFiles& recent, const std::vector<std::wstring>& paths)
{
    for (const auto& p : paths) {
        recent.Add(p);
    }
}

} // namespace

// ═══════════════════════════════════════════════
// 一覧の規則
// ═══════════════════════════════════════════════

// mendo を開き直すと空から始まる (設定に残さない)。
TEST(RecentFilesTest, StartsEmpty)
{
    const RecentFiles recent;
    EXPECT_TRUE(recent.empty());
    EXPECT_EQ(recent.size(), 0u);
}

TEST(RecentFilesTest, AddPutsNewestFirst)
{
    const auto paths = Paths(L"C:\\docs", 3);
    RecentFiles recent;
    AddAll(recent, paths);
    ASSERT_EQ(recent.size(), 3u);
    EXPECT_EQ(recent.Entries()[0], paths[2]);
    EXPECT_EQ(recent.Entries()[1], paths[1]);
    EXPECT_EQ(recent.Entries()[2], paths[0]);
}

// 開き直したファイルは新しい項目にせず、先頭へ移す。大文字小文字だけ違うパスも同じファイルとして扱う。
TEST(RecentFilesTest, AddMovesExistingEntryToFront)
{
    RecentFiles recent;
    recent.Add(L"C:\\docs\\a.md");
    recent.Add(L"C:\\docs\\b.md");
    recent.Add(L"C:\\DOCS\\A.MD");
    ASSERT_EQ(recent.size(), 2u);
    EXPECT_EQ(recent.Entries()[0], L"C:\\DOCS\\A.MD");
    EXPECT_EQ(recent.Entries()[1], L"C:\\docs\\b.md");
}

TEST(RecentFilesTest, AddDropsOldestBeyondCap)
{
    const auto paths = Paths(L"C:\\docs", static_cast<int>(RecentFiles::kMaxEntries) + 3);
    RecentFiles recent;
    AddAll(recent, paths);
    ASSERT_EQ(recent.size(), RecentFiles::kMaxEntries);
    EXPECT_EQ(recent.Entries().front(), paths.back());
    EXPECT_EQ(recent.Entries().back(), paths[3]);
}

// 空・埋め込みの仮想パス・相対パス・UNC/デバイスパスは一覧に入れない。
TEST(RecentFilesTest, AddRejectsPathsThatCannotBeListed)
{
    for (const std::wstring_view path : { std::wstring_view{}, std::wstring_view{ HELP_PATH }, std::wstring_view{ LICENSES_PATH }, std::wstring_view{ L"readme.md" },
                                          std::wstring_view{ L"\\\\server\\share\\a.md" }, std::wstring_view{ L"\\\\?\\C:\\docs\\a.md" } }) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ path }));
        RecentFiles recent;
        recent.Add(path);
        EXPECT_TRUE(recent.empty());
    }
}

TEST(RecentFilesTest, RemoveAtAndClear)
{
    RecentFiles recent;
    recent.Add(L"C:\\docs\\a.md");
    recent.Add(L"C:\\docs\\b.md");
    recent.Add(L"C:\\docs\\c.md");
    ASSERT_EQ(recent.size(), 3u);
    EXPECT_FALSE(recent.RemoveAt(3));
    // 新しい順なので、1 番目は b.md。
    EXPECT_TRUE(recent.RemoveAt(1));
    EXPECT_EQ(recent.Entries(), (std::vector<std::wstring>{ L"C:\\docs\\c.md", L"C:\\docs\\a.md" }));
    recent.Clear();
    EXPECT_TRUE(recent.empty());
}

TEST(RecentFileLabelTest, SplitsNameAndDirectory)
{
    const auto label = MakeRecentFileLabel(L"C:\\docs\\sub\\note.md");
    EXPECT_EQ(label.name, L"note.md");
    EXPECT_EQ(label.directory, L"C:\\docs\\sub");
}
