#include "summary-state.h"

#include <algorithm>

namespace rts {

SummaryState::SummaryState(double window_seconds, ClockFn clock)
	: window_seconds_(window_seconds),
	  clock_(std::move(clock))
{
}

bool SummaryState::add_transcript(const std::string &transcript, double completed_at)
{
	const double now = clock_();
	prune(now);
	if (transcript.empty() || completed_at < now - window_seconds_)
		return false;
	transcripts_.emplace_back(completed_at, transcript);
	return true;
}

std::optional<std::vector<std::string>> SummaryState::begin_update()
{
	if (in_flight_)
		return std::nullopt;
	prune(clock_());
	if (transcripts_.empty())
		return std::nullopt;

	in_flight_ = true;
	std::vector<std::string> request;
	request.reserve(transcripts_.size());
	for (const auto &entry : transcripts_)
		request.push_back(entry.second);
	return request;
}

void SummaryState::succeed_update(const DisplayLines &lines)
{
	display_lines_ = lines;
	in_flight_ = false;
}

void SummaryState::fail_update()
{
	in_flight_ = false;
}

void SummaryState::prune(double now)
{
	const double cutoff = now - window_seconds_;
	transcripts_.erase(std::remove_if(transcripts_.begin(), transcripts_.end(),
					  [cutoff](const auto &entry) { return entry.first < cutoff; }),
			   transcripts_.end());
}

} // namespace rts
