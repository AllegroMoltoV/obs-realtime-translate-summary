#include "turn-tracker.h"

#include <algorithm>
#include <chrono>

namespace rts {

ClockFn steady_clock_seconds()
{
	return [] {
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	};
}

TurnTracker::TurnTracker(ClockFn clock, int first_sequence)
	: clock_(std::move(clock)),
	  first_sequence_(first_sequence),
	  next_release_(first_sequence)
{
}

int TurnTracker::next_sequence() const
{
	int next = first_sequence_;
	for (const auto &entry : sequence_by_item_)
		next = (std::max)(next, entry.second + 1);
	return next;
}

TurnError TurnTracker::commit(const std::string &item_id, const std::optional<std::string> &previous_item_id,
			      int *sequence_out)
{
	const auto existing = sequence_by_item_.find(item_id);
	if (existing != sequence_by_item_.end()) {
		if (sequence_out)
			*sequence_out = existing->second;
		return TurnError::None;
	}

	int sequence = first_sequence_;
	if (previous_item_id) {
		const auto previous = sequence_by_item_.find(*previous_item_id);
		if (previous == sequence_by_item_.end())
			return TurnError::UnknownPrevious;
		sequence = previous->second + 1;
	}

	for (const auto &entry : sequence_by_item_) {
		if (entry.second == sequence)
			return TurnError::DuplicateSequence;
	}

	sequence_by_item_[item_id] = sequence;
	if (sequence_out)
		*sequence_out = sequence;
	return TurnError::None;
}

TurnError TurnTracker::complete(const std::string &item_id, const std::string &transcript,
				std::vector<CompletedTurn> &released)
{
	const auto sequence = sequence_for(item_id);
	if (!sequence)
		return TurnError::UnknownItem;
	resolved_[*sequence] = CompletedTurn{*sequence, item_id, transcript, clock_()};
	drain(released);
	return TurnError::None;
}

TurnError TurnTracker::fail(const std::string &item_id, std::vector<CompletedTurn> &released)
{
	const auto sequence = sequence_for(item_id);
	if (!sequence)
		return TurnError::UnknownItem;
	resolved_[*sequence] = std::nullopt;
	drain(released);
	return TurnError::None;
}

std::optional<int> TurnTracker::sequence_for(const std::string &item_id) const
{
	const auto found = sequence_by_item_.find(item_id);
	if (found == sequence_by_item_.end())
		return std::nullopt;
	return found->second;
}

int TurnTracker::pending_count() const
{
	int count = 0;
	for (const auto &entry : sequence_by_item_) {
		if (entry.second >= next_release_)
			++count;
	}
	return count;
}

void TurnTracker::drain(std::vector<CompletedTurn> &released)
{
	for (;;) {
		const auto found = resolved_.find(next_release_);
		if (found == resolved_.end())
			return;
		if (found->second)
			released.push_back(*found->second);
		resolved_.erase(found);
		++next_release_;
	}
}

} // namespace rts
