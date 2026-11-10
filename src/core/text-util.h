#pragma once

#include <cstddef>
#include <string>

namespace rts {

// UTF-8 の文字列を Unicode のコードポイント単位で扱う。元のツールの Python の len と str.strip() に合わせる

// コードポイント数を返す
size_t codepoint_length(const std::string &text);

// 前後の空白を除く。対象は Python の str.isspace() が真になる文字で、全角空白 U+3000 を含む
std::string trim_unicode(const std::string &text);

// max_chars を超える場合、先頭 max_chars - 1 文字に三点リーダー … を付ける
std::string truncate_with_ellipsis(const std::string &text, size_t max_chars);

bool contains_line_break(const std::string &text);

} // namespace rts
