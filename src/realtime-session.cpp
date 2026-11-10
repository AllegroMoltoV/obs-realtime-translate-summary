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
#include <random>

using json = nlohmann::json;

namespace rts {

namespace {

constexpr const char *kRealtimeUrl = "wss://api.openai.com/v1/realtime?intent=transcription";
constexpr size_t kChunkSamples = PcmConverter::kOutputRate / 10; // 100 ms
// 元のツールの websockets の ping_interval と ping_timeout と同じ値
constexpr auto kPingInterval = std::chrono::seconds(20);
constexpr auto kPingTimeout = std::chrono::seconds(20);
// error イベントのあとこの時間内に閉じた接続は、サーバー側の終了とみなして再接続する
constexpr auto kErrorCloseWindow = std::chrono::seconds(5);

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

double seconds_since(std::chrono::steady_clock::time_point since)
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
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
	retry_attempt_ = 0;
	produced_samples_ = caption_updates_ = completed_turns_ = router_errors_ = 0;
	first_ping_logged_ = first_pong_logged_ = false;
	last_metrics_ = Clock::now();
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

size_t RealtimeSession::drain_audio()
{
	size_t frames = 0;
	MonoFrame frame;
	while (capture_.pop(frame))
		frames += frame.frames;
	return frames;
}

bool RealtimeSession::wait_before_retry(int attempt, const RetryPolicy &policy, double jitter)
{
	const double delay = policy.delay_seconds(attempt, jitter);
	obs_log(LOG_INFO, "session reconnect scheduled attempt=%d/%d delay_s=%.2f", attempt, policy.max_retries, delay);
	const auto until =
		Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(delay));
	// 停止の要求をすぐ受け付けるよう、短い間隔で確かめながら待つ
	while (running_.load() && Clock::now() < until)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	return running_.load();
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

		RetryCounter counter;
		std::mt19937 rng(std::random_device{}());
		std::uniform_real_distribution<double> jitter(0.0, 1.0);
		bool attached = false;
		std::vector<int16_t> pcm;

		while (running_.load()) {
			RealtimeClient client;
			std::string error;
			const bool connected = client.connect(kRealtimeUrl, config_.api_key, error, &running_);
			if (!running_.load())
				break;

			ConnectionEnd end = ConnectionEnd::Lost;
			double connected_seconds = 0.0;
			if (!connected) {
				const long status = client.last_http_status();
				obs_log(LOG_ERROR, "session realtime connect failed %s", error.c_str());
				if (status == 401 || status == 403) {
					fail(SessionError::AuthFailed);
					break;
				}
			} else if (!client.send_text(session_update(config_.keywords).dump(), error)) {
				obs_log(LOG_ERROR, "session session.update send failed %s", error.c_str());
			} else {
				if (!attached) {
					if (!capture_.attach(config_.audio_source)) {
						obs_log(LOG_ERROR, "session audio source not found or has no audio");
						client.close();
						fail(SessionError::AudioSourceMissing);
						break;
					}
					attached = true;
				}
				// 待機と接続の間にたまった音声は古いため、接続に成功した直後に捨てる
				const size_t discarded = drain_audio();
				pcm.clear();
				if (discarded > 0)
					obs_log(LOG_INFO, "session discarded stale audio frames=%zu", discarded);

				state_ = SessionState::Running;
				retry_attempt_ = 0;
				obs_log(LOG_INFO, "session connected input_rate=%u keywords=%zu retries=%d", input_rate,
					config_.keywords.size(), counter.retries());
				const auto connected_at = Clock::now();
				end = run_connection(client, converter, pcm);
				connected_seconds = seconds_since(connected_at);
				obs_log(LOG_INFO, "session connection ended reason=%s connected_s=%.1f pongs=%llu",
					end == ConnectionEnd::Stopped ? "stopped"
					: end == ConnectionEnd::Fatal ? "fatal"
								      : "lost",
					connected_seconds, client.pongs_received());
			}
			client.close();
			if (end != ConnectionEnd::Lost || !running_.load())
				break;

			// 前の接続の未完了の発話と認識途中の字幕は捨てる。英訳と概要の処理中のものは続ける
			const auto reset = router_->reset_session();
			obs_log(LOG_INFO, "session reset pending_items=%d partial_captions=%d", reset.pending_items,
				reset.partial_captions);

			if (!counter.on_disconnect(connected_seconds)) {
				obs_log(LOG_ERROR, "session reconnect exhausted retries=%d", counter.retries());
				fail(SessionError::ConnectionLost);
				break;
			}
			retry_attempt_ = counter.retries();
			state_ = SessionState::Reconnecting;
			if (!wait_before_retry(counter.retries(), counter.policy(), jitter(rng)))
				break;
		}

		if (attached)
			capture_.detach();
		log_metrics();
	} catch (...) {
		capture_.detach();
		obs_log(LOG_ERROR, "session network thread exception");
		fail(SessionError::Internal);
	}
}

RealtimeSession::ConnectionEnd RealtimeSession::run_connection(RealtimeClient &client, PcmConverter &converter,
							       std::vector<int16_t> &pcm)
{
	session_updated_ = false;
	error_event_pending_ = false;
	auto last_ping = Clock::now();
	bool ping_outstanding = false;
	std::string error;
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
			const std::string audio = base64_encode(reinterpret_cast<const uint8_t *>(pcm.data()),
								kChunkSamples * sizeof(int16_t));
			const json append = {{"type", "input_audio_buffer.append"}, {"audio", audio}};
			if (!client.send_text(append.dump(), error)) {
				obs_log(LOG_ERROR, "session audio send failed %s", error.c_str());
				return ConnectionEnd::Lost;
			}
			++sent_chunks_;
			pcm.erase(pcm.begin(), pcm.begin() + kChunkSamples);
		}

		for (;;) {
			std::string message;
			const auto result = client.receive(message, error);
			if (result == RealtimeClient::RecvResult::Nothing)
				break;
			worked = true;
			if (result == RealtimeClient::RecvResult::Message) {
				handle_message(message);
				if (!running_.load())
					return ConnectionEnd::Fatal;
				continue;
			}
			obs_log(LOG_ERROR, "session realtime %s close_code=%d detail=%s after_error=%d",
				result == RealtimeClient::RecvResult::Closed ? "closed" : "error", client.close_code(),
				error.c_str(), error_event_pending_ ? 1 : 0);
			return ConnectionEnd::Lost;
		}

		// error イベントのあと閉じられないまま期限を過ぎたら、元のツールと同じく再試行せず停止する
		if (error_event_pending_ && Clock::now() - error_event_at_ > kErrorCloseWindow) {
			obs_log(LOG_ERROR, "session stopping after api error without close");
			fail(SessionError::ApiError);
			return ConnectionEnd::Fatal;
		}

		// ping の応答の途絶で切断を検知する。PONG を一度も受け取っていない接続では、
		// libcurl やサーバーが PONG を返さない場合の誤検知を避けるため判定しない
		const auto now = Clock::now();
		if (ping_outstanding && client.pongs_received() > 0 && client.last_pong() < last_ping &&
		    now - last_ping > kPingTimeout) {
			obs_log(LOG_ERROR, "session ping timeout");
			return ConnectionEnd::Lost;
		}
		if (now - last_ping >= kPingInterval) {
			const unsigned long long pongs_before = client.pongs_received();
			if (!client.send_ping(error)) {
				obs_log(LOG_ERROR, "session ping send failed %s", error.c_str());
				return ConnectionEnd::Lost;
			}
			if (pongs_before == 0 && !first_ping_logged_) {
				obs_log(LOG_INFO, "session first ping sent");
				first_ping_logged_ = true;
			}
			last_ping = now;
			ping_outstanding = true;
		}
		if (client.pongs_received() == 1 && !first_pong_logged_) {
			obs_log(LOG_INFO, "session first pong received");
			first_pong_logged_ = true;
		}

		if (Clock::now() - last_metrics_ >= std::chrono::seconds(60))
			log_metrics();

		if (!worked)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return ConnectionEnd::Stopped;
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
			if (event->error_code == "invalid_api_key") {
				fail(SessionError::AuthFailed);
			} else if (!session_updated_) {
				// session.updated より前のほかの error は設定が受け付けられなかったことを示し、再接続しても同じになる
				fail(SessionError::SessionRejected);
			} else {
				// セッションの時間の上限などで閉じられる前触れの可能性があるため、閉じられるかを少し待ってから判断する
				error_event_pending_ = true;
				error_event_at_ = Clock::now();
			}
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
