#include "keywords.h"

#include "text-util.h"

namespace rts {

KeywordParseResult parse_keywords(const std::string &text)
{
	KeywordParseResult result;
	int line_number = 0;
	size_t start = 0;
	while (start <= text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(start, end - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		++line_number;

		// 何も書かれていない行は入力の区切りとして無視する。空白だけの行は元のツールと同じく不正とする
		if (!line.empty()) {
			const std::string keyword = trim_unicode(line);
			if (keyword.empty() || keyword.find_first_of("<>") != std::string::npos)
				result.invalid_lines.push_back(line_number);
			else
				result.keywords.push_back(keyword);
		}

		if (end == text.size())
			break;
		start = end + 1;
	}
	return result;
}

} // namespace rts
