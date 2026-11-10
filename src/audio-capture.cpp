#include "audio-capture.h"

#include <plugin-support.h>

#include <algorithm>

namespace rts {

AudioCapture::AudioCapture() : queue_(std::make_unique<SPSCQueue<MonoFrame, kQueueCapacity>>()) {}

AudioCapture::~AudioCapture()
{
	detach();
}

bool AudioCapture::attach(const std::string &source_name)
{
	detach();

	obs_source_t *source = obs_get_source_by_name(source_name.c_str());
	if (!source)
		return false;

	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0) {
		obs_source_release(source);
		return false;
	}

	channels_ = audio_output_get_channels(obs_get_audio());
	queue_->clear();
	dropped_.store(0, std::memory_order_relaxed);
	muted_.store(0, std::memory_order_relaxed);
	muted_now_.store(false, std::memory_order_relaxed);

	weak_source_ = obs_source_get_weak_source(source);
	obs_source_add_audio_capture_callback(source, &AudioCapture::on_audio, this);
	obs_source_release(source);
	return true;
}

void AudioCapture::detach()
{
	if (!weak_source_)
		return;

	obs_source_t *source = obs_weak_source_get_source(weak_source_);
	if (source) {
		obs_source_remove_audio_capture_callback(source, &AudioCapture::on_audio, this);
		obs_source_release(source);
	}
	obs_weak_source_release(weak_source_);
	weak_source_ = nullptr;
}

void AudioCapture::on_audio(void *param, obs_source_t *, const struct audio_data *audio, bool muted)
{
	auto *self = static_cast<AudioCapture *>(param);
	self->muted_now_.store(muted, std::memory_order_relaxed);
	if (muted) {
		// ミュート中の音声は外部へ送らない
		self->muted_.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	self->push_audio(audio);
}

void AudioCapture::push_audio(const struct audio_data *audio)
{
	size_t active_channels = 0;
	for (size_t ch = 0; ch < channels_ && ch < MAX_AV_PLANES; ++ch) {
		if (audio->data[ch])
			++active_channels;
	}
	if (active_channels == 0)
		return;

	const float scale = 1.0f / static_cast<float>(active_channels);
	uint32_t offset = 0;
	while (offset < audio->frames) {
		MonoFrame frame;
		frame.frames = std::min<uint32_t>(MonoFrame::kMaxFrames, audio->frames - offset);
		for (uint32_t i = 0; i < frame.frames; ++i) {
			float sum = 0.0f;
			for (size_t ch = 0; ch < channels_ && ch < MAX_AV_PLANES; ++ch) {
				const auto *plane = reinterpret_cast<const float *>(audio->data[ch]);
				if (plane)
					sum += plane[offset + i];
			}
			frame.samples[i] = sum * scale;
		}
		if (!queue_->try_push(frame))
			dropped_.fetch_add(frame.frames, std::memory_order_relaxed);
		offset += frame.frames;
	}
}

} // namespace rts
