#pragma once

#include "summary-state.h"
#include "turn-tracker.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rts {

struct GenerationResult {
	bool ok = false;
	std::string text;
};

// 英訳と概要の API の呼び出し。テストでは偽の実装に差し替える。呼び出しは作業のスレッドから同期で行う
class TextGenerator {
public:
	virtual ~TextGenerator() = default;
	virtual GenerationResult translate(const std::string &transcript) = 0;
	virtual GenerationResult summarize(const std::vector<std::string> &transcripts) = 0;
	// 停止の要求で、実行中の呼び出しを中断させる
	virtual void cancel() {}
};

// 確定した発話から英訳と概要を作る。元のツールの TextProcessingPipeline を移した。
// 英訳は確定ごとに並行して呼び、古い順番の英訳で新しい英訳を上書きしない。
// 概要は 1 本だけ走らせ、前回の成功から最短間隔以上たってから確定が届くと始める
class TextPipeline {
public:
	using EnglishFn = std::function<void(const std::string &)>;
	using SummaryFn = std::function<void(const DisplayLines &)>;

	struct Options {
		double summary_interval_seconds = 10.0;
		double summary_window_seconds = 300.0;
		int summary_max_chars = 30;
		int translate_workers = 2;
	};

	struct Counters {
		int translations_succeeded = 0;
		int translations_failed = 0;
		int translations_discarded_as_stale = 0;
		int summaries_succeeded = 0;
		int summaries_failed = 0;
		int summaries_invalid = 0;
		int worker_exceptions = 0;
	};

	TextPipeline(TextGenerator &generator, Options options, EnglishFn on_english, SummaryFn on_summary,
		     ClockFn clock = steady_clock_seconds());
	~TextPipeline();

	TextPipeline(const TextPipeline &) = delete;
	TextPipeline &operator=(const TextPipeline &) = delete;

	void start();
	void stop();

	void submit_turn(const CompletedTurn &turn);

	// 受け付けた処理がすべて終わるまで待つ。テストと停止の前に使う
	void wait_idle();

	Counters counters() const;

private:
	void translate_loop();
	void summary_loop();
	void maybe_start_summary_locked();
	void finish_task_locked();

	TextGenerator &generator_;
	Options options_;
	EnglishFn on_english_;
	SummaryFn on_summary_;
	ClockFn clock_;
	SummaryState summary_state_;

	mutable std::mutex mutex_;
	std::condition_variable work_cv_;
	std::condition_variable idle_cv_;
	bool running_ = false;
	std::deque<CompletedTurn> translate_queue_;
	std::optional<std::vector<std::string>> summary_request_;
	bool summary_running_ = false;
	std::optional<double> last_summary_completed_at_;
	int latest_english_sequence_ = -1;
	int outstanding_ = 0;
	Counters counters_;

	std::mutex english_mutex_;
	std::vector<std::thread> workers_;
};

} // namespace rts
