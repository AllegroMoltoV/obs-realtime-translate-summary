#pragma once

#include "audio-capture.h"
#include "core/caption-router.h"
#include "core/text-pipeline.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace rts {

class OpenAiTextGenerator;

// 段階 1 から段階 3 の検証に使う起動の経路。段階 4 でドックからの開始と停止に置き換える。
// ログには本文、音声、キーを書かず、イベントの種類、件数、時刻、文字数だけを書く
class VerifySession {
public:
	enum class Mode { AudioOnly, Full };

	VerifySession();
	~VerifySession();

	VerifySession(const VerifySession &) = delete;
	VerifySession &operator=(const VerifySession &) = delete;

	bool start(const std::string &source_name, const std::string &api_key, Mode mode);
	void stop();
	bool running() const { return running_.load(); }

private:
	using Clock = std::chrono::steady_clock;

	void network_loop();
	void handle_message(const std::string &message);
	void log_metrics();

	AudioCapture capture_;
	std::string source_name_;
	std::string api_key_;
	Mode mode_ = Mode::AudioOnly;

	std::atomic<bool> running_{false};
	std::thread network_thread_;

	std::unique_ptr<OpenAiTextGenerator> generator_;
	std::unique_ptr<TextPipeline> pipeline_;
	std::unique_ptr<CaptionRouter> router_;

	// 通信のスレッドだけが触る
	Clock::time_point started_{};
	Clock::time_point last_metrics_{};
	uint64_t produced_samples_ = 0;
	uint64_t sent_chunks_ = 0;
	uint64_t caption_updates_ = 0;
	uint64_t completed_turns_ = 0;
	uint64_t router_errors_ = 0;
	uint64_t total_frames_at_start_ = 0;
	uint64_t lagged_frames_at_start_ = 0;
	bool network_alive_ = false;
};

} // namespace rts
