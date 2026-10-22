#pragma once
#include <dwrite.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

// 中国語と英語が混在する行で、ASCII (英数字・記号・空白) の連続区間だけ
// Latin フォントへ差し替える。中国語フォントの Latin 字形より Segoe UI の方が
// 英文の字幅と字面が揃うため、区間の単位でフォントを分ける。
// 区間は IDWriteTextLayout と同じ UTF-16 code unit 単位。
inline constexpr size_t kMaxLayoutTextLength = std::numeric_limits<uint32_t>::max();

template <class Emit>
void ForEachLatinRun(std::wstring_view text, Emit&& emit)
{
    const uint32_t size = static_cast<uint32_t>(std::min<size_t>(text.size(), kMaxLayoutTextLength));
    uint32_t run_start = 0;
    bool in_run = false;
    for (uint32_t i = 0; i < size; ++i) {
        const wchar_t c = text[i];
        const bool latin = c >= 0x20 && c <= 0x7E;
        if (latin && !in_run) {
            run_start = i;
            in_run = true;
        }
        else if (!latin && in_run) {
            emit(run_start, i - run_start);
            in_run = false;
        }
    }
    if (in_run) {
        emit(run_start, size - run_start);
    }
}

// 呼び出し側が既に中国語を主フォントにしているレイアウトへ、ASCII 区間だけ
// Latin フォントを適用する。コードブロックなど等幅を保ちたいレイアウトには使わない。
inline void ApplyLatinFontRuns(IDWriteTextLayout* layout, std::wstring_view text, const wchar_t* latin_family)
{
    if (!layout || !latin_family || !*latin_family) {
        return;
    }
    ForEachLatinRun(text, [layout, latin_family](uint32_t start, uint32_t length) {
        layout->SetFontFamilyName(latin_family, DWRITE_TEXT_RANGE{ start, length });
    });
}
