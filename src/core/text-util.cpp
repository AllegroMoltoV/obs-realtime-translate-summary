#include "text-util.h"

#include <cstdint>
#include <vector>

namespace rts {

namespace {

struct Codepoint {
	uint32_t value;
	size_t offset;
	size_t length;
};

// 不正なバイト列は 1 バイトを 1 文字として扱い、文字数の計算を止めない
std::vector<Codepoint> decode(const std::string &text)
{
	std::vector<Codepoint> result;
	size_t i = 0;
	while (i < text.size()) {
		const auto c = static_cast<unsigned char>(text[i]);
		size_t length = 1;
		uint32_t value = c;
		if (c >= 0xC0 && c < 0xE0) {
			length = 2;
			value = c & 0x1F;
		} else if (c >= 0xE0 && c < 0xF0) {
			length = 3;
			value = c & 0x0F;
		} else if (c >= 0xF0 && c < 0xF8) {
			length = 4;
			value = c & 0x07;
		}

		if (length > 1 && i + length <= text.size()) {
			bool valid = true;
			for (size_t k = 1; k < length; ++k) {
				const auto next = static_cast<unsigned char>(text[i + k]);
				if ((next & 0xC0) != 0x80) {
					valid = false;
					break;
				}
				value = (value << 6) | (next & 0x3F);
			}
			if (!valid) {
				length = 1;
				value = c;
			}
		} else if (length > 1) {
			length = 1;
			value = c;
		}
		result.push_back({value, i, length});
		i += length;
	}
	return result;
}

// Python の str.isspace() が真になる文字
bool is_unicode_space(uint32_t c)
{
	if (c >= 0x09 && c <= 0x0D)
		return true;
	if (c >= 0x1C && c <= 0x20)
		return true;
	switch (c) {
	case 0x85:
	case 0xA0:
	case 0x1680:
	case 0x2028:
	case 0x2029:
	case 0x202F:
	case 0x205F:
	case 0x3000:
		return true;
	default:
		return c >= 0x2000 && c <= 0x200A;
	}
}

} // namespace

size_t codepoint_length(const std::string &text)
{
	return decode(text).size();
}

std::string trim_unicode(const std::string &text)
{
	const auto codepoints = decode(text);
	size_t first = 0;
	size_t last = codepoints.size();
	while (first < last && is_unicode_space(codepoints[first].value))
		++first;
	while (last > first && is_unicode_space(codepoints[last - 1].value))
		--last;
	if (first == last)
		return std::string();
	const size_t begin = codepoints[first].offset;
	const size_t end = codepoints[last - 1].offset + codepoints[last - 1].length;
	return text.substr(begin, end - begin);
}

std::string truncate_with_ellipsis(const std::string &text, size_t max_chars)
{
	static const std::string kEllipsis = "\xE2\x80\xA6"; // …
	const auto codepoints = decode(text);
	if (max_chars == 0 || codepoints.size() <= max_chars)
		return text;
	const size_t keep = max_chars - 1;
	const size_t end = keep == 0 ? 0 : codepoints[keep - 1].offset + codepoints[keep - 1].length;
	return text.substr(0, end) + kEllipsis;
}

bool contains_line_break(const std::string &text)
{
	return text.find('\n') != std::string::npos || text.find('\r') != std::string::npos;
}

} // namespace rts
