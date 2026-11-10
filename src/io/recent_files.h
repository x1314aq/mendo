#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// 最近開いた Markdown ファイルの一覧 (新しい順)。プロセスの中だけで持ち、設定には保存しない
// (mendo を開き直すと空から始まる)。I/O を持たないので、ダイアログの表示内容を単体で試せる。
class RecentFiles {
public:
    // 一覧に残す件数。ダイアログは全件を一度に並べる (スクロールさせない) ので、画面に収まる数にする。
    static constexpr size_t kMaxEntries = 10;

    // 空・埋め込みの仮想パス・ネットワーク/デバイスパスは一覧に入れない。既にあれば先頭へ移すだけ。
    // 上限を超えた分は古いものから捨てる。
    void Add(std::wstring_view path);
    // 範囲外なら false。
    bool RemoveAt(size_t index);
    void Clear();

    constexpr const std::vector<std::wstring>& Entries() const noexcept
    {
        return entries_;
    }
    constexpr size_t size() const noexcept
    {
        return entries_.size();
    }
    constexpr bool empty() const noexcept
    {
        return entries_.empty();
    }
    // 一覧に入れられるパスか (Add が使う規則)。
    static bool IsAcceptable(std::wstring_view path);

private:
    // 一覧に残すパスだけを、重複を落として先頭から順に詰め直す。
    void Normalize();

    // entries_[0] が最も新しい。
    std::vector<std::wstring> entries_;
};

// 一覧の 1 行に出す 2 段。name は末尾の要素、directory はその親。
struct RecentFileLabel {
    std::wstring name;
    std::wstring directory;
};
RecentFileLabel MakeRecentFileLabel(std::wstring_view path);
