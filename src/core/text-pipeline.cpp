#include "text-pipeline.h"

#include "summary-parser.h"
#include "text-util.h"

namespace rts {

TextPipeline::TextPipeline(TextGenerator &generator, Options options, EnglishFn on_english, SummaryFn on_summary,
			   ClockFn clock)
	: generator_(generator),
	  options_(options),
	  on_english_(std::move(on_english)),
	  on_summary_(std::move(on_summary)),
	  clock_(clock),
	  summary_state_(options.summary_window_seconds, clock)
{
}

TextPipeline::~TextPipeline()
{
	stop();
}

void TextPipeline::start()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (running_)
		return;
	running_ = true;
	for (int i = 0; i < options_.translate_workers; ++i)
		workers_.emplace_back(&TextPipeline::translate_loop, this);
	workers_.emplace_back(&TextPipeline::summary_loop, this);
}

void TextPipeline::stop()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!running_)
			return;
		running_ = false;
		outstanding_ -= static_cast<int>(translate_queue_.size());
		translate_queue_.clear();
		if (summary_request_) {
			summary_request_.reset();
			summary_state_.fail_update();
			summary_running_ = false;
			--outstanding_;
		}
	}
	generator_.cancel();
	work_cv_.notify_all();
	for (auto &worker : workers_) {
		if (worker.joinable())
			worker.join();
	}
	workers_.clear();
	idle_cv_.notify_all();
}

void TextPipeline::submit_turn(const CompletedTurn &turn)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!running_)
		return;
	const bool included = summary_state_.add_transcript(turn.transcript, turn.completed_at);
	translate_queue_.push_back(turn);
	++outstanding_;
	if (included)
		maybe_start_summary_locked();
	work_cv_.notify_all();
}

void TextPipeline::wait_idle()
{
	std::unique_lock<std::mutex> lock(mutex_);
	idle_cv_.wait(lock, [this] { return outstanding_ == 0 || !running_; });
}

TextPipeline::Counters TextPipeline::counters() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return counters_;
}

void TextPipeline::maybe_start_summary_locked()
{
	if (summary_running_)
		return;
	if (last_summary_completed_at_ && clock_() - *last_summary_completed_at_ < options_.summary_interval_seconds)
		return;
	auto request = summary_state_.begin_update();
	if (!request)
		return;
	summary_request_ = std::move(*request);
	summary_running_ = true;
	++outstanding_;
}

void TextPipeline::finish_task_locked()
{
	--outstanding_;
	if (outstanding_ == 0)
		idle_cv_.notify_all();
}

void TextPipeline::translate_loop()
{
	for (;;) {
		CompletedTurn turn;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			work_cv_.wait(lock, [this] { return !running_ || !translate_queue_.empty(); });
			if (!running_)
				return;
			turn = std::move(translate_queue_.front());
			translate_queue_.pop_front();
		}

		bool succeeded = false;
		bool stale = false;
		bool threw = false;
		try {
			const GenerationResult result = generator_.translate(turn.transcript);
			const std::string text = result.ok ? trim_unicode(result.text) : std::string();
			if (!text.empty()) {
				succeeded = true;
				std::lock_guard<std::mutex> english_lock(english_mutex_);
				if (turn.sequence > latest_english_sequence_) {
					latest_english_sequence_ = turn.sequence;
					on_english_(text);
				} else {
					stale = true;
				}
			}
		} catch (...) {
			threw = true;
		}

		std::lock_guard<std::mutex> lock(mutex_);
		if (threw)
			++counters_.worker_exceptions;
		if (succeeded)
			++counters_.translations_succeeded;
		else
			++counters_.translations_failed;
		if (stale)
			++counters_.translations_discarded_as_stale;
		finish_task_locked();
	}
}

void TextPipeline::summary_loop()
{
	for (;;) {
		std::vector<std::string> request;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			work_cv_.wait(lock, [this] { return !running_ || summary_request_.has_value(); });
			if (!running_)
				return;
			request = std::move(*summary_request_);
			summary_request_.reset();
		}

		std::optional<SummaryParseResult> parsed;
		bool generated = false;
		bool threw = false;
		try {
			const GenerationResult result = generator_.summarize(request);
			generated = result.ok;
			if (result.ok)
				parsed = parse_summary_response(result.text, options_.summary_max_chars);
			if (parsed)
				on_summary_(parsed->display_lines);
		} catch (...) {
			threw = true;
			parsed.reset();
		}

		std::lock_guard<std::mutex> lock(mutex_);
		if (threw)
			++counters_.worker_exceptions;
		if (parsed) {
			summary_state_.succeed_update(parsed->display_lines);
			last_summary_completed_at_ = clock_();
			++counters_.summaries_succeeded;
		} else {
			summary_state_.fail_update();
			if (generated)
				++counters_.summaries_invalid;
			else
				++counters_.summaries_failed;
		}
		summary_running_ = false;
		finish_task_locked();
	}
}

} // namespace rts
