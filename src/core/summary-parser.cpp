#include "summary-parser.h"

#include "text-util.h"

#include <nlohmann/json.hpp>

namespace rts {

std::optional<SummaryParseResult> parse_summary_response(const std::string &text, int max_chars)
{
	if (max_chars < kMinSummaryMaxChars)
		return std::nullopt;

	const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
	if (parsed.is_discarded() || !parsed.is_object())
		return std::nullopt;
	const auto raw = parsed.find("display_lines");
	if (raw == parsed.end() || !raw->is_array() || raw->size() != 3)
		return std::nullopt;

	DisplayLines lines;
	for (size_t i = 0; i < 3; ++i) {
		const auto &item = (*raw)[i];
		if (!item.is_string())
			return std::nullopt;
		lines[i] = trim_unicode(item.get<std::string>());
		if (contains_line_break(lines[i]))
			return std::nullopt;
	}

	// 1 行目が空、または空行の後に文字のある行が続く応答は不正
	if (lines[0].empty())
		return std::nullopt;
	for (size_t i = 0; i < 3; ++i) {
		if (!lines[i].empty())
			continue;
		for (size_t k = i + 1; k < 3; ++k) {
			if (!lines[k].empty())
				return std::nullopt;
		}
	}

	SummaryParseResult result;
	for (size_t i = 0; i < 3; ++i) {
		result.model_line_chars[i] = codepoint_length(lines[i]);
		result.display_lines[i] = truncate_with_ellipsis(lines[i], static_cast<size_t>(max_chars));
	}
	return result;
}

} // namespace rts
