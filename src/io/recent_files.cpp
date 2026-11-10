#include "recent_files.h"
#include "document.h"
#include "file_io.h"
#include <algorithm>
#include <filesystem>

bool RecentFiles::IsAcceptable(std::wstring_view path)
{
    if (path.empty() || IsEmbeddedPath(path)) {
        return false;
    }
    // UNC やデバイスパスは、一覧を作っただけで資格情報の要求や接続待ちが走る。相対パスは cwd 次第で別の場所を指す。
    if (!path_util::IsLocalPath(path) || !std::filesystem::path(path).is_absolute()) {
        return false;
    }
    // 拡張子を見ない: ヘルプのリンクや起動引数から .txt なども開けるので、開いたものはそのまま残す。
    return true;
}

void RecentFiles::Normalize()
{
    std::vector<std::wstring> kept;
    kept.reserve(std::min(entries_.size(), kMaxEntries));
    for (auto& path : entries_) {
        if (kept.size() >= kMaxEntries) {
            break;
        }
        if (!IsAcceptable(path)) {
            continue;
        }
        const bool duplicate = std::ranges::any_of(kept, [&](const std::wstring& other) { return path_util::iequal(path, other); });
        if (!duplicate) {
            kept.push_back(std::move(path));
        }
    }
    entries_ = std::move(kept);
}

void RecentFiles::Add(std::wstring_view path)
{
    if (!IsAcceptable(path)) {
        return;
    }
    // 既にあるものを先頭へ移す。大文字小文字だけ違うパスを別項目にしない (NTFS は同じファイルを指す)。
    // 綴りは今開いたときのものへ揃える。
    const auto it = std::ranges::find_if(entries_, [&](const std::wstring& e) { return path_util::iequal(e, path); });
    if (it != entries_.end()) {
        entries_.erase(it);
    }
    entries_.insert(entries_.begin(), std::wstring{ path });
    Normalize();
}

bool RecentFiles::RemoveAt(size_t index)
{
    if (index >= entries_.size()) {
        return false;
    }
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void RecentFiles::Clear()
{
    entries_.clear();
}

RecentFileLabel MakeRecentFileLabel(std::wstring_view path)
{
    return {
        .name = std::wstring{ path_util::FilenameView(path) },
        .directory = path_util::ParentDirectory(path),
    };
}
