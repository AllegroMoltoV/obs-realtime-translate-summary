#pragma once

#include <media-io/audio-resampler.h>

#include <cstdint>
#include <vector>

namespace rts {

// OBS の標本化周波数のモノラルの浮動小数を、OpenAI へ送る 24 kHz、モノラル、PCM16 に変換する
class PcmConverter {
public:
	static constexpr uint32_t kOutputRate = 24000;

	PcmConverter() = default;
	~PcmConverter();

	PcmConverter(const PcmConverter &) = delete;
	PcmConverter &operator=(const PcmConverter &) = delete;

	bool open(uint32_t input_rate);
	void close();

	// 変換した標本を out の末尾に足す
	bool convert(const float *samples, uint32_t frames, std::vector<int16_t> &out);

private:
	audio_resampler_t *resampler_ = nullptr;
};

} // namespace rts
