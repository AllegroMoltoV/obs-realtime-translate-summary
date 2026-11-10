#include "verify-session.h"

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
constexpr int kSummaryMaxChars = 30;

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

} // namespace

VerifySession::VerifySession() = default;

VerifySession::~VerifySession()
{
	stop();
}

bool VerifySession::start(const std::string &source_name, const std::string &api_key, Mode mode)
{
	if (running_.load())
		return false;
	stop();

	source_name_ = source_name;
	api_key_ = api_key;
	mode_ = mode;
	produced_samples_ = sent_chunks_ = caption_updates_ = completed_turns_ = router_errors_ = 0;

	if (mode_ == Mode::Full) {
		generator_ = std::make_unique<OpenAiTextGenerator>(api_key_, ModelProfile{}, kSummaryMaxChars);
		TextPipeline::Options options;
		options.summary_max_chars = kSummaryMaxChars;
		pipeline_ = std::make_unique<TextPipeline>(
			*generator_, options,
			[](const std::string &english) {
				obs_log(LOG_INFO, "VERIFY english chars=%zu", codepoint_length(english));
			},
			[](const DisplayLines &lines) {
				obs_log(LOG_INFO, "VERIFY summary line_chars=%zu,%zu,%zu", codepoint_length(lines[0]),
					codepoint_length(lines[1]), codepoint_length(lines[2]));
			});
		router_ = std::make_unique<CaptionRouter>([this](const std::string &) { ++caption_updates_; },
							  [this](const CompletedTurn &turn) {
								  ++completed_turns_;
								  pipeline_->submit_turn(turn);
							  });
		pipeline_->start();
	}

	running_.store(true);
	network_thread_ = std::thread(&VerifySession::network_loop, this);
	return true;
}

void VerifySession::stop()
{
	running_.store(false);
	const bool had_thread = network_thread_.joinable();
	if (had_thread)
		network_thread_.join();
	if (pipeline_) {
		const auto c = pipeline_->counters();
		pipeline_->stop();
		obs_log(LOG_INFO,
			"VERIFY pipeline translations_ok=%d failed=%d stale=%d summaries_ok=%d failed=%d invalid=%d "
			"exceptions=%d",
			c.translations_succeeded, c.translations_failed, c.translations_discarded_as_stale,
			c.summaries_succeeded, c.summaries_failed, c.summaries_invalid, c.worker_exceptions);
	}
	router_.reset();
	pipeline_.reset();
	generator_.reset();
	if (had_thread)
		obs_log(LOG_INFO, "VERIFY stopped");
}

void VerifySession::network_loop()
{
	// 例外がスレッドの外へ出ると std::terminate で OBS ごと落ちるため、ここで捕捉する
	try {
		PcmConverter converter;
		const uint32_t input_rate = audio_output_get_sample_rate(obs_get_audio());
		if (!converter.open(input_rate)) {
			obs_log(LOG_ERROR, "VERIFY resampler open failed input_rate=%u", input_rate);
			running_.store(false);
			return;
		}

		RealtimeClient client;
		std::string error;
		if (mode_ == Mode::Full) {
			if (!client.connect(kRealtimeUrl, api_key_, error)) {
				obs_log(LOG_ERROR, "VERIFY realtime connect failed %s", error.c_str());
				running_.store(false);
				return;
			}
			if (!client.send_text(session_update({}).dump(), error)) {
				obs_log(LOG_ERROR, "VERIFY session.update send failed %s", error.c_str());
				running_.store(false);
				return;
			}
			network_alive_ = true;
		}

		if (!capture_.attach(source_name_)) {
			obs_log(LOG_ERROR, "VERIFY audio source not found or has no audio");
			running_.store(false);
			return;
		}

		started_ = Clock::now();
		last_metrics_ = started_;
		total_frames_at_start_ = obs_get_total_frames();
		lagged_frames_at_start_ = obs_get_lagged_frames();
		obs_log(LOG_INFO, "VERIFY started mode=%s input_rate=%u", mode_ == Mode::Full ? "full" : "audio",
			input_rate);

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
						obs_log(LOG_ERROR, "VERIFY audio send failed %s", error.c_str());
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
				obs_log(LOG_ERROR, "VERIFY realtime %s close_code=%d elapsed_s=%.1f detail=%s",
					result == RealtimeClient::RecvResult::Closed ? "closed" : "error",
					client.close_code(),
					std::chrono::duration<double>(Clock::now() - started_).count(), error.c_str());
				network_alive_ = false;
			}

			if (Clock::now() - last_metrics_ >= std::chrono::seconds(10))
				log_metrics();

			if (!worked)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}

		capture_.detach();
		log_metrics();
		client.close();
	} catch (const std::exception &) {
		capture_.detach();
		obs_log(LOG_ERROR, "VERIFY network thread exception");
		running_.store(false);
	} catch (...) {
		capture_.detach();
		obs_log(LOG_ERROR, "VERIFY network thread unknown exception");
		running_.store(false);
	}
}

void VerifySession::handle_message(const std::string &message)
{
	const auto event = parse_transcription_event(message);
	if (!event) {
		obs_log(LOG_WARNING, "VERIFY event invalid json bytes=%zu", message.size());
		return;
	}

	if (event->type == TranscriptionEvent::Type::Other) {
		if (!event->error_type.empty())
			obs_log(LOG_ERROR, "VERIFY event %s error_type=%s error_code=%s error_param=%s",
				event->type_name.c_str(), event->error_type.c_str(), event->error_code.c_str(),
				event->error_param.c_str());
		else
			obs_log(LOG_INFO, "VERIFY event %s", event->type_name.c_str());
		return;
	}
	if (event->type == TranscriptionEvent::Type::Completed)
		obs_log(LOG_INFO, "VERIFY event completed chars=%zu",
			codepoint_length(trim_unicode(event->transcript)));
	if (event->type == TranscriptionEvent::Type::Failed)
		obs_log(LOG_WARNING, "VERIFY event transcription failed error_type=%s", event->error_type.c_str());

	if (!router_)
		return;
	const TurnError error = router_->handle(*event);
	if (error != TurnError::None) {
		++router_errors_;
		obs_log(LOG_ERROR, "VERIFY router error=%s event=%s", turn_error_name(error), event->type_name.c_str());
	}
}

void VerifySession::log_metrics()
{
	const auto now = Clock::now();
	const double elapsed = std::chrono::duration<double>(now - started_).count();
	const double produced = static_cast<double>(produced_samples_) / PcmConverter::kOutputRate;
	obs_log(LOG_INFO,
		"VERIFY metrics elapsed_s=%.1f produced_s=%.2f ratio=%.4f queued=%zu dropped_frames=%llu "
		"muted_callbacks=%llu sent_chunks=%llu caption_updates=%llu completed=%llu router_errors=%llu "
		"total_frames=%llu lagged_frames=%llu",
		elapsed, produced, elapsed > 0 ? produced / elapsed : 0.0, capture_.queued(),
		static_cast<unsigned long long>(capture_.dropped_frames()),
		static_cast<unsigned long long>(capture_.muted_callbacks()),
		static_cast<unsigned long long>(sent_chunks_), static_cast<unsigned long long>(caption_updates_),
		static_cast<unsigned long long>(completed_turns_), static_cast<unsigned long long>(router_errors_),
		static_cast<unsigned long long>(obs_get_total_frames() - total_frames_at_start_),
		static_cast<unsigned long long>(obs_get_lagged_frames() - lagged_frames_at_start_));
	last_metrics_ = now;
}

} // namespace rts
