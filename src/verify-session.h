#pragma once

#include "audio-capture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rts {

// 段階 1 の前提の検証だけに使う。段階 4 でドックからの開始と停止に置き換える。
// ログには本文、音声、キーを書かず、イベントの種類、件数、時刻、文字数だけを書く
class VerifySession {
public:
	enum class Mode { AudioOnly, Full };

	VerifySession() = default;
	~VerifySession();

	VerifySession(const VerifySession &) = delete;
	VerifySession &operator=(const VerifySession &) = delete;

	bool start(const std::string &source_name, const std::string &api_key, Mode mode);
	void stop();
	bool running() const { return running_.load(); }

private:
	using Clock = std::chrono::steady_clock;

	void network_loop();
	void text_loop();
	void handle_event(const std::string &message);
	void log_metrics();
	void translate(const std::string &transcript);
	void summarize();

	AudioCapture capture_;
	std::string source_name_;
	std::string api_key_;
	Mode mode_ = Mode::AudioOnly;

	std::atomic<bool> running_{false};
	std::thread network_thread_;
	std::thread text_thread_;

	// 通信のスレッドだけが触る
	Clock::time_point started_{};
	Clock::time_point last_metrics_{};
	uint64_t produced_samples_ = 0;
	uint64_t sent_chunks_ = 0;
	uint64_t delta_events_ = 0;
	uint64_t completed_events_ = 0;
	uint64_t total_frames_at_start_ = 0;
	uint64_t lagged_frames_at_start_ = 0;
	bool network_alive_ = false;

	// 確定した発話を英訳と概要のスレッドへ渡す
	std::mutex text_mutex_;
	std::condition_variable text_cv_;
	std::deque<std::string> pending_transcripts_;
	std::vector<std::pair<Clock::time_point, std::string>> recent_transcripts_;
	std::atomic<bool> omit_temperature_{false};
};

} // namespace rts
