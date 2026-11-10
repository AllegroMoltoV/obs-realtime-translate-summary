#pragma once

#include "audio-capture.h"
#include "core/caption-router.h"
#include "core/text-pipeline.h"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace rts {

class OpenAiTextGenerator;

struct SessionConfig {
	std::string audio_source;
	std::string api_key;
	std::vector<std::string> keywords;
	int summary_max_chars = 30;
	double summary_interval_seconds = 10.0;
};

enum class SessionState { Stopped, Connecting, Running, Error };

enum class SessionError {
	None,
	ConnectFailed,
	AuthFailed,
	SessionRejected,
	AudioSourceMissing,
	ConnectionLost,
	Internal
};

// 音声の取り出し、文字起こしの接続、英訳と概要を束ねる。
// 出力は作業のスレッドから on_output で返す。表示の更新は呼び出し側がメインスレッドで行う。
// ログには本文、音声、キーを書かず、イベントの種類、件数、時刻、文字数だけを書く
class RealtimeSession {
public:
	enum Output { Japanese = 0, English = 1, Summary = 2 };
	using OutputFn = std::function<void(Output output, const std::string &text)>;

	RealtimeSession();
	~RealtimeSession();

	RealtimeSession(const RealtimeSession &) = delete;
	RealtimeSession &operator=(const RealtimeSession &) = delete;

	bool start(const SessionConfig &config, OutputFn on_output);
	// 通信と作業のスレッドを合流させる。戻った後は on_output が呼ばれない
	void stop();

	SessionState state() const { return state_.load(); }
	SessionError error() const { return error_.load(); }
	bool muted() const { return capture_.muted_now(); }
	double sent_audio_seconds() const;
	long long input_tokens() const { return input_tokens_.load(); }
	long long output_tokens() const { return output_tokens_.load(); }
	double elapsed_seconds() const;
	// 英訳と概要の失敗の件数。ドックの警告に使う
	int text_failures() const { return text_failures_.load(); }

private:
	using Clock = std::chrono::steady_clock;

	void network_loop();
	void handle_message(const std::string &message);
	void fail(SessionError error);
	void log_metrics();

	AudioCapture capture_;
	SessionConfig config_;
	OutputFn on_output_;

	std::atomic<bool> running_{false};
	std::atomic<SessionState> state_{SessionState::Stopped};
	std::atomic<SessionError> error_{SessionError::None};
	std::thread network_thread_;

	std::unique_ptr<OpenAiTextGenerator> generator_;
	std::unique_ptr<TextPipeline> pipeline_;
	std::unique_ptr<CaptionRouter> router_;

	std::atomic<uint64_t> sent_chunks_{0};
	std::atomic<long long> input_tokens_{0};
	std::atomic<long long> output_tokens_{0};
	std::atomic<int64_t> started_ns_{0};
	std::atomic<int> text_failures_{0};

	// 通信のスレッドだけが触る
	Clock::time_point last_metrics_{};
	uint64_t produced_samples_ = 0;
	uint64_t caption_updates_ = 0;
	uint64_t completed_turns_ = 0;
	uint64_t router_errors_ = 0;
	bool network_alive_ = false;
	bool session_updated_ = false;
};

} // namespace rts
