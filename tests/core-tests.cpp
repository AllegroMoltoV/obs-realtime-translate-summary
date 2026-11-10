// 字幕・英訳・概要の規則の単体テスト。外部 API を使わず、時計と API の呼び出しを差し替えて決定的に動かす

#include "core/caption-router.h"
#include "core/keywords.h"
#include "core/prompts.h"
#include "core/start-check.h"
#include "core/summary-parser.h"
#include "core/summary-state.h"
#include "core/text-pipeline.h"
#include "core/text-util.h"
#include "core/turn-tracker.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace rts;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                    \
	do {                                                                           \
		++g_checks;                                                            \
		if (!(cond)) {                                                         \
			++g_failures;                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
		}                                                                      \
	} while (0)

struct FakeClock {
	double now = 1000.0;
	ClockFn fn()
	{
		return [this] {
			return now;
		};
	}
};

const std::string kFullWidthSpace = "\xE3\x80\x80"; // U+3000

// ---- text-util ----

void test_text_util()
{
	CHECK(codepoint_length("日本語abc") == 6);
	CHECK(trim_unicode(kFullWidthSpace + " 字幕 \t" + kFullWidthSpace) == "字幕");
	CHECK(trim_unicode(kFullWidthSpace + kFullWidthSpace).empty());
	CHECK(truncate_with_ellipsis("日本語文字列", 6) == "日本語文字列");
	CHECK(truncate_with_ellipsis("日本語文字列追加", 6) == "日本語文字\xE2\x80\xA6");
}

// ---- turn-tracker ----

void test_turns_released_in_commit_order()
{
	FakeClock clock;
	TurnTracker tracker(clock.fn());
	CHECK(tracker.commit("a", std::nullopt) == TurnError::None);
	CHECK(tracker.commit("b", std::string("a")) == TurnError::None);
	CHECK(tracker.commit("c", std::string("b")) == TurnError::None);

	std::vector<CompletedTurn> released;
	CHECK(tracker.complete("c", "三", released) == TurnError::None);
	CHECK(released.empty());
	CHECK(tracker.complete("b", "二", released) == TurnError::None);
	CHECK(released.empty());
	CHECK(tracker.complete("a", "一", released) == TurnError::None);
	CHECK(released.size() == 3);
	if (released.size() == 3) {
		CHECK(released[0].transcript == "一");
		CHECK(released[1].transcript == "二");
		CHECK(released[2].transcript == "三");
		CHECK(released[0].completed_at == clock.now);
	}
}

void test_failed_turn_does_not_block_following()
{
	TurnTracker tracker;
	tracker.commit("a", std::nullopt);
	tracker.commit("b", std::string("a"));
	std::vector<CompletedTurn> released;
	tracker.complete("b", "二", released);
	CHECK(released.empty());
	CHECK(tracker.fail("a", released) == TurnError::None);
	CHECK(released.size() == 1 && released[0].transcript == "二");
	CHECK(tracker.pending_count() == 0);
}

void test_commit_duplicates_and_unknowns()
{
	TurnTracker tracker;
	int first = -1;
	int again = -1;
	tracker.commit("a", std::nullopt, &first);
	// 同じ item_id の再 commit は既存の順番を返す
	CHECK(tracker.commit("a", std::nullopt, &again) == TurnError::None);
	CHECK(first == again);
	CHECK(tracker.commit("b", std::string("missing")) == TurnError::UnknownPrevious);
	CHECK(tracker.commit("b", std::string("a")) == TurnError::None);
	// 同じ previous_item_id を持つ別の item は同じ順番になるため異常
	CHECK(tracker.commit("c", std::string("a")) == TurnError::DuplicateSequence);
	std::vector<CompletedTurn> released;
	CHECK(tracker.complete("unknown", "x", released) == TurnError::UnknownItem);
}

// ---- caption-router ----

TranscriptionEvent make_event(TranscriptionEvent::Type type, const std::string &item,
			      std::optional<std::string> previous = std::nullopt, const std::string &text = "")
{
	TranscriptionEvent event;
	event.type = type;
	event.item_id = item;
	event.previous_item_id = std::move(previous);
	if (type == TranscriptionEvent::Type::Delta)
		event.delta = text;
	else
		event.transcript = text;
	return event;
}

void test_router_accumulates_deltas_and_ignores_older()
{
	std::vector<std::string> captions;
	std::vector<std::string> completed;
	CaptionRouter router([&](const std::string &c) { captions.push_back(c); },
			     [&](const CompletedTurn &t) { completed.push_back(t.transcript); });
	using T = TranscriptionEvent::Type;
	router.handle(make_event(T::Committed, "a"));
	router.handle(make_event(T::Committed, "b", std::string("a")));
	router.handle(make_event(T::Delta, "a", std::nullopt, "こん"));
	router.handle(make_event(T::Delta, "a", std::nullopt, "にちは"));
	CHECK(!captions.empty() && captions.back() == "こんにちは");
	router.handle(make_event(T::Delta, "b", std::nullopt, "次"));
	CHECK(captions.back() == "次");
	const size_t before = captions.size();
	// 表示中より古い発話の差分は捨てる
	router.handle(make_event(T::Delta, "a", std::nullopt, "!"));
	CHECK(captions.size() == before);

	router.handle(make_event(T::Completed, "b", std::nullopt, "次の話"));
	CHECK(completed.empty());
	router.handle(make_event(T::Completed, "a", std::nullopt, " こんにちは。" + kFullWidthSpace));
	CHECK(completed.size() == 2);
	if (completed.size() == 2) {
		CHECK(completed[0] == "こんにちは。");
		CHECK(completed[1] == "次の話");
	}
}

void test_router_skips_empty_completed_and_resets()
{
	int completed = 0;
	CaptionRouter router([](const std::string &) {}, [&](const CompletedTurn &) { ++completed; });
	using T = TranscriptionEvent::Type;
	router.handle(make_event(T::Committed, "a"));
	router.handle(make_event(T::Completed, "a", std::nullopt, kFullWidthSpace));
	CHECK(completed == 0);
	router.handle(make_event(T::Committed, "b", std::string("a")));
	router.handle(make_event(T::Delta, "b", std::nullopt, "途中"));
	const auto counts = router.reset_session();
	CHECK(counts.pending_items == 1);
	CHECK(counts.partial_captions == 1);
	// リセット後は前のセッションの item を知らない
	CHECK(router.handle(make_event(T::Committed, "c", std::string("b"))) == TurnError::UnknownPrevious);
}

void test_parse_transcription_event()
{
	const auto event = parse_transcription_event(
		R"({"type":"input_audio_buffer.committed","item_id":"x","previous_item_id":null})");
	CHECK(event && event->type == TranscriptionEvent::Type::Committed && !event->previous_item_id);
	CHECK(!parse_transcription_event("not json"));
	const auto error = parse_transcription_event(R"({"type":"error","error":{"type":"invalid_request_error"}})");
	CHECK(error && error->type == TranscriptionEvent::Type::Other && error->error_type == "invalid_request_error");
}

// ---- summary-state ----

void test_summary_window_uses_completion_time()
{
	FakeClock clock;
	SummaryState state(300.0, clock.fn());
	CHECK(!state.add_transcript("", clock.now));
	CHECK(!state.add_transcript("古い", clock.now - 301.0));
	CHECK(state.add_transcript("一", clock.now - 200.0));
	CHECK(state.add_transcript("二", clock.now));
	clock.now += 150.0; // 「一」は窓の外へ出る
	auto request = state.begin_update();
	CHECK(request && request->size() == 1 && (*request)[0] == "二");
	CHECK(!state.begin_update()); // 更新中は二重に始めない
	state.fail_update();
	auto retry = state.begin_update(); // 失敗後は入力を保ったまま再び始められる
	CHECK(retry && retry->size() == 1);
}

// ---- summary-parser ----

void test_summary_parser()
{
	auto ok = parse_summary_response(R"({"display_lines":["目的を説明中","字幕方式を比較","OBSへ表示"]})", 30);
	CHECK(ok && ok->display_lines[2] == "OBSへ表示");
	CHECK(!parse_summary_response(R"({"display_lines":["一行だけ"]})", 30));
	CHECK(!parse_summary_response(R"({"display_lines":["a","b",1]})", 30));
	CHECK(!parse_summary_response("not json", 30));

	auto trailing = parse_summary_response(R"({"display_lines":["今日はピアノを弾く","",""]})", 30);
	CHECK(trailing && trailing->display_lines[1].empty());
	CHECK(!parse_summary_response(R"({"display_lines":["","",""]})", 30));
	CHECK(!parse_summary_response(R"({"display_lines":["話題","","補足"]})", 30));
	CHECK(!parse_summary_response("{\"display_lines\":[\"\xE3\x80\x80\",\"\",\"\"]}", 30));
	CHECK(!parse_summary_response(R"({"display_lines":["一\n二","",""]})", 30));

	auto cut = parse_summary_response(R"({"display_lines":["日本語文字列追加","日本語文字列",""]})", 6);
	CHECK(cut && cut->display_lines[0] == "日本語文字\xE2\x80\xA6");
	CHECK(cut && cut->display_lines[1] == "日本語文字列");
	CHECK(cut && cut->model_line_chars[0] == 8);
	CHECK(!parse_summary_response(R"({"display_lines":["a","",""]})", 3));
}

// ---- keywords と prompts ----

void test_keywords()
{
	const auto ok = parse_keywords(" AllegroMoltoV \n\n倉庫くん" + kFullWidthSpace + "\r\n");
	CHECK(ok.ok());
	CHECK(ok.keywords.size() == 2 && ok.keywords[0] == "AllegroMoltoV" && ok.keywords[1] == "倉庫くん");

	const auto bad = parse_keywords("良い\n" + kFullWidthSpace + "\n<タグ>\n大なり>\n   ");
	CHECK(bad.invalid_lines == std::vector<int>({2, 3, 4, 5}));
	CHECK(bad.keywords.size() == 1);

	CHECK(session_update({})["session"]["audio"]["input"]["transcription"].count("keywords") == 0);
	CHECK(session_update({"a"})["session"]["audio"]["input"]["transcription"]["keywords"].size() == 1);
	CHECK(summary_instructions(30).find("各行の最大文字数は28文字") != std::string::npos);
	CHECK(summary_instructions(30).find("情報が十分な行は24文字前後") != std::string::npos);
	CHECK(summary_instructions(4).find("情報が十分な行は2文字前後") != std::string::npos);
}

// ---- text-pipeline ----

// 呼び出しを記録し、テストが release するまで完了を止められる偽の API
class FakeGenerator : public TextGenerator {
public:
	GenerationResult translate(const std::string &transcript) override
	{
		std::unique_lock<std::mutex> lock(mutex_);
		translate_calls.push_back(transcript);
		cv_.notify_all();
		cv_.wait(lock, [&] { return cancelled_ || !blocked_.count(transcript); });
		return {!cancelled_, "EN:" + transcript};
	}

	GenerationResult summarize(const std::vector<std::string> &transcripts) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		summary_inputs.push_back(transcripts);
		return {true, summary_response};
	}

	void cancel() override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		cancelled_ = true;
		cv_.notify_all();
	}

	void block(const std::string &transcript)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		blocked_[transcript] = true;
	}

	void release(const std::string &transcript)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		blocked_.erase(transcript);
		cv_.notify_all();
	}

	void wait_for_translate_calls(size_t count)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		cv_.wait(lock, [&] { return translate_calls.size() >= count; });
	}

	std::vector<std::string> translate_calls;
	std::vector<std::vector<std::string>> summary_inputs;
	std::string summary_response = R"({"display_lines":["要約","",""]})";

private:
	std::mutex mutex_;
	std::condition_variable cv_;
	std::map<std::string, bool> blocked_;
	bool cancelled_ = false;
};

CompletedTurn turn(int sequence, const std::string &text, double at)
{
	return CompletedTurn{sequence, "item" + std::to_string(sequence), text, at};
}

void test_older_translation_cannot_overwrite_newer()
{
	FakeClock clock;
	FakeGenerator generator;
	std::vector<std::string> english;
	std::mutex english_mutex;
	TextPipeline pipeline(
		generator, {},
		[&](const std::string &text) {
			std::lock_guard<std::mutex> lock(english_mutex);
			english.push_back(text);
		},
		[](const DisplayLines &) {}, clock.fn());
	pipeline.start();

	generator.block("古い");
	pipeline.submit_turn(turn(0, "古い", clock.now));
	generator.wait_for_translate_calls(1);
	pipeline.submit_turn(turn(1, "新しい", clock.now));
	generator.wait_for_translate_calls(2);
	// 新しい英訳が先に完了するまで待ってから古い英訳を完了させる
	for (;;) {
		std::lock_guard<std::mutex> lock(english_mutex);
		if (!english.empty())
			break;
	}
	generator.release("古い");
	pipeline.wait_idle();

	CHECK(english.size() == 1 && english[0] == "EN:新しい");
	CHECK(pipeline.counters().translations_discarded_as_stale == 1);
	pipeline.stop();
}

void test_summary_respects_minimum_interval()
{
	FakeClock clock;
	FakeGenerator generator;
	std::vector<DisplayLines> summaries;
	TextPipeline::Options options;
	options.summary_interval_seconds = 10.0;
	TextPipeline pipeline(
		generator, options, [](const std::string &) {},
		[&](const DisplayLines &lines) { summaries.push_back(lines); }, clock.fn());
	pipeline.start();

	pipeline.submit_turn(turn(0, "一", clock.now));
	pipeline.wait_idle();
	CHECK(generator.summary_inputs.size() == 1);

	clock.now += 5.0; // 最短間隔の内側
	pipeline.submit_turn(turn(1, "二", clock.now));
	pipeline.wait_idle();
	CHECK(generator.summary_inputs.size() == 1);

	clock.now += 6.0; // 前回の完了から 11 秒
	pipeline.submit_turn(turn(2, "三", clock.now));
	pipeline.wait_idle();
	CHECK(generator.summary_inputs.size() == 2);
	if (generator.summary_inputs.size() == 2)
		CHECK(generator.summary_inputs[1].size() == 3);
	CHECK(summaries.size() == 2);
	pipeline.stop();
}

void test_invalid_summary_keeps_last_and_allows_retry()
{
	FakeClock clock;
	FakeGenerator generator;
	generator.summary_response = R"({"display_lines":["","",""]})";
	int summaries = 0;
	TextPipeline pipeline(
		generator, {}, [](const std::string &) {}, [&](const DisplayLines &) { ++summaries; }, clock.fn());
	pipeline.start();
	pipeline.submit_turn(turn(0, "一", clock.now));
	pipeline.wait_idle();
	CHECK(summaries == 0);
	CHECK(pipeline.counters().summaries_invalid == 1);

	// 失敗は最短間隔の起点にならず、次の確定ですぐ再び始まる
	generator.summary_response = R"({"display_lines":["要約","",""]})";
	pipeline.submit_turn(turn(1, "二", clock.now));
	pipeline.wait_idle();
	CHECK(summaries == 1);
	pipeline.stop();
}

void test_stop_cancels_inflight_translation()
{
	FakeGenerator generator;
	TextPipeline pipeline(generator, {}, [](const std::string &) {}, [](const DisplayLines &) {});
	pipeline.start();
	generator.block("止める");
	pipeline.submit_turn(turn(0, "止める", 0.0));
	generator.wait_for_translate_calls(1);
	pipeline.stop(); // cancel で偽の API が戻り、合流できる
	CHECK(true);
}

// ---- start-check ----

bool has_problem(const std::vector<StartProblem> &problems, StartProblem p)
{
	return std::find(problems.begin(), problems.end(), p) != problems.end();
}

void test_start_check()
{
	StartInputs ok;
	ok.api_key_saved = true;
	ok.audio_source = "mic";
	ok.audio_source_exists = true;
	ok.targets[0] = {"ja", true, true};
	CHECK(check_start(ok).empty());

	StartInputs none;
	const auto p1 = check_start(none);
	CHECK(has_problem(p1, StartProblem::ApiKeyMissing));
	CHECK(has_problem(p1, StartProblem::AudioSourceNotSelected));
	CHECK(has_problem(p1, StartProblem::NoTargetSelected));

	StartInputs bad = ok;
	bad.audio_source_exists = false;
	bad.targets[1] = {"ja", true, true};   // 重複
	bad.targets[2] = {"img", true, false}; // テキストソースでない
	const auto p2 = check_start(bad);
	CHECK(has_problem(p2, StartProblem::AudioSourceMissing));
	CHECK(has_problem(p2, StartProblem::DuplicateTarget));
	CHECK(has_problem(p2, StartProblem::TargetNotText));
	CHECK(!has_problem(p2, StartProblem::NoTargetSelected));

	StartInputs gone = ok;
	gone.targets[0].exists = false;
	CHECK(has_problem(check_start(gone), StartProblem::TargetMissing));

	CHECK(is_supported_text_source_id("text_gdiplus"));
	CHECK(is_supported_text_source_id("text_ft2_source"));
	CHECK(!is_supported_text_source_id("text_gdiplus_v3"));
	CHECK(!valid_summary_max_chars(3));
	CHECK(valid_summary_max_chars(4));
	CHECK(valid_summary_max_chars(30));
	CHECK(!valid_summary_interval(0.0));
	CHECK(valid_summary_interval(0.5));
}

} // namespace

int main()
{
	const std::vector<std::pair<const char *, std::function<void()>>> tests = {
		{"text_util", test_text_util},
		{"turns_released_in_commit_order", test_turns_released_in_commit_order},
		{"failed_turn_does_not_block_following", test_failed_turn_does_not_block_following},
		{"commit_duplicates_and_unknowns", test_commit_duplicates_and_unknowns},
		{"router_accumulates_deltas_and_ignores_older", test_router_accumulates_deltas_and_ignores_older},
		{"router_skips_empty_completed_and_resets", test_router_skips_empty_completed_and_resets},
		{"parse_transcription_event", test_parse_transcription_event},
		{"summary_window_uses_completion_time", test_summary_window_uses_completion_time},
		{"summary_parser", test_summary_parser},
		{"keywords", test_keywords},
		{"older_translation_cannot_overwrite_newer", test_older_translation_cannot_overwrite_newer},
		{"summary_respects_minimum_interval", test_summary_respects_minimum_interval},
		{"invalid_summary_keeps_last_and_allows_retry", test_invalid_summary_keeps_last_and_allows_retry},
		{"stop_cancels_inflight_translation", test_stop_cancels_inflight_translation},
		{"start_check", test_start_check},
	};
	for (const auto &test : tests) {
		const int before = g_failures;
		test.second();
		std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", test.first);
	}
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
