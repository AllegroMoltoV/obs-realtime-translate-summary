#pragma once

#include "spsc-queue.h"

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace rts {

// OBS の 1 回の音声出力の最大フレーム数 (AUDIO_OUTPUT_FRAMES) に合わせた、モノラルの音声の枠
struct MonoFrame {
	static constexpr uint32_t kMaxFrames = 1024;
	uint32_t frames = 0;
	float samples[kMaxFrames];
};

// 選んだ音声のソースから、フィルター適用後・フェーダー適用前の音声を受け取る。
// 音声のスレッドで呼ばれるコールバックでは、モノラルへ平均して枠へ写し、キューへ入れるだけにする
class AudioCapture {
public:
	static constexpr size_t kQueueCapacity = 512; // 48 kHz で約 10 秒

	AudioCapture();
	~AudioCapture();

	AudioCapture(const AudioCapture &) = delete;
	AudioCapture &operator=(const AudioCapture &) = delete;

	bool attach(const std::string &source_name);
	void detach();

	bool pop(MonoFrame &frame) { return queue_->try_pop(frame); }

	uint64_t dropped_frames() const { return dropped_.load(std::memory_order_relaxed); }
	uint64_t muted_callbacks() const { return muted_.load(std::memory_order_relaxed); }
	size_t queued() const { return queue_->size_approx(); }

private:
	static void on_audio(void *param, obs_source_t *source, const struct audio_data *audio, bool muted);
	void push_audio(const struct audio_data *audio);

	obs_weak_source_t *weak_source_ = nullptr;
	size_t channels_ = 0;
	std::unique_ptr<SPSCQueue<MonoFrame, kQueueCapacity>> queue_;
	std::atomic<uint64_t> dropped_{0};
	std::atomic<uint64_t> muted_{0};
};

} // namespace rts
