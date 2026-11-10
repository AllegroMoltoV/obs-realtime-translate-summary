#include "pcm-converter.h"

namespace rts {

PcmConverter::~PcmConverter()
{
	close();
}

bool PcmConverter::open(uint32_t input_rate)
{
	close();

	resample_info src = {};
	src.samples_per_sec = input_rate;
	src.format = AUDIO_FORMAT_FLOAT;
	src.speakers = SPEAKERS_MONO;

	resample_info dst = {};
	dst.samples_per_sec = kOutputRate;
	dst.format = AUDIO_FORMAT_16BIT;
	dst.speakers = SPEAKERS_MONO;

	resampler_ = audio_resampler_create(&dst, &src);
	return resampler_ != nullptr;
}

void PcmConverter::close()
{
	if (resampler_) {
		audio_resampler_destroy(resampler_);
		resampler_ = nullptr;
	}
}

bool PcmConverter::convert(const float *samples, uint32_t frames, std::vector<int16_t> &out)
{
	if (!resampler_ || frames == 0)
		return false;

	const uint8_t *input[MAX_AV_PLANES] = {reinterpret_cast<const uint8_t *>(samples)};
	uint8_t *output[MAX_AV_PLANES] = {};
	uint32_t out_frames = 0;
	uint64_t ts_offset = 0;
	if (!audio_resampler_resample(resampler_, output, &out_frames, &ts_offset, input, frames))
		return false;

	const auto *pcm = reinterpret_cast<const int16_t *>(output[0]);
	out.insert(out.end(), pcm, pcm + out_frames);
	return true;
}

} // namespace rts
