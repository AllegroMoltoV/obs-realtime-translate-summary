#pragma once

#include "summary-state.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>

namespace rts {

constexpr int kMinSummaryMaxChars = 4;

struct SummaryParseResult {
	DisplayLines display_lines;
	// 切り詰める前の各行の文字数
	std::array<size_t, 3> model_line_chars{};
};

// 概要の応答を検証し、N 文字を超える行を切り詰める。不正な応答は nullopt。元のツールの parse_summary_response を移した
std::optional<SummaryParseResult> parse_summary_response(const std::string &text, int max_chars);

} // namespace rts
