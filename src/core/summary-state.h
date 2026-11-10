#pragma once

#include "turn-tracker.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rts {

using DisplayLines = std::array<std::string, 3>;

// 概要の入力にする直近の確定発話を、確定時刻で選んで保持する。元のツールの SummaryState を移した
class SummaryState {
public:
	SummaryState(double window_seconds, ClockFn clock = steady_clock_seconds());

	// 空の発話と窓より古い発話は入れず false を返す
	bool add_transcript(const std::string &transcript, double completed_at);

	// 更新中か窓が空なら nullopt。始めた場合は入力の発話を古い順に返す
	std::optional<std::vector<std::string>> begin_update();
	void succeed_update(const DisplayLines &lines);
	void fail_update();

	bool in_flight() const { return in_flight_; }
	const std::optional<DisplayLines> &display_lines() const { return display_lines_; }

private:
	void prune(double now);

	double window_seconds_;
	ClockFn clock_;
	std::vector<std::pair<double, std::string>> transcripts_;
	bool in_flight_ = false;
	std::optional<DisplayLines> display_lines_;
};

} // namespace rts
