#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rts {

// 単調に増える秒数を返す時計。テストで差し替える
using ClockFn = std::function<double()>;
ClockFn steady_clock_seconds();

struct CompletedTurn {
	int sequence = 0;
	std::string item_id;
	std::string transcript;
	double completed_at = 0.0;
};

enum class TurnError { None, UnknownPrevious, DuplicateSequence, UnknownItem };

// 確定した発話を commit の順に放出する。元のツールの state.py の TurnTracker を移した
class TurnTracker {
public:
	explicit TurnTracker(ClockFn clock = steady_clock_seconds());

	// 同じ item_id の再 commit は既存の順番を返す
	TurnError commit(const std::string &item_id, const std::optional<std::string> &previous_item_id,
			 int *sequence_out = nullptr);
	TurnError complete(const std::string &item_id, const std::string &transcript,
			   std::vector<CompletedTurn> &released);
	TurnError fail(const std::string &item_id, std::vector<CompletedTurn> &released);

	std::optional<int> sequence_for(const std::string &item_id) const;
	int pending_count() const;

private:
	void drain(std::vector<CompletedTurn> &released);

	ClockFn clock_;
	std::map<std::string, int> sequence_by_item_;
	std::map<int, std::optional<CompletedTurn>> resolved_;
	int next_release_ = 0;
};

} // namespace rts
