#include "realtime-session.h"

#include "base64.h"
#include "core/prompts.h"
#include "core/text-util.h"
#include "openai-text-generator.h"
#include "pcm-converter.h"
#include "realtime-client.h"

#include <obs.h>
#include <plugin-support.h>

#include <nlohmann/json.hpp>

#include <exception>

using json = nlohmann::json;

namespace rts {

namespace {

constexpr const char *kRealtimeUrl = "wss://api.openai.com/v1/realtime?intent=transcription";
constexpr size_t kChunkSamples = PcmConverter::kOutputRate / 10; // 100 ms

const char *turn_error_name(TurnError error)
{
	switch (error) {
	case TurnError::None:
		return "none";
	case TurnError::UnknownPrevious:
		return "unknown_previous";
	case TurnError::DuplicateSequence:
		return "duplicate_sequence";
	case TurnError::UnknownItem:
		return "unknown_item";
	}
	return "unknown";
}

std::string join_lines(const DisplayLines &lines)
{
	std::string text;
	for (const auto &line : lines) {
		if (line.empty())
			continue;
		if (!text.empty())
			text += "\n";
		text += line;
	}
	return text;
}

} // namespace

RealtimeSession::RealtimeSession() = default;

RealtimeSession::~RealtimeSession()
{
	stop();
}

bool RealtimeSession::start(const SessionConfig &config, OutputFn on_output)
{
	if (running_.load())
		return false;
	stop();

	config_ = config;
	on_output_ = std::move(on_output);
	sent_chunks_ = 0;
	input_tokens_ = 0;
	output_tokens_ = 0;
	produced_samples_ = caption_updates_ = completed_turns_ = router_errors_ = 0;
	network_alive_ = false;
	session_updated_ = false;
	error_ = SessionError::None;
	started_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();

	generator_ = std::make_unique<OpenAiTextGenerator>(config_.api_key, default_translate_profile(),
							   default_summary_profile(), config_.summary_max_chars);
	text_failures_ = 0;
	generator_->on_usage = [this](long long in, long long out) {
		input_tokens_ += in;
		output_tokens_ += out;
	};
	generator_->on_failure = [this] {
		++text_failures_;
	};
	TextPipeline::Options options;
	options.summary_max_chars = config_.summary_max_chars;
	options.summary_interval_seconds = config_.summary_interval_seconds;
	pipeline_ = std::make_unique<TextPipeline>(
		*generator_, options, [this](const std::string &english) { on_output_(English, english); },
		[this](const DisplayLines &lines) { on_output_(Summary, join_lines(lines)); });
	router_ = std::make_unique<CaptionRouter>(
		[this](const std::string &caption) {
			++caption_updates_;
			on_output_(Japanese, caption);
		},
		[this](const CompletedTurn &turn) {
			++completed_turns_;
			pipeline_->submit_turn(turn);
		});
	pipeline_->start();

	state_ = SessionState::Connecting;
	running_ = true;
	network_thread_ = std::thread(&RealtimeSession::network_loop, this);
	return true;
}

void RealtimeSession::stop()
{
	running_ = false;
	const bool had_thread = network_thread_.joinable();
	if (had_thread)
		network_thread_.join();
	if (pipeline_) {
		const auto c = pipeline_->counters();
		pipeline_->stop();
		obs_log(LOG_INFO,
			"session pipeline translations_ok=%d failed=%d stale=%d summaries_ok=%d failed=%d invalid=%d "
			"exceptions=%d",
			c.translations_succeeded, c.translations_failed, c.translations_discarded_as_stale,
			c.summaries_succeeded, c.summaries_failed, c.summaries_invalid, c.worker_exceptions);
	}
	router_.reset();
	pipeline_.reset();
	generator_.reset();
	if (state_ != SessionState::Error)
		state_ = SessionState::Stopped;
	if (had_thread)
		obs_log(LOG_INFO, "session stopped");
}

double RealtimeSession::sent_audio_seconds() const
{
	return static_cast<double>(sent_chunks_.load()) * kChunkSamples / PcmConverter::kOutputRate;
}

double RealtimeSession::elapsed_seconds() const
{
	const int64_t started = started_ns_.load();
	if (started == 0)
		return 0.0;
	const int64_t now =
		std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
	return static_cast<double>(now - started) / 1e9;
}

void RealtimeSession::fail(SessionError error)
{
	error_ = error;
	state_ = SessionState::Error;
	running_ = false;
}

void RealtimeSession::network_loop()
{
	// 例外がスレッドの外へ出ると std::terminate で OBS ごと落ちるため、ここで捕捉する
	try {
		PcmConverter converter;
		const uint32_t input_rate = audio_output_get_sample_rate(obs_get_audio());
		if (!converter.open(input_rate)) {
			obs_log(LOG_ERROR, "session resampler open failed input_rate=%u", input_rate);
			fail(SessionError::Internal);
			return;
		}

		RealtimeClient client;
		std::string error;
		if (!client.connect(kRealtimeUrl, config_.api_key, error)) {
			obs_log(LOG_ERROR, "session realtime connect failed %s", error.c_str());
			const long status = client.last_http_status();
			fail(status == 401 || status == 403 ? SessionError::AuthFailed : SessionError::ConnectFailed);
			return;
		}
		if (!client.send_text(session_update(config_.keywords).dump(), error)) {
			obs_log(LOG_ERROR, "session session.update send failed %s", error.c_str());
			fail(SessionError::ConnectFailed);
			return;
		}
		network_alive_ = true;

		if (!capture_.attach(config_.audio_source)) {
			obs_log(LOG_ERROR, "session audio source not found or has no audio");
			client.close();
			fail(SessionError::AudioSourceMissing);
			return;
		}

		state_ = SessionState::Running;
		last_metrics_ = Clock::now();
		obs_log(LOG_INFO, "session started input_rate=%u keywords=%zu", input_rate, config_.keywords.size());

		std::vector<int16_t> pcm;
		MonoFrame frame;
		while (running_.load()) {
			bool worked = false;

			while (capture_.pop(frame)) {
				worked = true;
				const size_t before = pcm.size();
				converter.convert(frame.samples, frame.frames, pcm);
				produced_samples_ += pcm.size() - before;
			}

			while (pcm.size() >= kChunkSamples) {
				if (network_alive_) {
					const std::string audio =
						base64_encode(reinterpret_cast<const uint8_t *>(pcm.data()),
							      kChunkSamples * sizeof(int16_t));
					const json append = {{"type", "input_audio_buffer.append"}, {"audio", audio}};
					if (client.send_text(append.dump(), error)) {
						++sent_chunks_;
					} else {
						obs_log(LOG_ERROR, "session audio send failed %s", error.c_str());
						network_alive_ = false;
					}
				}
				pcm.erase(pcm.begin(), pcm.begin() + kChunkSamples);
			}

			while (network_alive_) {
				std::string message;
				const auto result = client.receive(message, error);
				if (result == RealtimeClient::RecvResult::Nothing)
					break;
				worked = true;
				if (result == RealtimeClient::RecvResult::Message) {
					handle_message(message);
					continue;
				}
				obs_log(LOG_ERROR, "session realtime %s close_code=%d elapsed_s=%.1f detail=%s",
					result == RealtimeClient::RecvResult::Closed ? "closed" : "error",
					client.close_code(), elapsed_seconds(), error.c_str());
				network_alive_ = false;
			}

			// 再接続は段階 5 で足す。それまでは切断を停止の原因として返す
			if (!network_alive_ && running_.load() && error_.load() == SessionError::None)
				fail(SessionError::ConnectionLost);

			if (Clock::now() - last_metrics_ >= std::chrono::seconds(60))
				log_metrics();

			if (!worked)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}

		capture_.detach();
		log_metrics();
		client.close();
	} catch (...) {
		capture_.detach();
		obs_log(LOG_ERROR, "session network thread exception");
		fail(SessionError::Internal);
	}
}

void RealtimeSession::handle_message(const std::string &message)
{
	const auto event = parse_transcription_event(message);
	if (!event) {
		obs_log(LOG_WARNING, "session event invalid json bytes=%zu", message.size());
		return;
	}

	if (event->type == TranscriptionEvent::Type::Other) {
		if (event->type_name == "session.updated")
			session_updated_ = true;
		if (event->type_name == "error") {
			obs_log(LOG_ERROR, "session event error error_type=%s error_code=%s error_param=%s",
				event->error_type.c_str(), event->error_code.c_str(), event->error_param.c_str());
			// OpenAI は無効なキーでもハンドシェイクを HTTP 101 で通し、この error を送ってから閉じる
			if (event->error_code == "invalid_api_key")
				fail(SessionError::AuthFailed);
			// session.updated より前のほかの error は設定が受け付けられなかったことを示し、続けても字幕が出ないため止める
			else if (!session_updated_)
				fail(SessionError::SessionRejected);
		}
		return;
	}
	if (event->type == TranscriptionEvent::Type::Failed)
		obs_log(LOG_WARNING, "session transcription failed error_type=%s", event->error_type.c_str());

	const TurnError error = router_->handle(*event);
	if (error != TurnError::None) {
		++router_errors_;
		obs_log(LOG_ERROR, "session router error=%s event=%s", turn_error_name(error),
			event->type_name.c_str());
	}
}

void RealtimeSession::log_metrics()
{
	const double elapsed = elapsed_seconds();
	const double produced = static_cast<double>(produced_samples_) / PcmConverter::kOutputRate;
	obs_log(LOG_INFO,
		"session metrics elapsed_s=%.1f produced_s=%.2f queued=%zu dropped_frames=%llu muted_callbacks=%llu "
		"sent_chunks=%llu caption_updates=%llu completed=%llu router_errors=%llu input_tokens=%lld "
		"output_tokens=%lld",
		elapsed, produced, capture_.queued(), static_cast<unsigned long long>(capture_.dropped_frames()),
		static_cast<unsigned long long>(capture_.muted_callbacks()),
		static_cast<unsigned long long>(sent_chunks_.load()), static_cast<unsigned long long>(caption_updates_),
		static_cast<unsigned long long>(completed_turns_), static_cast<unsigned long long>(router_errors_),
		input_tokens_.load(), output_tokens_.load());
	last_metrics_ = Clock::now();
}

} // namespace rts
