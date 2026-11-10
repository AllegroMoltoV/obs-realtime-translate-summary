#pragma once

#include <string>
#include <vector>

namespace rts {

struct KeywordParseResult {
	std::vector<std::string> keywords;
	// 1 から数えた不正な行の番号。空白だけ、<、> を含む行
	std::vector<int> invalid_lines;
	bool ok() const { return invalid_lines.empty(); }
};

// 1 行 1 語句の固有名詞のヒントを検証する。空行は無視し、前後の空白を除く。元のツールの config.py の検証に合わせる
KeywordParseResult parse_keywords(const std::string &text);

} // namespace rts
